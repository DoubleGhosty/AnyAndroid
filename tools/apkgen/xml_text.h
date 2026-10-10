#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aa::tool {

// Text XML element (subset: elements, attributes, comments, prolog; text
// content is ignored). Enough for AndroidManifest.xml and simple layouts.
struct XmlNode {
  std::string name;  // qualified, e.g. "manifest" or "android:foo"
  std::vector<std::pair<std::string, std::string>> attributes;  // qualified name -> decoded value
  std::vector<XmlNode> children;
  int line = 0;
};

// Throws std::runtime_error("ligne N : ...") on malformed input.
XmlNode parse_xml_text(std::string_view text);

}  // namespace aa::tool
