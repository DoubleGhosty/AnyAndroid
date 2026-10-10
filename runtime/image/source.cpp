#include "image/source.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "common/bytes.h"
#include "common/file_io.h"
#include "common/hash.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <mutex>
#endif

namespace aa::image {

namespace {

void check_range(uint64_t offset, size_t length, uint64_t size, const char* what) {
  if (offset > size || length > size - offset) {
    throw_format(std::string(what) + " : lecture hors limites (" + std::to_string(offset) + " + " +
                 std::to_string(length) + " > " + std::to_string(size) + ")");
  }
}

}  // namespace

std::vector<uint8_t> Source::read(uint64_t offset, size_t length) const {
  std::vector<uint8_t> out(length);
  read(offset, out);
  return out;
}

uint32_t Source::u32_at(uint64_t offset) const {
  uint8_t b[4];
  read(offset, b);
  return load_u32(b);
}

// --- MemorySource ---------------------------------------------------------------

void MemorySource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), data_.size(), "mémoire");
  if (!out.empty()) std::memcpy(out.data(), data_.data() + offset, out.size());
}

// --- FileSource -------------------------------------------------------------------

#ifdef _WIN32

FileSource::FileSource(const std::filesystem::path& path) {
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("ouverture impossible : " + path_utf8(path));
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz)) {
    CloseHandle(h);
    throw std::runtime_error("taille illisible : " + path_utf8(path));
  }
  handle_ = h;
  size_ = uint64_t(sz.QuadPart);
}

FileSource::~FileSource() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

void FileSource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), size_, "fichier");
  size_t done = 0;
  while (done < out.size()) {
    DWORD chunk = DWORD(std::min<size_t>(out.size() - done, 64u << 20));
    OVERLAPPED ov{};
    uint64_t pos = offset + done;
    ov.Offset = DWORD(pos);
    ov.OffsetHigh = DWORD(pos >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), out.data() + done, chunk, &got, &ov) || got == 0) {
      throw std::runtime_error("erreur de lecture à l'offset " + std::to_string(pos));
    }
    done += got;
  }
}

#else

namespace {
struct PortableFile {
  std::ifstream in;
  std::mutex mu;
};
}  // namespace

FileSource::FileSource(const std::filesystem::path& path) {
  auto f = new PortableFile;
  f->in.open(path, std::ios::binary);
  if (!f->in) {
    delete f;
    throw std::runtime_error("ouverture impossible : " + path_utf8(path));
  }
  size_ = std::filesystem::file_size(path);
  handle_ = f;
}

FileSource::~FileSource() { delete static_cast<PortableFile*>(handle_); }

void FileSource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), size_, "fichier");
  auto* f = static_cast<PortableFile*>(handle_);
  std::lock_guard lock(f->mu);
  f->in.clear();
  f->in.seekg(std::streamoff(offset));
  if (!f->in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()))) {
    throw std::runtime_error("erreur de lecture à l'offset " + std::to_string(offset));
  }
}

#endif

// --- SliceSource / ExtentSource -------------------------------------------------------

SliceSource::SliceSource(std::shared_ptr<const Source> base, uint64_t offset, uint64_t length)
    : base_(std::move(base)), offset_(offset), length_(length) {
  check_range(offset, 0, base_->size(), "tranche");
  if (length > base_->size() - offset) throw_format("tranche hors de l'image");
}

void SliceSource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), length_, "tranche");
  base_->read(offset_ + offset, out);
}

ExtentSource::ExtentSource(std::shared_ptr<const Source> base, std::vector<Extent> extents)
    : base_(std::move(base)), extents_(std::move(extents)) {
  uint64_t pos = 0;
  for (const Extent& e : extents_) {
    if (e.logical != pos) throw_format("extents non contigus");
    if (!e.zero && (e.physical > base_->size() || e.length > base_->size() - e.physical)) {
      throw_format("extent hors de l'image");
    }
    pos += e.length;
  }
  size_ = pos;
}

