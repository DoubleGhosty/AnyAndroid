#include "resources/res_table.h"

#include <algorithm>
#include <cstring>

#include "common/bytes.h"
#include "common/text.h"

namespace aa::res {

namespace {

constexpr uint16_t kChunkStringPool = 0x0001;
constexpr uint16_t kChunkTable = 0x0002;
constexpr uint16_t kChunkPackage = 0x0200;
constexpr uint16_t kChunkType = 0x0201;
constexpr uint16_t kChunkTypeSpec = 0x0202;
constexpr uint16_t kChunkLibrary = 0x0203;

constexpr uint16_t kEntryComplex = 0x0001;
constexpr uint16_t kEntryCompact = 0x0008;
constexpr uint8_t kTypeSparse = 0x01;
constexpr uint8_t kTypeOffset16 = 0x02;

constexpr uint32_t kMaxStyleSpans = 4096;

struct Chunk {
  uint16_t type = 0;
  uint16_t header_size = 0;
  uint32_t size = 0;
  std::span<const uint8_t> data;  // whole chunk
};

// Reads the chunk header at `pos` of `area`, checking it as AOSP's
// validate_chunk does (sizes, alignment, bounds).
Chunk chunk_at(std::span<const uint8_t> area, size_t pos, uint16_t min_header) {
  if (pos + 8 > area.size()) throw_format("en-tête de bloc tronqué");
  Chunk c;
  c.type = load_u16(area.data() + pos);
  c.header_size = load_u16(area.data() + pos + 2);
  c.size = load_u32(area.data() + pos + 4);
  if (c.header_size < std::max<uint16_t>(min_header, 8) || c.size < c.header_size || ((c.size | c.header_size) & 3) ||
      c.size > area.size() - pos) {
    throw_format("bloc de ressources invalide (type 0x" + std::to_string(c.type) + ")");
  }
  c.data = area.subspan(pos, c.size);
  return c;
}

std::u16string fixed_u16(std::span<const uint8_t> b, size_t chars) {
  std::u16string out;
  for (size_t i = 0; i < chars && 2 * i + 1 < b.size(); ++i) {
    char16_t c = char16_t(load_u16(b.data() + 2 * i));
    if (c == 0) break;
    out.push_back(c);
  }
  return out;
}

}  // namespace

// --- StringPool ---------------------------------------------------------------------

std::shared_ptr<StringPool> StringPool::parse(Bytes bytes, size_t offset, size_t size) {
  if (!bytes || offset > bytes->size() || size > bytes->size() - offset) throw_format("pool de chaînes hors limites");
  std::shared_ptr<StringPool> p(new StringPool());
  p->bytes_ = bytes;
  p->chunk_ = std::span<const uint8_t>(bytes->data() + offset, size);
  Chunk c = chunk_at(p->chunk_, 0, 28);
  if (c.type != kChunkStringPool) throw_format("pool de chaînes attendu");
  p->chunk_ = c.data;
  const uint8_t* h = c.data.data();
  p->count_ = load_u32(h + 8);
  p->style_count_ = load_u32(h + 12);
  p->flags_ = load_u32(h + 16);
  p->strings_start_ = load_u32(h + 20);
  p->styles_start_ = load_u32(h + 24);
  p->index_offset_ = c.header_size;
  uint64_t index_end = uint64_t(c.header_size) + (uint64_t(p->count_) + p->style_count_) * 4;
  if (index_end > c.size) throw_format("index du pool de chaînes hors limites");
  if (p->count_ && (p->strings_start_ < index_end || p->strings_start_ >= c.size)) {
    throw_format("début des chaînes invalide");
  }
  if (p->style_count_ && (p->styles_start_ < index_end || p->styles_start_ >= c.size)) {
    throw_format("début des styles invalide");
  }
  return p;
}

std::optional<std::span<const uint8_t>> StringPool::raw(uint32_t index, size_t* chars) const {
  if (index >= count_) return std::nullopt;
  size_t end = (style_count_ && styles_start_ > strings_start_) ? styles_start_ : chunk_.size();
  size_t pos = size_t(strings_start_) + load_u32(chunk_.data() + index_offset_ + size_t(index) * 4);
  const uint8_t* d = chunk_.data();
  if (utf8()) {
    auto length = [&](size_t& at) -> std::optional<size_t> {
      if (at >= end) return std::nullopt;
      size_t v = d[at];
      if (v & 0x80) {
        if (at + 1 >= end) return std::nullopt;
        v = ((v & 0x7f) << 8) | d[at + 1];
        at += 2;
      } else {
        at += 1;
      }
      return v;
    };
    auto n16 = length(pos);
    if (!n16) return std::nullopt;
    auto n8 = length(pos);
    if (!n8 || pos + *n8 > end) return std::nullopt;
    *chars = *n16;
    return chunk_.subspan(pos, *n8);
  }
  if (pos + 2 > end) return std::nullopt;
  size_t n = load_u16(d + pos);
  pos += 2;
  if (n & 0x8000) {
    if (pos + 2 > end) return std::nullopt;
    n = ((n & 0x7fff) << 16) | load_u16(d + pos);
    pos += 2;
  }
  if (n > (end - pos) / 2) return std::nullopt;
  *chars = n;
  return chunk_.subspan(pos, n * 2);
}

std::optional<std::u16string> StringPool::string16(uint32_t index) const {
  size_t chars = 0;
  auto r = raw(index, &chars);
  if (!r) return std::nullopt;
  if (utf8()) {
    return text::utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(r->data()), r->size()));
  }
  std::u16string out(chars, u'\0');
  for (size_t i = 0; i < chars; ++i) out[i] = char16_t(load_u16(r->data() + 2 * i));
  return out;
}

