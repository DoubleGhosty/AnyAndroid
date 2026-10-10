#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aa::tool {

// Raw DEFLATE (fixed Huffman codes + greedy LZ77). Deterministic output.
std::vector<uint8_t> deflate_fixed(std::span<const uint8_t> input);

// Deterministic ZIP writer (fixed timestamps) for reproducible APK fixtures.
class ZipWriter {
 public:
  struct Options {
    bool compress = true;
    // Test hooks used to fabricate malformed archives.
    bool corrupt_crc = false;
    uint64_t declared_size_override = 0;
  };

  void add(const std::string& name, std::span<const uint8_t> data, const Options& options);
  void add(const std::string& name, std::span<const uint8_t> data, bool compress = true) {
    add(name, data, Options{compress});
  }
  std::vector<uint8_t> finish();

 private:
  struct Entry {
    std::string name;
    uint16_t method;
    uint32_t crc;
    uint32_t csize;
    uint32_t usize;
    uint32_t offset;
  };
  std::vector<uint8_t> out_;
  std::vector<Entry> entries_;
};

}  // namespace aa::tool