void ExtentSource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), size_, "partition");
  if (out.empty()) return;
  auto it = std::upper_bound(extents_.begin(), extents_.end(), offset,
                             [](uint64_t off, const Extent& e) { return off < e.logical; });
  size_t done = 0;
  for (--it; done < out.size(); ++it) {
    uint64_t pos = offset + done;
    uint64_t in_ext = pos - it->logical;
    size_t n = size_t(std::min<uint64_t>(it->length - in_ext, out.size() - done));
    if (it->zero) {
      std::memset(out.data() + done, 0, n);
    } else {
      base_->read(it->physical + in_ext, out.subspan(done, n));
    }
    done += n;
  }
}

// --- SparseSource -----------------------------------------------------------------------

SparseSource::SparseSource(std::shared_ptr<const Source> base) : base_(std::move(base)) {
  auto hdr = base_->read(0, 28);
  ByteReader r(hdr);
  if (r.u32() != kMagic) throw_format("ce n'est pas une image sparse");
  uint16_t major = r.u16();
  r.u16();  // minor
  uint16_t file_hdr = r.u16();
  uint16_t chunk_hdr = r.u16();
  uint32_t blk_sz = r.u32();
  uint32_t total_blks = r.u32();
  uint32_t total_chunks = r.u32();
  if (major != 1) throw_format("version d'image sparse non prise en charge : " + std::to_string(major));
  if (file_hdr < 28 || chunk_hdr < 12) throw_format("en-têtes sparse trop courts");
  if (blk_sz == 0 || blk_sz % 4 != 0 || blk_sz > (64u << 20)) throw_format("taille de bloc sparse invalide");
  if (total_chunks > 50'000'000) throw_format("trop de segments sparse");

  uint64_t pos = file_hdr;
  uint64_t out = 0;
  chunks_.reserve(total_chunks);
  for (uint32_t i = 0; i < total_chunks; ++i) {
    auto ch = base_->read(pos, 12);
    uint16_t type = load_u16(ch.data());
    uint32_t chunk_blocks = load_u32(ch.data() + 4);
    uint32_t total_sz = load_u32(ch.data() + 8);
    uint64_t len = uint64_t(chunk_blocks) * blk_sz;
    uint64_t data = pos + chunk_hdr;
    Chunk c{out, len, Chunk::kDontCare, 0, 0};
    switch (type) {
      case 0xCAC1:  // raw
        if (total_sz != chunk_hdr + len) throw_format("segment sparse brut incohérent");
        if (data + len > base_->size()) throw_format("segment sparse brut tronqué");
        c.kind = Chunk::kRaw;
        c.in_offset = data;
        break;
      case 0xCAC2:  // fill
        if (total_sz != chunk_hdr + 4u) throw_format("segment sparse de remplissage incohérent");
        c.kind = Chunk::kFill;
        c.fill = base_->u32_at(data);
        break;
      case 0xCAC3:  // don't care
        if (total_sz != chunk_hdr) throw_format("segment sparse vide incohérent");
        break;
      case 0xCAC4:  // crc32 of the data so far
        if (total_sz != chunk_hdr + 4u) throw_format("segment sparse CRC incohérent");
        len = 0;
        break;
      default:
        throw_format("type de segment sparse inconnu : " + hex(type, 4));
    }
    if (len) {
      chunks_.push_back(c);
      out += len;
    }
    pos += total_sz;
  }
  if (out != uint64_t(total_blks) * blk_sz) throw_format("image sparse : nombre de blocs incohérent");
  size_ = out;
}