std::optional<std::string> StringPool::string8(uint32_t index) const {
  if (!utf8()) {
    auto s = string16(index);
    if (!s) return std::nullopt;
    return text::utf16_to_utf8(*s);
  }
  size_t chars = 0;
  auto r = raw(index, &chars);
  if (!r) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(r->data()), r->size());
}

std::vector<uint32_t> StringPool::style(uint32_t index) const {
  std::vector<uint32_t> out;
  if (index >= style_count_) return out;
  size_t pos = size_t(styles_start_) + load_u32(chunk_.data() + index_offset_ + (size_t(count_) + index) * 4);
  while (pos + 4 <= chunk_.size() && out.size() < kMaxStyleSpans * 3) {
    uint32_t name = load_u32(chunk_.data() + pos);
    if (name == 0xffffffffu) break;
    if (pos + 12 > chunk_.size()) break;
    out.push_back(name);
    out.push_back(load_u32(chunk_.data() + pos + 4));
    out.push_back(load_u32(chunk_.data() + pos + 8));
    pos += 12;
  }
  return out;
}

std::optional<uint32_t> StringPool::find(std::u16string_view s) const {
  std::lock_guard<std::mutex> guard(lock_);
  if (!lookup_) {
    lookup_ = std::make_unique<std::unordered_map<std::u16string, uint32_t>>();
    for (uint32_t i = 0; i < count_; ++i) {
      if (auto v = string16(i)) lookup_->emplace(std::move(*v), i);
    }
  }
  auto it = lookup_->find(std::u16string(s));
  if (it == lookup_->end()) return std::nullopt;
  return it->second;
}

// --- Types and entries ------------------------------------------------------------------

std::optional<uint32_t> TypeVariant::entry_offset(uint32_t index) const {
  const uint8_t* o = offsets.data();
  if (flags & kTypeSparse) {
    // Sorted (index, offset / 4) pairs.
    size_t lo = 0, hi = entry_count;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      uint16_t idx = load_u16(o + mid * 4);
      if (idx == index) return uint32_t(load_u16(o + mid * 4 + 2)) * 4;
      if (idx < index) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return std::nullopt;
  }
  if (index >= entry_count) return std::nullopt;
  if (flags & kTypeOffset16) {
    uint16_t v = load_u16(o + size_t(index) * 2);
    if (v == 0xffff) return std::nullopt;
    return uint32_t(v) * 4;
  }
  uint32_t v = load_u32(o + size_t(index) * 4);
  if (v == 0xffffffffu) return std::nullopt;
  return v;
}

