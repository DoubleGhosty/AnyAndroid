#include "apk-inspector/zip_reader.h"

#include <algorithm>

#include "apk-inspector/inflate.h"
#include "common/bytes.h"
#include "common/hash.h"
#include "common/text.h"

namespace aa::apk {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCdSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr size_t kEocdSize = 22;

}  // namespace

bool is_unsafe_entry_name(std::string_view name) {
  if (name.empty()) return true;
  if (name.front() == '/') return true;
  if (name.find('\\') != std::string_view::npos) return true;
  if (name.find('\0') != std::string_view::npos) return true;
  if (name.size() >= 2 && name[1] == ':') return true;  // "C:..."
  for (const auto& part : text::split(name, '/')) {
    if (part == "..") return true;
  }
  return false;
}

std::unique_ptr<ZipArchive> ZipArchive::open(std::vector<uint8_t> data, const ZipLimits& limits) {
  if (data.size() > limits.max_archive_size) throw_format("archive trop volumineuse");
  std::unique_ptr<ZipArchive> zip(new ZipArchive());
  zip->data_ = std::move(data);
  zip->limits_ = limits;
  zip->parse();
  return zip;
}

void ZipArchive::parse() {
  ByteReader r(data_);
  if (data_.size() < kEocdSize) throw_format("fichier trop court pour être une archive ZIP");

  // The end-of-central-directory record sits in the last 64 KiB + 22 bytes
  // (variable-length comment). Search backwards for its signature.
  size_t min_pos = data_.size() > 0xFFFF + kEocdSize ? data_.size() - (0xFFFF + kEocdSize) : 0;
  size_t eocd = SIZE_MAX;
  for (size_t p = data_.size() - kEocdSize + 1; p-- > min_pos;) {
    if (r.u32_at(p) == kEocdSig) {
      uint16_t comment_len = r.u16_at(p + 20);
      if (p + kEocdSize + comment_len == data_.size()) {
        eocd = p;
        break;
      }
    }
  }
  if (eocd == SIZE_MAX) throw_format("fin de répertoire central introuvable : ce n'est pas une archive ZIP valide");
  if (eocd >= 20 && r.u32_at(eocd - 20) == kZip64LocatorSig) throw_format("archives ZIP64 non prises en charge");

  r.seek(eocd + 4);
  uint16_t disk = r.u16();
  uint16_t cd_disk = r.u16();
  uint16_t disk_entries = r.u16();
  uint16_t total_entries = r.u16();
  uint32_t cd_size = r.u32();
  uint32_t cd_offset = r.u32();
  if (disk != 0 || cd_disk != 0 || disk_entries != total_entries) throw_format("archives multi-volumes non prises en charge");
  if (total_entries > limits_.max_entries) throw_format("trop d'entrées dans l'archive");
  if (uint64_t(cd_offset) + cd_size > eocd) throw_format("répertoire central hors limites");
  cd_offset_ = cd_offset;

  ByteReader cd = r.sub(cd_offset, cd_size);
  entries_.reserve(total_entries);
  for (uint32_t i = 0; i < total_entries; ++i) {
    if (cd.u32() != kCdSig) throw_format("signature d'entrée du répertoire central invalide (entrée " + std::to_string(i) + ")");
    cd.skip(4);  // version made by, version needed
    ZipEntry e;
    e.flags = cd.u16();
    e.method = cd.u16();
    cd.skip(4);  // time, date
    e.crc32 = cd.u32();
    e.compressed_size = cd.u32();
    e.uncompressed_size = cd.u32();
    uint16_t name_len = cd.u16();
    uint16_t extra_len = cd.u16();
    uint16_t comment_len = cd.u16();
    cd.skip(8);  // disk start, internal attrs, external attrs
    e.local_header_offset = cd.u32();
    auto name = cd.bytes(name_len);
    e.name.assign(name.begin(), name.end());
    cd.skip(extra_len);
    cd.skip(comment_len);

    if (e.compressed_size == 0xFFFFFFFF || e.uncompressed_size == 0xFFFFFFFF || e.local_header_offset == 0xFFFFFFFF) {
      throw_format("entrée ZIP64 non prise en charge : " + text::printable(e.name));
    }
    e.unsafe_name = is_unsafe_entry_name(e.name);
    if (e.unsafe_name) warnings_.push_back("nom d'entrée dangereux (ne sera jamais extrait) : " + text::printable(e.name));
    if (e.flags & 0x1) throw_format("entrée chiffrée non prise en charge : " + text::printable(e.name));
    if (e.uncompressed_size > limits_.max_entry_size) {
      throw_format("entrée trop volumineuse : " + text::printable(e.name) + " (" + std::to_string(e.uncompressed_size) + " octets)");
    }
    if (e.uncompressed_size > limits_.ratio_threshold &&
        e.uncompressed_size / std::max<uint64_t>(e.compressed_size, 1) > limits_.max_ratio) {
      throw_format("taux de compression suspect (bombe ZIP ?) : " + text::printable(e.name));
    }

    // Local header: validate and locate the data.
    ByteReader local(data_);
    if (e.local_header_offset > cd_offset) throw_format("en-tête local hors limites : " + text::printable(e.name));
    local.seek(e.local_header_offset);
    if (local.u32() != kLocalSig) throw_format("signature d'en-tête local invalide : " + text::printable(e.name));
    local.skip(22);
    uint16_t lname_len = local.u16();
    uint16_t lextra_len = local.u16();
    auto lname = local.bytes(lname_len);
    if (std::string(lname.begin(), lname.end()) != e.name) {
      throw_format("nom incohérent entre en-tête local et répertoire central : " + text::printable(e.name));
    }
    local.skip(lextra_len);
    e.data_offset = local.pos();
    if (e.data_offset + e.compressed_size > cd_offset) throw_format("données d'entrée hors limites : " + text::printable(e.name));

    if (!by_name_.emplace(e.name, entries_.size()).second) {
      throw_format("entrée dupliquée : " + text::printable(e.name));
    }
    entries_.push_back(std::move(e));
  }
  if (!cd.at_end()) warnings_.push_back("octets inattendus à la fin du répertoire central");

  // Entries must not overlap: overlapping entries are a classic zip-bomb
  // construction and a source of parser differentials.
  std::vector<const ZipEntry*> sorted;
  for (const auto& e : entries_) sorted.push_back(&e);
  std::sort(sorted.begin(), sorted.end(),
            [](const ZipEntry* a, const ZipEntry* b) { return a->local_header_offset < b->local_header_offset; });
  for (size_t i = 1; i < sorted.size(); ++i) {
    uint64_t prev_end = sorted[i - 1]->data_offset + sorted[i - 1]->compressed_size;
    if (sorted[i]->local_header_offset < prev_end) {
      throw_format("entrées qui se chevauchent : " + text::printable(sorted[i - 1]->name) + " / " +
                   text::printable(sorted[i]->name));
    }
  }
}

