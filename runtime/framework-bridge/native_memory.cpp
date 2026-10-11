#include "framework-bridge/native_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

namespace aa::bridge {

namespace {

constexpr uint64_t kTag = 0x5;
constexpr uint32_t kMaxRegions = 1u << 20;
constexpr uint64_t kMaxRegionSize = 1ull << 31;
constexpr uint64_t kDefaultLimit = 1ull << 30;
// Freed slots wait in a FIFO and are reused only once this many are free, so
// that a slot (and its 8-bit generation) comes back after thousands of other
// allocations: a stale address names a live region again only after about
// kQuarantine * 255 allocations, not 255.
constexpr uint32_t kQuarantine = 4096;
constexpr uint32_t kNoSlot = UINT32_MAX;

struct Region {
  uint8_t gen = 1;
  bool live = false;
  bool writable = true;
  uint32_t next_free = kNoSlot;      // FIFO of free slots
  std::vector<uint8_t> bytes;        // owned regions
  // Lazy regions (native_wrap_lazy): the loader until the bytes are loaded.
  std::shared_ptr<const std::function<std::vector<uint8_t>()>> loader;
  uint64_t lazy_size = 0;
  uint64_t charged = 0;              // bytes counted against the budget
  rt::ArrayObject* array = nullptr;  // regions over a Java array (see ArrayRegion)
};

// The lock is never held while calling into the runtime: raising a Java
// exception allocates, which may run the collector, whose ArrayRegion peers
// take the lock.
struct Memory {
  std::mutex lock;
  std::vector<Region> regions;
  uint32_t free_head = kNoSlot, free_tail = kNoSlot, free_count = 0;
  size_t live = 0;
  uint64_t used = 0;
  uint64_t limit = kDefaultLimit;
};

// Never destroyed: a runtime may destroy its arrays (and their ArrayRegion
// peers) after static destructors have run.
Memory& memory() {
  static Memory* m = new Memory;
  return *m;
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

// The live region named by `d` (caller holds the lock).
Region* find(Memory& m, const Decoded& d) {
  if (d.id >= m.regions.size()) return nullptr;
  Region& r = m.regions[d.id];
  return r.live && r.gen == d.gen ? &r : nullptr;
}

bool fits(const Memory& m, uint64_t size) { return m.used <= m.limit && size <= m.limit - m.used; }

// Caller holds the lock. Never allocates (also runs from peer destructors).
void release(Memory& m, uint32_t id) {
  Region& r = m.regions[id];
  m.used -= std::min(m.used, r.charged);
  --m.live;
  uint8_t gen = uint8_t(r.gen + 1);
  r = Region{};
  r.gen = gen ? gen : 1;
  if (m.free_tail == kNoSlot) {
    m.free_head = id;
  } else {
    m.regions[m.free_tail].next_free = id;
  }
  m.free_tail = id;
  ++m.free_count;
}

// A slot for a new region, marked live (caller holds the lock); nullopt when
// the table is full. May throw std::bad_alloc.
std::optional<uint32_t> take_slot(Memory& m) {
  uint32_t id;
  if (m.free_count > 0 && (m.free_count >= kQuarantine || m.regions.size() >= kMaxRegions)) {
    id = m.free_head;
    m.free_head = m.regions[id].next_free;
    if (m.free_head == kNoSlot) m.free_tail = kNoSlot;
    m.regions[id].next_free = kNoSlot;
    --m.free_count;
  } else if (m.regions.size() < kMaxRegions) {
    m.regions.emplace_back();
    id = uint32_t(m.regions.size() - 1);
  } else {
    return std::nullopt;
  }
  m.regions[id].live = true;
  ++m.live;
  return id;
}

// Peer of a Java array that has a native address (VMRuntime.addressOf). The
// region is released when the array is collected (on_collect runs before
// any object of the cycle is destroyed) or destroyed with its runtime, so a
// live region always designates a live array. Comparing the array's handle
// and address instead would be fooled by a recycled heap slot (8-bit
// generation) holding a new object allocated at the same address.
struct ArrayRegion final : rt::NativePeer {
  uint32_t id = 0;
  uint8_t gen = 0;
  bool dropped = false;

  void on_collect(const std::function<bool(rt::Handle)>&) override { drop(); }
  ~ArrayRegion() override { drop(); }

  void drop() noexcept {
    if (dropped) return;
    dropped = true;
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    if (find(m, Decoded{id, gen, 0})) release(m, id);
  }
};

[[noreturn]] void unknown(rt::Runtime& rt, int64_t address) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(address));
  rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("adresse native inconnue : ") + buf);
}

[[noreturn]] void out_of_memory(rt::Runtime& rt, const char* what) {
  rt.throw_new("Ljava/lang/OutOfMemoryError;", std::string(what) + " : mémoire native épuisée");
}

