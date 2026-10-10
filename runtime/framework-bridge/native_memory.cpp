#include "framework-bridge/native_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace aa::bridge {

namespace {

constexpr uint64_t kTag = 0x5;
constexpr uint32_t kMaxRegions = 1u << 20;
constexpr uint64_t kMaxRegionSize = 1ull << 31;

struct Region {
  uint8_t gen = 1;
  bool live = false;
  bool writable = true;
  std::vector<uint8_t> bytes;          // owned regions
  rt::Handle array = rt::kNull;        // regions over a Java array
  rt::ArrayObject* object = nullptr;   // identity of that array
};

struct Memory {
  std::mutex lock;
  std::vector<Region> regions;
  std::vector<uint32_t> free;
  std::unordered_map<rt::Handle, uint32_t> by_array;
};

Memory& memory() {
  static Memory m;
  return m;
}

int64_t encode(uint32_t id, uint8_t gen, uint32_t offset) {
  return int64_t((kTag << 60) | (uint64_t(gen) << 52) | (uint64_t(id) << 32) | offset);
}

struct Decoded {
  uint32_t id;
  uint8_t gen;
  uint32_t offset;
};

std::optional<Decoded> decode(int64_t address) {
  uint64_t v = uint64_t(address);
  if ((v >> 60) != kTag) return std::nullopt;
  return Decoded{uint32_t((v >> 32) & 0xfffff), uint8_t(v >> 52), uint32_t(v)};
}

bool array_alive(rt::Runtime& rt, const Region& r) {
  return rt.is_valid_handle(r.array) && rt.object(r.array) == r.object;
}

void release(Memory& m, uint32_t id) {
  Region& r = m.regions[id];
  if (r.array != rt::kNull) m.by_array.erase(r.array);
  r = Region{uint8_t(r.gen + 1)};
  if (r.gen == 0) r.gen = 1;
  m.free.push_back(id);
}

// A free slot (caller holds the lock); dead array regions are reclaimed
// when the table is full.
std::optional<uint32_t> take_slot(rt::Runtime& rt, Memory& m) {
  if (m.free.empty() && m.regions.size() >= kMaxRegions) {
    for (uint32_t id = 0; id < m.regions.size(); ++id) {
      const Region& r = m.regions[id];
      if (r.live && r.array != rt::kNull && !array_alive(rt, r)) release(m, id);
    }
  }
  if (!m.free.empty()) {
    uint32_t id = m.free.back();
    m.free.pop_back();
    return id;
  }
  if (m.regions.size() >= kMaxRegions) return std::nullopt;
  m.regions.emplace_back();
  return uint32_t(m.regions.size() - 1);
}

[[noreturn]] void unknown(rt::Runtime& rt, int64_t address) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(address));
  rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("adresse native inconnue : ") + buf);
}

// The whole region behind `address` and the offset in it, or an exception.
std::pair<std::span<uint8_t>, uint32_t> region_of(rt::Runtime& rt, int64_t address, bool write) {
  auto d = decode(address);
  if (!d) unknown(rt, address);
  Memory& m = memory();
  std::span<uint8_t> data;
  bool ok = false, read_only = false;
  {
    std::lock_guard<std::mutex> guard(m.lock);
    if (d->id < m.regions.size()) {
      Region& r = m.regions[d->id];
      if (r.live && r.gen == d->gen) {
        if (r.array != rt::kNull) {
          if (array_alive(rt, r)) {
            data = std::span<uint8_t>(r.object->data);
            ok = true;
          }
        } else {
          data = std::span<uint8_t>(r.bytes);
          ok = true;
        }
        read_only = !r.writable;
      }
    }
  }
  if (!ok) unknown(rt, address);
  if (write && read_only) rt.throw_new("Ljava/nio/ReadOnlyBufferException;", "");
  return {data, d->offset};
}

}  // namespace

bool is_native_address(int64_t address) { return decode(address).has_value(); }

int64_t native_alloc(rt::Runtime& rt, uint64_t size) {
  if (size > kMaxRegionSize) rt.throw_new("Ljava/lang/OutOfMemoryError;", "allocateMemory : taille trop grande");
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  auto id = take_slot(rt, m);
  if (!id) rt.throw_new("Ljava/lang/OutOfMemoryError;", "trop de blocs de mémoire native");
  Region& r = m.regions[*id];
  r.live = true;
  r.bytes.assign(size_t(size), 0);
  return encode(*id, r.gen, 0);
}

