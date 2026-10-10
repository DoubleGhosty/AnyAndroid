#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace aa::image {

// Decodes an LZ4 block (no frame header) into at most `max_out` bytes.
// Decoding stops early once `max_out` bytes are produced (partial decoding, as
// EROFS needs); malformed input raises FormatError.
std::vector<uint8_t> lz4_decompress(std::span<const uint8_t> in, size_t max_out);

}  // namespace aa::image
