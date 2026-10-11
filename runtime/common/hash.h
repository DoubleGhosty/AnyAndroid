#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aa::hash {

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);
uint32_t adler32(std::span<const uint8_t> data);
std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::string to_hex(std::span<const uint8_t> data);

// Message digests of RFC 1321 (MD5) and FIPS 180-4 (SHA family).
enum class Algorithm : uint8_t { kMd5, kSha1, kSha224, kSha256, kSha384, kSha512 };

constexpr size_t kMaxDigestSize = 64;

// Incremental digest: update() any number of times with consecutive chunks of
// the message, then finish(). Memory use is constant whatever the message
// length (no buffering beyond one block). Copying a Digest copies its state,
// so a prefix can be hashed once and completed in several ways.
class Digest {
 public:
  explicit Digest(Algorithm algorithm);

  Algorithm algorithm() const { return algorithm_; }
  size_t size() const;        // digest length in bytes
  size_t block_size() const;  // 64 or 128
  void update(std::span<const uint8_t> data);
  // Writes size() bytes to `out` (at least that long) and starts a new message.
  void finish(std::span<uint8_t> out);
  std::vector<uint8_t> finish();
  void reset();

 private:
  void compress(const uint8_t* block);

  Algorithm algorithm_;
  uint32_t h32_[8] = {};  // MD5, SHA-1, SHA-224, SHA-256
  uint64_t h64_[8] = {};  // SHA-384, SHA-512
  uint8_t block_[128] = {};
  size_t used_ = 0;     // bytes waiting in block_
  uint64_t total_ = 0;  // message length in bytes (modulo 2^64)
};

// One-shot form of Digest.
std::vector<uint8_t> digest(Algorithm algorithm, std::span<const uint8_t> data);

}  // namespace aa::hash
