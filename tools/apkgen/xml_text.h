#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aa::tool {

// Text XML element (subset: elements, attributes, comments, prolog and text
// content). Enough for AndroidManifest.xml, layouts and values resources.
struct XmlNode {
  std::string name;  // qualified, e.g. "manifest" or "android:foo"
  std::vector<std::pair<std::string, std::string>> attributes;  // qualified name -> decoded value
  std::vector<XmlNode> children;
  std::string text;  // character data directly inside the element, entities decoded
  int line = 0;
};

// Throws std::runtime_error("ligne N : ...") on malformed input.
XmlNode parse_xml_text(std::string_view text);

}  // namespace aa::tool
