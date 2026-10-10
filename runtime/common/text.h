#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aa::text {

// UTF-8 <-> UTF-16. Unpaired surrogates survive the round trip (they are
// encoded as 3-byte sequences, "WTF-8"), so Java strings stay lossless.
// Invalid UTF-8 bytes decode to U+FFFD.
std::u16string utf8_to_utf16(std::string_view in);
std::string utf16_to_utf8(std::u16string_view in);

// Decodes a NUL-terminated Modified UTF-8 string (DEX string_data_item) into
// UTF-16 code units. Throws FormatError if the terminator is missing.
std::u16string mutf8_to_utf16(std::span<const uint8_t> in);

#ifdef _WIN32
std::wstring to_wide(std::string_view utf8);
std::string from_wide(std::wstring_view wide);
std::wstring u16_to_wide(std::u16string_view s);
std::u16string wide_to_u16(std::wstring_view s);
#endif

// Escapes control characters so untrusted strings are safe to print.
std::string printable(std::string_view s, size_t max_len = 200);

bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);

// "Lcom/example/Foo;" -> "com.example.Foo", "[I" -> "int[]".
std::string descriptor_to_java(std::string_view descriptor);
// "com.example.Foo" -> "Lcom/example/Foo;".
std::string java_to_descriptor(std::string_view java_name);

}  // namespace aa::text