void native_free(rt::Runtime& rt, int64_t address) {
  if (address == 0) return;
  auto d = decode(address);
  bool ok = false;
  if (d && d->offset == 0) {
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    if (d->id < m.regions.size()) {
      Region& r = m.regions[d->id];
      if (r.live && r.gen == d->gen && r.array == rt::kNull) {
        release(m, d->id);
        ok = true;
      }
    }
  }
  if (!ok) unknown(rt, address);
}

int64_t native_realloc(rt::Runtime& rt, int64_t address, uint64_t size) {
  if (address == 0) return size ? native_alloc(rt, size) : 0;
  if (size == 0) {
    native_free(rt, address);
    return 0;
  }
  auto [old, offset] = region_of(rt, address, false);
  if (offset != 0) unknown(rt, address);
  std::vector<uint8_t> copy(old.begin(), old.begin() + std::min<size_t>(old.size(), size_t(size)));
  int64_t fresh = native_alloc(rt, size);
  if (!copy.empty()) std::memcpy(native_span(rt, fresh, copy.size(), true).data(), copy.data(), copy.size());
  native_free(rt, address);
  return fresh;
}

int64_t native_address_of(rt::Runtime& rt, rt::Handle array) {
  auto* a = dynamic_cast<rt::ArrayObject*>(rt.object(array));
  if (!a || !a->klass || !a->klass->component || a->klass->component->primitive == 0) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "addressOf : tableau primitif attendu");
  }
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  auto known = m.by_array.find(array);
  if (known != m.by_array.end()) {
    Region& r = m.regions[known->second];
    if (r.live && r.object == a) return encode(known->second, r.gen, 0);
    release(m, known->second);
  }
  auto id = take_slot(rt, m);
  if (!id) rt.throw_new("Ljava/lang/OutOfMemoryError;", "trop de blocs de mémoire native");
  Region& r = m.regions[*id];
  r.live = true;
  r.array = array;
  r.object = a;
  m.by_array[array] = *id;
  return encode(*id, r.gen, 0);
}

int64_t native_wrap(rt::Runtime& rt, std::vector<uint8_t> data, bool writable) {
  if (data.size() > kMaxRegionSize) rt.throw_new("Ljava/lang/OutOfMemoryError;", "bloc trop grand");
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  auto id = take_slot(rt, m);
  if (!id) rt.throw_new("Ljava/lang/OutOfMemoryError;", "trop de blocs de mémoire native");
  Region& r = m.regions[*id];
  r.live = true;
  r.writable = writable;
  r.bytes = std::move(data);
  return encode(*id, r.gen, 0);
}

std::span<uint8_t> native_span(rt::Runtime& rt, int64_t address, uint64_t size, bool write) {
  auto [data, offset] = region_of(rt, address, write);
  if (offset > data.size() || size > data.size() - offset) {
    rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "accès hors du bloc de mémoire native");
  }
  return data.subspan(offset, size_t(size));
}

std::string native_cstring(rt::Runtime& rt, int64_t address, size_t max) {
  auto [data, offset] = region_of(rt, address, false);
  if (offset > data.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "chaîne hors du bloc");
  auto rest = data.subspan(offset);
  size_t n = std::min(rest.size(), max);
  auto end = std::find(rest.begin(), rest.begin() + n, uint8_t(0));
  if (end == rest.begin() + n) rt.throw_new("Ljava/lang/IllegalArgumentException;", "chaîne native sans fin");
  return std::string(rest.begin(), end);
}

rt::ArrayObject& native_array(rt::Runtime& rt, int64_t address) {
  auto d = decode(address);
  if (!d || d->offset != 0) unknown(rt, address);
  Memory& m = memory();
  rt::ArrayObject* a = nullptr;
  {
    std::lock_guard<std::mutex> guard(m.lock);
    if (d->id < m.regions.size()) {
      Region& r = m.regions[d->id];
      if (r.live && r.gen == d->gen && r.array != rt::kNull && array_alive(rt, r)) a = r.object;
    }
  }
  if (!a) unknown(rt, address);
  return *a;
}

}  // namespace aa::bridge
