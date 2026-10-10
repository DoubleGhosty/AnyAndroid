#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aa::apk {

// Limits applied to untrusted archives. Defaults are generous for real APKs
// while keeping hostile inputs (zip bombs, huge directories) bounded.
struct ZipLimits {
  uint64_t max_archive_size = 1ull << 30;     // 1 GiB
  uint32_t max_entries = 65535;
  uint64_t max_entry_size = 512ull << 20;     // uncompressed, per entry
  uint32_t max_ratio = 1000;                  // compression ratio refused above...
  uint64_t ratio_threshold = 1ull << 20;      // ...for entries larger than 1 MiB
};

struct ZipEntry {
  std::string name;
  uint16_t method = 0;  // 0 = stored, 8 = deflate
  uint16_t flags = 0;
  uint32_t crc32 = 0;
  uint64_t compressed_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t local_header_offset = 0;
  uint64_t data_offset = 0;
  // Name that would escape an extraction directory ("..", absolute, drive
  // letter, backslash, NUL). Such entries are never extracted.
  bool unsafe_name = false;
};

// Read-only view of a ZIP archive held in memory. Opening validates the
// central directory and every local header; reading an entry decompresses
// it, checks its CRC and enforces the size limits.
class ZipArchive {
 public:
  static std::unique_ptr<ZipArchive> open(std::vector<uint8_t> data, const ZipLimits& limits = {});

  const std::vector<ZipEntry>& entries() const { return entries_; }
  const ZipEntry* find(std::string_view name) const;
  std::vector<uint8_t> read(const ZipEntry& entry) const;
  std::vector<uint8_t> read(std::string_view name) const;
  const std::vector<std::string>& warnings() const { return warnings_; }
  std::span<const uint8_t> raw() const { return data_; }
  uint64_t central_directory_offset() const { return cd_offset_; }

 private:
  ZipArchive() = default;
  void parse();

  std::vector<uint8_t> data_;
  ZipLimits limits_;
  uint64_t cd_offset_ = 0;
  std::vector<ZipEntry> entries_;
  std::unordered_map<std::string, size_t> by_name_;
  std::vector<std::string> warnings_;
};

bool is_unsafe_entry_name(std::string_view name);

}  // namespace aa::apk