void SparseSource::read(uint64_t offset, std::span<uint8_t> out) const {
  check_range(offset, out.size(), size_, "image sparse");
  if (out.empty()) return;
  auto it = std::upper_bound(chunks_.begin(), chunks_.end(), offset,
                             [](uint64_t off, const Chunk& c) { return off < c.out_offset; });
  size_t done = 0;
  for (--it; done < out.size(); ++it) {
    uint64_t pos = offset + done;
    uint64_t in_chunk = pos - it->out_offset;
    size_t n = size_t(std::min<uint64_t>(it->length - in_chunk, out.size() - done));
    switch (it->kind) {
      case Chunk::kRaw: base_->read(it->in_offset + in_chunk, out.subspan(done, n)); break;
      case Chunk::kFill:
        for (size_t i = 0; i < n; ++i) out[done + i] = uint8_t(it->fill >> (8 * ((in_chunk + i) % 4)));
        break;
      case Chunk::kDontCare: std::memset(out.data() + done, 0, n); break;
    }
    done += n;
  }
}

// --- Super image (liblp) -------------------------------------------------------------------

namespace {

constexpr uint64_t kLpReserved = 4096;
constexpr uint64_t kLpGeometrySize = 4096;
constexpr uint32_t kLpGeometryMagic = 0x616c4467;
constexpr uint32_t kLpHeaderMagic = 0x414C5030;

std::string fixed_name(std::span<const uint8_t> b) {
  size_t n = 0;
  while (n < b.size() && b[n]) ++n;
  return std::string(b.begin(), b.begin() + n);
}

}  // namespace

bool is_super_image(const Source& src) {
  return src.size() >= kLpReserved + 2 * kLpGeometrySize + 128 && src.u32_at(kLpReserved) == kLpGeometryMagic;
}

