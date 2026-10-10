#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Small helpers reproducing java.lang formatting rules.
namespace aa::rt {

std::u16string u16(std::string_view utf8);
// Float.toString / Double.toString: shortest round-tripping digits, decimal
// notation for 1e-3 <= |v| < 1e7, "d.dddE±n" otherwise.
std::string java_fp_to_string(double v, bool is_float);
int32_t java_string_hash(std::u16string_view s);
// Simple case mapping (ASCII and Latin-1 Supplement only).
char16_t java_to_upper(char16_t c);
char16_t java_to_lower(char16_t c);

}  // namespace aa::rt
