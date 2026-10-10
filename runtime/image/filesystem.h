#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "image/source.h"

// Read-only file systems found in Android images: ext4 (system images,
// APEX payloads) and EROFS (recent system images and APEX payloads).
namespace aa::image {

enum class FileType { kUnknown, kRegular, kDirectory, kSymlink, kOther };

struct DirEntry {
  std::string name;
  uint64_t inode = 0;
  FileType type = FileType::kUnknown;
};

struct Stat {
  uint64_t inode = 0;
  FileType type = FileType::kUnknown;
  uint64_t size = 0;
  uint32_t mode = 0;
};

class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual std::string kind() const = 0;  // "ext4", "erofs"
  virtual uint64_t root() const = 0;
  virtual Stat stat_inode(uint64_t inode) const = 0;
  virtual std::vector<DirEntry> read_dir(uint64_t inode) const = 0;
  // Content of a regular file or symlink; FormatError beyond max_size.
  virtual std::vector<uint8_t> read_inode(uint64_t inode, uint64_t max_size) const = 0;

  // Absolute paths ("/system/framework"). Symbolic links are followed inside
  // this file system, the last component only when follow_last is set.
  std::optional<Stat> stat(std::string_view path, bool follow_last = true) const;
  std::vector<DirEntry> list(std::string_view path) const;  // FormatError if not a directory
  std::vector<uint8_t> read_file(std::string_view path, uint64_t max_size) const;
  std::string read_link(uint64_t inode) const;

 private:
  std::optional<uint64_t> resolve(std::string_view path, bool follow_last, int depth) const;
};

// Detects and opens the file system stored in `src` (ext4 or EROFS).
std::unique_ptr<FileSystem> open_filesystem(std::shared_ptr<const Source> src);
// "ext4", "erofs" or "" when the source holds neither.
std::string detect_filesystem(const Source& src);

}  // namespace aa::image