const ZipEntry* ZipArchive::find(std::string_view name) const {
  auto it = by_name_.find(std::string(name));
  return it == by_name_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> ZipArchive::read(std::string_view name) const {
  const ZipEntry* e = find(name);
  if (!e) throw_format("entrée absente : " + text::printable(name));
  return read(*e);
}

std::vector<uint8_t> ZipArchive::read(const ZipEntry& e) const {
  std::span<const uint8_t> raw(data_.data() + e.data_offset, static_cast<size_t>(e.compressed_size));
  std::vector<uint8_t> out;
  if (e.method == 0) {
    if (e.compressed_size != e.uncompressed_size) throw_format("taille incohérente pour une entrée stockée : " + text::printable(e.name));
    out.assign(raw.begin(), raw.end());
  } else if (e.method == 8) {
    try {
      out = inflate_raw(raw, static_cast<size_t>(e.uncompressed_size));
    } catch (const FormatError& err) {
      throw_format(text::printable(e.name) + " : " + err.what());
    }
  } else {
    throw_format("méthode de compression " + std::to_string(e.method) + " non prise en charge : " + text::printable(e.name));
  }
  if (hash::crc32(out) != e.crc32) throw_format("CRC invalide : " + text::printable(e.name));
  return out;
}

}  // namespace aa::apk