[[noreturn]] void no_slot(rt::Runtime& rt) {
  rt.throw_new("Ljava/lang/OutOfMemoryError;", "trop de blocs de mémoire native");
}

// Publishes owned bytes as a new region.
int64_t add_owned(rt::Runtime& rt, std::vector<uint8_t>&& bytes, bool writable, const char* what) {
  enum { kOk, kBudget, kSlots, kNoMemory } status = kOk;
  int64_t address = 0;
  uint64_t charged = bytes.capacity();
  {
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    if (!fits(m, charged)) {
      status = kBudget;
    } else {
      try {
        if (auto id = take_slot(m)) {
          Region& r = m.regions[*id];
          r.writable = writable;
          r.bytes = std::move(bytes);
          r.charged = charged;
          m.used += charged;
          address = encode(*id, r.gen, 0);
        } else {
          status = kSlots;
        }
      } catch (const std::bad_alloc&) {
        status = kNoMemory;
      }
    }
  }
  if (status == kSlots) no_slot(rt);
  if (status != kOk) out_of_memory(rt, what);
  return address;
}

struct Found {
  std::span<uint8_t> data;
  uint32_t offset = 0;
  bool writable = true;
  bool owned = true;
  bool lazy = false;  // not loaded yet: data is empty
};

std::optional<Found> lookup(int64_t address) {
  auto d = decode(address);
  if (!d) return std::nullopt;
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  Region* r = find(m, *d);
  if (!r) return std::nullopt;
  Found f;
  f.data = r->array ? std::span<uint8_t>(r->array->data) : std::span<uint8_t>(r->bytes);
  f.offset = d->offset;
  f.writable = r->writable;
  f.owned = r->array == nullptr;
  f.lazy = r->loader != nullptr;
  return f;
}

// Loads a lazy region (the lock is not held while its loader runs: another
// thread may load it too, the first result is kept).
void load(rt::Runtime& rt, int64_t address) {
  Decoded d = *decode(address);
  Memory& m = memory();
  std::shared_ptr<const std::function<std::vector<uint8_t>()>> loader;
  uint64_t size = 0;
  {
    std::lock_guard<std::mutex> guard(m.lock);
    Region* r = find(m, d);
    if (!r || !r->loader) return;
    loader = r->loader;
    size = r->lazy_size;
  }
  std::vector<uint8_t> bytes;
  std::string failure;
  try {
    bytes = (*loader)();
    if (bytes.size() != size) failure = "taille inattendue";
  } catch (const std::bad_alloc&) {
    out_of_memory(rt, "mmap");
  } catch (const std::exception& e) {
    failure = e.what();
  }
  if (!failure.empty()) {
    rt.throw_new("Ljava/lang/InternalError;", "défaut lors d'un accès à une projection de fichier : " + failure);
  }
  bool budget = true;
  {
    std::lock_guard<std::mutex> guard(m.lock);
    Region* r = find(m, d);
    if (!r || !r->loader) return;  // freed, or loaded by another thread
    uint64_t charged = bytes.capacity();
    if (fits(m, charged)) {
      r->bytes = std::move(bytes);
      r->charged = charged;
      m.used += charged;
      r->loader = nullptr;
    } else {
      budget = false;
    }
  }
  if (!budget) out_of_memory(rt, "mmap");
}

// The whole region behind `address` and the offset in it, or an exception.
Found region_of(rt::Runtime& rt, int64_t address, bool write) {
  auto f = lookup(address);
  if (f && f->lazy) {
    load(rt, address);
    f = lookup(address);
  }
  if (!f) unknown(rt, address);
  if (write && !f->writable) rt.throw_new("Ljava/nio/ReadOnlyBufferException;", "");
  return *f;
}

}  // namespace

bool is_native_address(int64_t address) { return decode(address).has_value(); }

int64_t native_alloc(rt::Runtime& rt, uint64_t size) {
  if (size > kMaxRegionSize) rt.throw_new("Ljava/lang/OutOfMemoryError;", "allocateMemory : taille trop grande");
  if (!native_memory_fits(size)) out_of_memory(rt, "allocateMemory");  // before building the buffer
  std::vector<uint8_t> bytes;
  try {
    bytes.assign(size_t(size), 0);
  } catch (const std::bad_alloc&) {
    out_of_memory(rt, "allocateMemory");
  }
  return add_owned(rt, std::move(bytes), true, "allocateMemory");
}

bool native_try_free(int64_t address) {
  auto d = decode(address);
  if (!d || d->offset != 0) return false;
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  Region* r = find(m, *d);
  if (!r || r->array) return false;  // array regions die with their array
  release(m, d->id);
  return true;
}

void native_free(rt::Runtime& rt, int64_t address) {
  if (address != 0 && !native_try_free(address)) unknown(rt, address);
}

