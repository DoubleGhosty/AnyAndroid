#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "resources/res_table.h"

// Compiled XML (ResXMLTree / ResXMLParser): layouts, drawables, menus read by
// XmlBlock. The tree validates every node once; the parser then walks them
// and answers with string pool indices, as libandroidfw does.
namespace aa::res {

class XmlTree {
 public:
  struct Node {
    uint16_t type = 0;  // RES_XML_* chunk type
    uint32_t line = 0;
    std::span<const uint8_t> ext;  // node extension (element, namespace, cdata)
    std::span<const uint8_t> attrs;  // start element: attribute array
    uint16_t attr_size = 0, attr_count = 0;
    uint16_t id_index = 0, class_index = 0, style_index = 0;  // 1-based, 0 = none
  };

  // Throws FormatError. `package_id` is the runtime id given to references
  // of package 0 (TYPE_DYNAMIC_REFERENCE of a shared library).
  static std::shared_ptr<XmlTree> parse(std::vector<uint8_t> bytes, uint8_t package_id = 0x7f);

  const std::shared_ptr<StringPool>& strings() const { return strings_; }
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<uint32_t>& resource_ids() const { return res_ids_; }
  uint8_t package_id() const { return package_id_; }

 private:
  Bytes bytes_;
  std::shared_ptr<StringPool> strings_;
  std::vector<uint32_t> res_ids_;
  std::vector<Node> nodes_;
  uint8_t package_id_ = 0x7f;
};

class XmlParser {
 public:
  enum Event : int32_t {
    kBadDocument = -1,
    kStartDocument = 0,
    kEndDocument = 1,
    kStartNamespace = 0x0100,
    kEndNamespace = 0x0101,
    kStartTag = 0x0102,
    kEndTag = 0x0103,
    kText = 0x0104,
  };

  XmlParser(std::shared_ptr<XmlTree> tree, uint32_t source_resid);

  int32_t next();
  int32_t event() const { return event_; }
  const std::shared_ptr<XmlTree>& tree() const { return tree_; }
  uint32_t source_resid() const { return source_resid_; }

  // String pool indices; -1 when absent.
  int32_t element_namespace() const;
  int32_t element_name() const;
  int32_t text() const;
  int32_t line() const;
  int32_t attribute_count() const;  // -1 outside a start tag
  int32_t attribute_namespace(int32_t i) const;  // -2 for a bad index
  int32_t attribute_name(int32_t i) const;
  uint32_t attribute_name_resid(int32_t i) const;  // 0 if none
  int32_t attribute_raw_value(int32_t i) const;
  // Typed value; dynamic references are resolved to plain ones.
  Value attribute_value(int32_t i) const;
  // -2 (NAME_NOT_FOUND) when absent. A null namespace matches attributes
  // without one.
  int32_t index_of_attribute(const std::u16string* ns, std::u16string_view name) const;
  int32_t index_of_id() const;
  int32_t index_of_class() const;
  int32_t index_of_style() const;
  // Attribute index whose resource id is `resid`, -1 if none.
  int32_t index_of_resid(uint32_t resid) const;

 private:
  const XmlTree::Node* node() const;
  std::span<const uint8_t> attribute(int32_t i) const;

  std::shared_ptr<XmlTree> tree_;
  uint32_t source_resid_ = 0;
  int32_t event_ = kStartDocument;
  size_t index_ = 0;
};

// Resolves TYPE_DYNAMIC_REFERENCE/ATTRIBUTE: package 0 is the owner's
// package; other ids are absolute in AnyAndroid (no shared libraries).
Value resolve_dynamic(Value v, uint8_t owner_package_id);

}  // namespace aa::res
