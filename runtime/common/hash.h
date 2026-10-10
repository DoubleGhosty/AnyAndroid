#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace aa::hash {

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);
uint32_t adler32(std::span<const uint8_t> data);
std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::string to_hex(std::span<const uint8_t> data);

}  // namespace aa::hash
