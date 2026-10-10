#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace aa {

// Raised when untrusted input does not match the expected format. Parsers
// throw it instead of reading out of bounds; callers turn it into a
// user-facing diagnostic.
class FormatError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void throw_format(const std::string& message);

inline uint16_t load_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t load_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Bounds-checked little-endian reader over an immutable buffer.
class ByteReader {
 public:
  ByteReader() = default;
  explicit ByteReader(std::span<const uint8_t> data, size_t pos = 0);

  size_t size() const { return data_.size(); }
  size_t pos() const { return pos_; }
  size_t remaining() const { return data_.size() - pos_; }
  bool at_end() const { return pos_ >= data_.size(); }
  std::span<const uint8_t> data() const { return data_; }

  void seek(size_t pos);
  void skip(size_t n);
  void require(size_t n, const char* what) const;

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int32_t s32() { return static_cast<int32_t>(u32()); }
  uint32_t uleb128();
  int32_t sleb128();
  std::span<const uint8_t> bytes(size_t n);

  // Absolute reads that do not move the cursor.
  uint16_t u16_at(size_t off) const;
  uint32_t u32_at(size_t off) const;
  // Reader restricted to [off, off + len) of this buffer.
  ByteReader sub(size_t off, size_t len) const;

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

// Growable little-endian writer, used by the tooling (APK/DEX generation).
class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void uleb128(uint32_t v);
  void sleb128(int32_t v);
  void bytes(std::span<const uint8_t> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
  void zeros(size_t n) { buf_.insert(buf_.end(), n, 0); }
  void align(size_t n);

  size_t size() const { return buf_.size(); }
  void patch_u16(size_t off, uint16_t v);
  void patch_u32(size_t off, uint32_t v);

  std::vector<uint8_t>& buffer() { return buf_; }
  const std::vector<uint8_t>& buffer() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

std::string hex(uint64_t v, int digits = 0);

}  // namespace aa
