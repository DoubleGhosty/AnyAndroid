#pragma once

#include <cstdint>

// Simple (one-to-one) Unicode case mapping, independent of the C library's
// locale: in the "C" locale towupper/towlower only map ASCII. Built once from
// the system's Unicode tables (LCMapStringEx, invariant locale) for the BMP;
// supplementary code points are returned unchanged.
namespace aa::unicode {

char32_t to_lower(char32_t c);
char32_t to_upper(char32_t c);
// Simple case folding: code points equal ignoring case fold to the same value.
inline char32_t fold(char32_t c) { return to_lower(to_upper(c)); }

}  // namespace aa::unicode