std::vector<LogicalPartition> read_super_partitions(const Source& src) {
  if (!is_super_image(src)) throw_format("ce n'est pas une image super (partitions dynamiques)");
  auto geo = src.read(kLpReserved, 52);
  ByteReader g(geo);
  g.skip(4);
  uint32_t struct_size = g.u32();
  if (struct_size < 52 || struct_size > kLpGeometrySize) throw_format("géométrie liblp invalide");
  auto geo_full = src.read(kLpReserved, struct_size);
  std::array<uint8_t, 32> expected;
  std::memcpy(expected.data(), geo_full.data() + 8, 32);
  std::memset(geo_full.data() + 8, 0, 32);
  if (hash::sha256(geo_full) != expected) throw_format("somme de contrôle de la géométrie liblp invalide");
  g.skip(32);
  uint32_t metadata_max = g.u32();
  if (metadata_max < 128 || metadata_max > (16u << 20)) throw_format("taille de métadonnées liblp invalide");

  uint64_t meta = kLpReserved + 2 * kLpGeometrySize;  // slot 0, primary copy
  auto head = src.read(meta, 128);
  ByteReader h(head);
  if (h.u32() != kLpHeaderMagic) throw_format("en-tête de métadonnées liblp invalide");
  uint16_t major = h.u16();
  h.u16();  // minor
  uint32_t header_size = h.u32();
  if (major != 10) throw_format("version liblp non prise en charge : " + std::to_string(major));
  if (header_size < 128 || header_size > metadata_max) throw_format("taille d'en-tête liblp invalide");
  auto header = src.read(meta, header_size);
  std::memcpy(expected.data(), header.data() + 12, 32);
  std::memset(header.data() + 12, 0, 32);
  if (hash::sha256(header) != expected) throw_format("somme de contrôle de l'en-tête liblp invalide");
  ByteReader hr(header);
  hr.seek(44);
  uint32_t tables_size = hr.u32();
  std::array<uint8_t, 32> tables_sum;
  std::memcpy(tables_sum.data(), header.data() + 48, 32);
  if (uint64_t(header_size) + tables_size > metadata_max) throw_format("tables liblp trop grandes");
  auto tables = src.read(meta + header_size, tables_size);
  if (hash::sha256(tables) != tables_sum) throw_format("somme de contrôle des tables liblp invalide");

  struct Desc {
    uint32_t offset, count, entry_size;
  };
  hr.seek(80);
  auto desc = [&](uint32_t min_entry) {
    Desc d{hr.u32(), hr.u32(), hr.u32()};
    if (d.entry_size < min_entry || uint64_t(d.offset) + uint64_t(d.count) * d.entry_size > tables.size()) {
      throw_format("table liblp hors limites");
    }
    return d;
  };
  Desc parts = desc(52), exts = desc(24), groups = desc(48);
  ByteReader t(tables);

  std::vector<std::string> group_names;
  for (uint32_t i = 0; i < groups.count; ++i) {
    group_names.push_back(fixed_name(std::span(tables).subspan(groups.offset + i * groups.entry_size, 36)));
  }
  std::vector<LogicalPartition> out;
  for (uint32_t i = 0; i < parts.count; ++i) {
    size_t base = parts.offset + size_t(i) * parts.entry_size;
    LogicalPartition p;
    p.name = fixed_name(std::span(tables).subspan(base, 36));
    p.attributes = t.u32_at(base + 36);
    uint32_t first = t.u32_at(base + 40);
    uint32_t count = t.u32_at(base + 44);
    uint32_t group = t.u32_at(base + 48);
    if (group < group_names.size()) p.group = group_names[group];
    if (uint64_t(first) + count > exts.count) throw_format("extents liblp hors limites pour " + p.name);
    uint64_t logical = 0;
    for (uint32_t e = first; e < first + count; ++e) {
      ByteReader x = t.sub(exts.offset + size_t(e) * exts.entry_size, 24);
      uint64_t sectors = x.u64();
      uint32_t type = x.u32();
      uint64_t data = x.u64();
      uint32_t source = x.u32();
      if (sectors > (UINT64_MAX >> 10)) throw_format("extent liblp trop grand");
      ExtentSource::Extent ext;
      ext.logical = logical;
      ext.length = sectors * 512;
      if (type == 1) {
        ext.zero = true;
      } else if (type == 0) {
        if (source != 0) throw_format("partition " + p.name + " répartie sur plusieurs périphériques : non prise en charge");
        if (data > (UINT64_MAX >> 10)) throw_format("extent liblp invalide");
        ext.physical = data * 512;
      } else {
        throw_format("type d'extent liblp inconnu : " + std::to_string(type));
      }
      p.extents.push_back(ext);
      logical += ext.length;
    }
    p.size = logical;
    out.push_back(std::move(p));
  }
  return out;
}

// --- Opening ---------------------------------------------------------------------------------

OpenedImage open_image(const std::filesystem::path& path, const std::string& partition) {
  OpenedImage img;
  std::shared_ptr<const Source> src = std::make_shared<FileSource>(path);
  if (src->size() >= 28 && src->u32_at(0) == SparseSource::kMagic) {
    src = std::make_shared<SparseSource>(src);
    img.layers.push_back("sparse");
  }
  if (is_super_image(*src)) {
    auto parts = read_super_partitions(*src);
    const LogicalPartition* chosen = nullptr;
    std::vector<std::string> wanted;
    if (!partition.empty()) {
      wanted = {partition};
    } else {
      wanted = {"system", "system_a", "system_b"};
    }
    for (const auto& w : wanted) {
      for (const auto& p : parts) {
        if (p.name == w && p.size > 0) {
          chosen = &p;
          break;
        }
      }
      if (chosen) break;
    }
    if (!chosen) {
      std::string names;
      for (const auto& p : parts) names += (names.empty() ? "" : ", ") + p.name;
      throw_format("partition introuvable dans l'image super (disponibles : " + names + ")");
    }
    src = std::make_shared<ExtentSource>(src, chosen->extents);
    img.layers.push_back("super:" + chosen->name);
  } else if (!partition.empty()) {
    throw_format("--partition ne s'applique qu'aux images super");
  }
  img.source = std::move(src);
  return img;
}

}  // namespace aa::image
