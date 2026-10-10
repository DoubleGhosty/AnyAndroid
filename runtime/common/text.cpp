#include "common/text.h"

#include "common/bytes.h"

namespace aa::text {

namespace {

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

void append_utf16(std::u16string& out, uint32_t cp) {
  if (cp < 0x10000) {
    out.push_back(char16_t(cp));
  } else {
    cp -= 0x10000;
    out.push_back(char16_t(0xD800 + (cp >> 10)));
    out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
  }
}

}  // namespace

std::u16string utf8_to_utf16(std::string_view in) {
  std::u16string out;
  out.reserve(in.size());
  size_t i = 0;
  const auto* s = reinterpret_cast<const uint8_t*>(in.data());
  while (i < in.size()) {
    uint8_t b0 = s[i];
    uint32_t cp;
    size_t len;
    if (b0 < 0x80) {
      cp = b0;
      len = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
      cp = b0 & 0x1F;
      len = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
      cp = b0 & 0x0F;
      len = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
      cp = b0 & 0x07;
      len = 4;
    } else {
      out.push_back(u'�');
      ++i;
      continue;
    }
    if (i + len > in.size()) {
      out.push_back(u'�');
      ++i;
      continue;
    }
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      if ((s[i + k] & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
      out.push_back(u'�');
      ++i;
      continue;
    }
    append_utf16(out, cp);
    i += len;
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    uint32_t c = in[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      append_utf8(out, cp);
      ++i;
    } else {
      append_utf8(out, c);  // includes lone surrogates (WTF-8)
    }
  }
  return out;
}

std::u16string mutf8_to_utf16(std::span<const uint8_t> in) {
  std::u16string out;
  size_t i = 0;
  while (true) {
    if (i >= in.size()) throw_format("chaîne MUTF-8 non terminée");
    uint8_t b0 = in[i];
    if (b0 == 0) return out;
    if (b0 < 0x80) {
      out.push_back(char16_t(b0));
      i += 1;
    } else if ((b0 & 0xE0) == 0xC0) {
      if (i + 1 >= in.size()) throw_format("chaîne MUTF-8 tronquée");
      out.push_back(char16_t(((b0 & 0x1F) << 6) | (in[i + 1] & 0x3F)));
      i += 2;
    } else if ((b0 & 0xF0) == 0xE0) {
      if (i + 2 >= in.size()) throw_format("chaîne MUTF-8 tronquée");
      out.push_back(char16_t(((b0 & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F)));
      i += 3;
    } else {
      throw_format("octet MUTF-8 invalide " + hex(b0));
    }
  }
}

#ifdef _WIN32
std::wstring to_wide(std::string_view utf8) { return u16_to_wide(utf8_to_utf16(utf8)); }
std::string from_wide(std::wstring_view wide) { return utf16_to_utf8(wide_to_u16(wide)); }
std::wstring u16_to_wide(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
std::u16string wide_to_u16(std::wstring_view s) { return std::u16string(s.begin(), s.end()); }
#endif

std::string printable(std::string_view s, size_t max_len) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (out.size() >= max_len) {
      out += "…";
      break;
    }
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7f) {
      static const char* digits = "0123456789abcdef";
      out += "\\x";
      out.push_back(digits[c >> 4]);
      out.push_back(digits[c & 15]);
    } else {
      out.push_back(char(c));
    }
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t p = s.find(sep, start);
    if (p == std::string_view::npos) {
      out.emplace_back(s.substr(start));
      return out;
    }
    out.emplace_back(s.substr(start, p - start));
    start = p + 1;
  }
}

std::string descriptor_to_java(std::string_view d) {
  size_t dims = 0;
  while (dims < d.size() && d[dims] == '[') ++dims;
  std::string_view base = d.substr(dims);
  std::string name;
  if (base.size() >= 2 && base.front() == 'L' && base.back() == ';') {
    name = std::string(base.substr(1, base.size() - 2));
    for (char& c : name) {
      if (c == '/') c = '.';
    }
  } else if (base.size() == 1) {
    switch (base[0]) {
      case 'V': name = "void"; break;
      case 'Z': name = "boolean"; break;
      case 'B': name = "byte"; break;
      case 'S': name = "short"; break;
      case 'C': name = "char"; break;
      case 'I': name = "int"; break;
      case 'J': name = "long"; break;
      case 'F': name = "float"; break;
      case 'D': name = "double"; break;
      default: name = std::string(base);
    }
  } else {
    name = std::string(base);
  }
  for (size_t i = 0; i < dims; ++i) name += "[]";
  return name;
}

std::string java_to_descriptor(std::string_view java_name) {
  std::string d = "L";
  for (char c : java_name) d.push_back(c == '.' ? '/' : c);
  d.push_back(';');
  return d;
}

}  // namespace aa::text
