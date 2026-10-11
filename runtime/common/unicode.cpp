#include "common/unicode.h"

#include <array>
#include <cwctype>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aa::unicode {

namespace {

struct Tables {
  std::array<char16_t, 0x10000> lower;
  std::array<char16_t, 0x10000> upper;
};

void fill(std::array<char16_t, 0x10000>& out, bool upper) {
  for (uint32_t c = 0; c < 0x10000; ++c) out[c] = char16_t(c);
#ifdef _WIN32
  // One call over the whole BMP; surrogates (meaningless alone) stay as is.
  std::vector<wchar_t> in(0x10000), mapped(0x10000);
  for (uint32_t c = 0; c < 0x10000; ++c) in[c] = (c >= 0xD800 && c <= 0xDFFF) ? L' ' : wchar_t(c);
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE, in.data(), 0x10000,
                        mapped.data(), 0x10000, nullptr, nullptr, 0);
  if (n == 0x10000) {
    for (uint32_t c = 0; c < 0x10000; ++c) {
      if (c < 0xD800 || c > 0xDFFF) out[c] = char16_t(mapped[c]);
    }
    return;
  }
#endif
  for (uint32_t c = 0; c < 0x10000; ++c) {
    if (c < 0xD800 || c > 0xDFFF) out[c] = char16_t(upper ? std::towupper(wint_t(c)) : std::towlower(wint_t(c)));
  }
}

const Tables& tables() {
  static const Tables* t = [] {
    auto* x = new Tables;  // never destroyed: used until the process ends
    fill(x->lower, false);
    fill(x->upper, true);
    return x;
  }();
  return *t;
}

}  // namespace

char32_t to_lower(char32_t c) { return c <= 0xFFFF ? tables().lower[c] : c; }
char32_t to_upper(char32_t c) { return c <= 0xFFFF ? tables().upper[c] : c; }

}  // namespace aa::unicode
