#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Random-access byte sources for Android disk images (G2, ADR-004).
//
// Images are untrusted and large (a system image is several GiB): they are
// never loaded whole. Every source is bounds-checked and raises FormatError
// on any read outside its extent.
namespace aa::image {

class Source {
 public:
  virtual ~Source() = default;
  virtual uint64_t size() const = 0;
  // Fills `out` with the bytes at [offset, offset + out.size()).
  virtual void read(uint64_t offset, std::span<uint8_t> out) const = 0;

  std::vector<uint8_t> read(uint64_t offset, size_t length) const;
  uint32_t u32_at(uint64_t offset) const;
};

// Bytes held in memory (owned).
class MemorySource final : public Source {
 public:
  explicit MemorySource(std::vector<uint8_t> data) : data_(std::move(data)) {}
  uint64_t size() const override { return data_.size(); }
  using Source::read;
  void read(uint64_t offset, std::span<uint8_t> out) const override;

 private:
  std::vector<uint8_t> data_;
};

// A file on disk, read on demand.
class FileSource final : public Source {
 public:
  explicit FileSource(const std::filesystem::path& path);
  ~FileSource() override;
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;
  uint64_t size() const override { return size_; }
  using Source::read;
  void read(uint64_t offset, std::span<uint8_t> out) const override;

 private:
  void* handle_ = nullptr;
  uint64_t size_ = 0;
};

// [offset, offset + length) of another source.
class SliceSource final : public Source {
 public:
  SliceSource(std::shared_ptr<const Source> base, uint64_t offset, uint64_t length);
  uint64_t size() const override { return length_; }
  using Source::read;
  void read(uint64_t offset, std::span<uint8_t> out) const override;

 private:
  std::shared_ptr<const Source> base_;
  uint64_t offset_;
  uint64_t length_;
};

// Concatenation of extents of a base source, with zero-filled gaps (logical
// partitions of a super image).
class ExtentSource final : public Source {
 public:
  struct Extent {
    uint64_t logical = 0;   // offset in this source
    uint64_t length = 0;
    bool zero = false;      // reads as zeros
    uint64_t physical = 0;  // offset in the base source
  };
  ExtentSource(std::shared_ptr<const Source> base, std::vector<Extent> extents);
  uint64_t size() const override { return size_; }
  using Source::read;
  void read(uint64_t offset, std::span<uint8_t> out) const override;

 private:
  std::shared_ptr<const Source> base_;
  std::vector<Extent> extents_;  // sorted, contiguous
  uint64_t size_ = 0;
};

// Android sparse image (libsparse format), expanded on the fly.
class SparseSource final : public Source {
 public:
  static constexpr uint32_t kMagic = 0xED26FF3A;
  explicit SparseSource(std::shared_ptr<const Source> base);
  uint64_t size() const override { return size_; }
  using Source::read;
  void read(uint64_t offset, std::span<uint8_t> out) const override;

 private:
  struct Chunk {
    uint64_t out_offset;
    uint64_t length;
    enum Kind : uint8_t { kRaw, kFill, kDontCare } kind;
    uint64_t in_offset;  // raw data position in the base source
    uint32_t fill;
  };
  std::shared_ptr<const Source> base_;
  std::vector<Chunk> chunks_;
  uint64_t size_ = 0;
};

// Dynamic partitions (super.img, "liblp" metadata).
struct LogicalPartition {
  std::string name;
  std::string group;
  uint32_t attributes = 0;
  uint64_t size = 0;
  std::vector<ExtentSource::Extent> extents;
};
bool is_super_image(const Source& src);
std::vector<LogicalPartition> read_super_partitions(const Source& src);

// What an image file turned out to be, and the source to read it through.
struct OpenedImage {
  std::shared_ptr<const Source> source;  // expanded (sparse → raw)
  std::vector<std::string> layers;       // e.g. {"sparse", "super:system_a", "ext4"}
};

// Opens an image file: expands a sparse image, and selects `partition` (or
// system / system_a / system_b) when the image is a super image.
OpenedImage open_image(const std::filesystem::path& path, const std::string& partition = {});

}  // namespace aa::image
