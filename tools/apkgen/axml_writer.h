#pragma once

#include <cstdint>
#include <vector>

#include "xml_text.h"

namespace aa::tool {

struct AxmlOptions {
  bool utf8 = false;  // string pool encoding (aapt uses UTF-16 for binary XML)
};

// Compiles a text XML tree into Android binary XML. Values of android:*
// attributes are typed like aapt does for literals (booleans, integers,
// strings); resource references other than @null are rejected.
std::vector<uint8_t> compile_axml(const XmlNode& root, const AxmlOptions& options = {});

// Serialises a string pool chunk (shared with tests).
std::vector<uint8_t> write_string_pool(const std::vector<std::string>& strings, bool utf8);

}  // namespace aa::tool