const TypeGroup* LoadedPackage::type(uint8_t type_id) const {
  if (type_id == 0 || type_id > types.size() || types[type_id - 1u].id == 0) return nullptr;
  return &types[type_id - 1u];
}

std::optional<Entry> LoadedPackage::read_entry(const TypeVariant& v, uint32_t offset) const {
  std::span<const uint8_t> e = v.entries;
  if ((offset & 3) || uint64_t(offset) + 8 > e.size()) return std::nullopt;
  const uint8_t* p = e.data() + offset;
  Entry out;
  out.flags = load_u16(p + 2);
  if (out.flags & kEntryCompact) {
    out.key = load_u16(p);
    out.value.type = uint8_t(out.flags >> 8);
    out.value.data = load_u32(p + 4);
    return out;
  }
  uint16_t size = load_u16(p);
  out.key = load_u32(p + 4);
  if (size < 8 || uint64_t(offset) + size > e.size()) return std::nullopt;
  if (out.flags & kEntryComplex) {
    if (size < 16) return std::nullopt;
    out.complex = true;
    out.parent = load_u32(p + 8);
    uint32_t count = load_u32(p + 12);
    uint64_t maps = uint64_t(offset) + size;
    if (uint64_t(count) * 12 > e.size() - maps) return std::nullopt;
    out.map.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* m = e.data() + maps + uint64_t(i) * 12;
      out.map.emplace_back(load_u32(m), Value{m[7], load_u32(m + 8)});
    }
    return out;
  }
  uint64_t value = uint64_t(offset) + size;
  if (value + 8 > e.size()) return std::nullopt;
  out.value.type = e[value + 3];
  out.value.data = load_u32(e.data() + value + 4);
  return out;
}

std::optional<std::u16string> LoadedPackage::type_name(uint8_t type_id) const {
  if (!type_strings || type_id == 0 || type_id - 1u < type_id_offset) return std::nullopt;
  return type_strings->string16(type_id - 1u - type_id_offset);
}

std::optional<uint32_t> LoadedPackage::find_entry_by_name(std::u16string_view type_name_wanted,
                                                          std::u16string_view entry_name) const {
  std::lock_guard<std::mutex> guard(lock_);
  if (!names_built_) {
    names_built_ = true;
    for (const TypeGroup& g : types) {
      if (g.id == 0 || !key_strings) continue;
      auto tname = type_name(g.id);
      if (!tname) continue;
      auto& by_name = names_[*tname];
      for (uint32_t i = 0; i < g.spec_flags.size(); ++i) {
        for (const TypeVariant& v : g.variants) {
          auto off = v.entry_offset(i);
          if (!off) continue;
          auto entry = read_entry(v, *off);
          if (!entry) continue;
          if (auto key = key_strings->string16(entry->key)) {
            by_name.emplace(std::move(*key), (id << 24) | (uint32_t(g.id) << 16) | i);
          }
          break;
        }
      }
    }
  }
  auto t = names_.find(std::u16string(type_name_wanted));
  if (t == names_.end()) return std::nullopt;
  auto e = t->second.find(std::u16string(entry_name));
  if (e == t->second.end()) return std::nullopt;
  return e->second;
}

// --- LoadedArsc -----------------------------------------------------------------------