int64_t native_realloc(rt::Runtime& rt, int64_t address, uint64_t size) {
  if (address == 0) return size ? native_alloc(rt, size) : 0;
  Found old = region_of(rt, address, false);
  if (old.offset != 0 || !old.owned) unknown(rt, address);
  if (size == 0) {
    native_free(rt, address);
    return 0;
  }
  // As realloc(3): the old block stays valid until the new one exists (both
  // count against the budget meanwhile) and is left untouched on failure.
  int64_t fresh = native_alloc(rt, size);
  old = region_of(rt, address, false);  // spans stay valid, but re-read after the allocation
  size_t n = std::min<size_t>(old.data.size(), size_t(size));
  if (n) std::memcpy(native_span(rt, fresh, n, true).data(), old.data.data(), n);
  native_free(rt, address);
  return fresh;
}

int64_t native_address_of(rt::Runtime& rt, rt::Handle array) {
  auto* a = dynamic_cast<rt::ArrayObject*>(rt.object(array));
  if (!a || !a->klass || !a->klass->component || a->klass->component->primitive == 0) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "addressOf : tableau primitif attendu");
  }
  if (auto* known = dynamic_cast<ArrayRegion*>(a->peer.get())) return encode(known->id, known->gen, 0);
  if (a->peer) rt.throw_new("Ljava/lang/IllegalArgumentException;", "addressOf : tableau doté d'un autre état natif");
  std::unique_ptr<ArrayRegion> peer;
  bool full = false, no_memory = false;
  try {
    peer = std::make_unique<ArrayRegion>();
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    if (auto id = take_slot(m)) {
      Region& r = m.regions[*id];
      r.array = a;
      peer->id = *id;
      peer->gen = r.gen;
    } else {
      full = true;
    }
  } catch (const std::bad_alloc&) {
    no_memory = true;
  }
  if (full) no_slot(rt);
  if (no_memory) out_of_memory(rt, "addressOf");
  int64_t address = encode(peer->id, peer->gen, 0);
  a->peer = std::move(peer);
  return address;
}

int64_t native_wrap(rt::Runtime& rt, std::vector<uint8_t> data, bool writable) {
  if (data.size() > kMaxRegionSize) rt.throw_new("Ljava/lang/OutOfMemoryError;", "bloc trop grand");
  return add_owned(rt, std::move(data), writable, "mmap");
}

int64_t native_wrap_lazy(rt::Runtime& rt, uint64_t size, std::function<std::vector<uint8_t>()> load) {
  if (size > kMaxRegionSize) rt.throw_new("Ljava/lang/OutOfMemoryError;", "bloc trop grand");
  auto loader = std::make_shared<const std::function<std::vector<uint8_t>()>>(std::move(load));
  bool full = false, no_memory = false;
  int64_t address = 0;
  {
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    try {
      if (auto id = take_slot(m)) {
        Region& r = m.regions[*id];
        r.writable = false;
        r.loader = std::move(loader);
        r.lazy_size = size;
        address = encode(*id, r.gen, 0);
      } else {
        full = true;
      }
    } catch (const std::bad_alloc&) {
      no_memory = true;
    }
  }
  if (full) no_slot(rt);
  if (no_memory) out_of_memory(rt, "mmap");
  return address;
}

std::span<uint8_t> native_span(rt::Runtime& rt, int64_t address, uint64_t size, bool write) {
  Found f = region_of(rt, address, write);
  if (f.offset > f.data.size() || size > f.data.size() - f.offset) {
    rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "accès hors du bloc de mémoire native");
  }
  return f.data.subspan(f.offset, size_t(size));
}

std::string native_cstring(rt::Runtime& rt, int64_t address, size_t max) {
  Found f = region_of(rt, address, false);
  if (f.offset > f.data.size()) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "chaîne hors du bloc");
  auto rest = f.data.subspan(f.offset);
  size_t n = std::min(rest.size(), max);
  auto end = std::find(rest.begin(), rest.begin() + n, uint8_t(0));
  if (end == rest.begin() + n) rt.throw_new("Ljava/lang/IllegalArgumentException;", "chaîne native sans fin");
  return std::string(rest.begin(), end);
}

rt::ArrayObject& native_array(rt::Runtime& rt, int64_t address) {
  auto d = decode(address);
  if (!d || d->offset != 0) unknown(rt, address);
  rt::ArrayObject* a = nullptr;
  {
    Memory& m = memory();
    std::lock_guard<std::mutex> guard(m.lock);
    if (Region* r = find(m, *d)) a = r->array;
  }
  if (!a) unknown(rt, address);
  return *a;
}

NativeMemoryStats native_memory_stats() {
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  return {m.used, m.limit, m.live};
}

void set_native_memory_limit(uint64_t bytes) {
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  m.limit = bytes;
}

bool native_memory_fits(uint64_t size) {
  Memory& m = memory();
  std::lock_guard<std::mutex> guard(m.lock);
  return fits(m, size);
}

}  // namespace aa::bridge
