#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "xml_text.h"

namespace aa::tool {

struct AxmlOptions {
  bool utf8 = false;  // string pool encoding (aapt uses UTF-16 for binary XML)
  // Resource id of a reference ("@string/app_name", "@+id/ok"); nullopt when
  // unknown. Without a resolver, references other than @null are rejected.
  std::function<std::optional<uint32_t>(std::string_view ref)> resolve;
};

// Compiles a text XML tree into Android binary XML. Values of android:*
// attributes are typed like aapt does for literals: booleans, integers,
// colors (#RGB...), dimensions (16dp), the enums of layout_width/height,
// orientation and visibility, and resource references through the resolver.
std::vector<uint8_t> compile_axml(const XmlNode& root, const AxmlOptions& options = {});

// Res_value of a typed literal: color "#AARRGGBB" (and short forms) or
// dimension "12.5sp"; nullopt when `v` is neither.
std::optional<std::pair<uint8_t, uint32_t>> parse_color(std::string_view v);
std::optional<std::pair<uint8_t, uint32_t>> parse_dimension(std::string_view v);

// Serialises a string pool chunk (shared with tests).
std::vector<uint8_t> write_string_pool(const std::vector<std::string>& strings, bool utf8);

}  // namespace aa::tool