namespace {

std::unique_ptr<LoadedPackage> load_package(const Bytes& data, const Chunk& pc) {
  if (pc.header_size < 284) throw_format("en-tête de paquet trop court");
  auto pkg = std::make_unique<LoadedPackage>();
  const uint8_t* h = pc.data.data();
  pkg->id = load_u32(h + 8);
  if (pkg->id > 0xff) throw_format("identifiant de paquet invalide");
  pkg->name = fixed_u16(pc.data.subspan(12, 256), 128);
  uint32_t type_strings = load_u32(h + 268);
  uint32_t key_strings = load_u32(h + 276);
  if (pc.header_size >= 288) pkg->type_id_offset = load_u32(h + 284);
  if (pkg->type_id_offset > 255) throw_format("décalage de types invalide");
  size_t base = size_t(pc.data.data() - data->data());

  for (size_t pos = pc.header_size; pos < pc.size;) {
    Chunk c = chunk_at(pc.data, pos, 8);
    switch (c.type) {
      case kChunkStringPool:
        if (pos == type_strings && !pkg->type_strings) {
          pkg->type_strings = StringPool::parse(data, base + pos, c.size);
        } else if (pos == key_strings && !pkg->key_strings) {
          pkg->key_strings = StringPool::parse(data, base + pos, c.size);
        }
        break;
      case kChunkTypeSpec: {
        if (c.header_size < 16) throw_format("typeSpec trop court");
        uint8_t id = c.data[8];
        uint32_t count = load_u32(c.data.data() + 12);
        if (id == 0) throw_format("typeSpec d'identifiant nul");
        if (uint64_t(count) * 4 > c.size - c.header_size) throw_format("typeSpec hors limites");
        if (pkg->types.size() < id) pkg->types.resize(id);
        TypeGroup& g = pkg->types[id - 1u];
        if (g.id == 0) {
          g.id = id;
          g.spec_flags.resize(count);
          for (uint32_t i = 0; i < count; ++i) g.spec_flags[i] = load_u32(c.data.data() + c.header_size + i * 4);
        }
        break;
      }
      case kChunkType: {
        if (c.header_size < 24) throw_format("type trop court");
        uint8_t id = c.data[8];
        if (id == 0 || id > pkg->types.size() || pkg->types[id - 1u].id == 0) {
          throw_format("type sans typeSpec");
        }
        TypeVariant v;
        v.flags = c.data[9];
        v.entry_count = load_u32(c.data.data() + 12);
        uint32_t entries_start = load_u32(c.data.data() + 16);
        v.config = ResConfig::parse(c.data.subspan(20, c.header_size - 20u));
        uint64_t per = (v.flags & kTypeOffset16) && !(v.flags & kTypeSparse) ? 2 : 4;
        uint64_t offsets_end = c.header_size + per * v.entry_count;
        if (offsets_end > entries_start || entries_start > c.size) throw_format("table d'entrées hors limites");
        v.offsets = c.data.subspan(c.header_size, size_t(offsets_end - c.header_size));
        v.entries = c.data.subspan(entries_start);
        pkg->types[id - 1u].variants.push_back(std::move(v));
        break;
      }
      case kChunkLibrary: {
        if (c.header_size < 12) throw_format("bloc de bibliothèques trop court");
        uint32_t count = load_u32(c.data.data() + 8);
        if (uint64_t(count) * 260 > c.size - c.header_size) throw_format("bibliothèques hors limites");
        for (uint32_t i = 0; i < count; ++i) {
          auto e = c.data.subspan(c.header_size + size_t(i) * 260, 260);
          pkg->libraries.emplace_back(load_u32(e.data()), fixed_u16(e.subspan(4), 128));
        }
        break;
      }
      default:
        break;  // overlayables, staged aliases: not used
    }
    pos += c.size;
  }
  return pkg;
}

}  // namespace

std::shared_ptr<LoadedArsc> LoadedArsc::load(Bytes data) {
  if (!data) throw_format("table de ressources absente");
  std::span<const uint8_t> all(*data);
  Chunk table = chunk_at(all, 0, 12);
  if (table.type != kChunkTable) throw_format("resources.arsc : en-tête de table attendu");
  auto arsc = std::shared_ptr<LoadedArsc>(new LoadedArsc());
  arsc->data_ = data;
  for (size_t pos = table.header_size; pos < table.size;) {
    Chunk c = chunk_at(table.data, pos, 8);
    if (c.type == kChunkStringPool && !arsc->values_) {
      arsc->values_ = StringPool::parse(data, pos, c.size);
    } else if (c.type == kChunkPackage) {
      arsc->packages_.push_back(load_package(data, c));
    }
    pos += c.size;
  }
  if (!arsc->values_) throw_format("resources.arsc sans pool de valeurs");
  return arsc;
}

}  // namespace aa::res
