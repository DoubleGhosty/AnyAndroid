#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::apk {

// Decompresses a raw DEFLATE stream (RFC 1951, no zlib header). The output
// must be exactly `expected_size` bytes: anything longer is refused before it
// is produced, which bounds memory use on hostile archives. Throws
// FormatError on any malformed input.
std::vector<uint8_t> inflate_raw(std::span<const uint8_t> input, size_t expected_size);

}  // namespace aa::apk
