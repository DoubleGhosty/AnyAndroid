#include "resources/res_xml.h"

#include "common/bytes.h"

namespace aa::res {

namespace {

constexpr uint16_t kChunkStringPool = 0x0001;
constexpr uint16_t kChunkXml = 0x0003;
constexpr uint16_t kXmlFirstNode = 0x0100;
constexpr uint16_t kXmlLastNode = 0x017f;
constexpr uint16_t kXmlStartNamespace = 0x0100;
constexpr uint16_t kXmlEndNamespace = 0x0101;
constexpr uint16_t kXmlStartElement = 0x0102;
constexpr uint16_t kXmlEndElement = 0x0103;
constexpr uint16_t kXmlCdata = 0x0104;
constexpr uint16_t kXmlResourceMap = 0x0180;
constexpr size_t kAttributeSize = 20;  // ResXMLTree_attribute
constexpr int32_t kNameNotFound = -2;

int32_t as_index(uint32_t v) { return v == 0xffffffffu ? -1 : int32_t(v); }

}  // namespace

Value resolve_dynamic(Value v, uint8_t owner_package_id) {
  if (v.type == kTypeDynamicReference || v.type == kTypeDynamicAttribute) {
    v.type = v.type == kTypeDynamicReference ? kTypeReference : kTypeAttribute;
    if (v.data != 0 && (v.data >> 24) == 0) v.data |= uint32_t(owner_package_id) << 24;
  }
  return v;
}

std::shared_ptr<XmlTree> XmlTree::parse(std::vector<uint8_t> bytes, uint8_t package_id) {
  auto tree = std::shared_ptr<XmlTree>(new XmlTree());
  tree->package_id_ = package_id;
  tree->bytes_ = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
  std::span<const uint8_t> all(*tree->bytes_);
  if (all.size() < 8) throw_format("XML compilé tronqué");
  uint16_t type = load_u16(all.data());
  uint16_t header = load_u16(all.data() + 2);
  uint32_t size = load_u32(all.data() + 4);
  if (type != kChunkXml || header < 8 || size < header || size > all.size()) throw_format("en-tête XML compilé invalide");
  all = all.first(size);

  bool root_seen = false;
  for (size_t pos = header; pos < all.size();) {
    if (pos + 8 > all.size()) throw_format("bloc XML tronqué");
    uint16_t ctype = load_u16(all.data() + pos);
    uint16_t chead = load_u16(all.data() + pos + 2);
    uint32_t csize = load_u32(all.data() + pos + 4);
    if (chead < 8 || csize < chead || csize > all.size() - pos || ((chead | csize) & 3)) throw_format("bloc XML invalide");
    std::span<const uint8_t> c = all.subspan(pos, csize);
    if (!root_seen) {
      if (ctype == kChunkStringPool && !tree->strings_) {
        tree->strings_ = StringPool::parse(tree->bytes_, pos, csize);
      } else if (ctype == kXmlResourceMap) {
        for (size_t i = chead; i + 4 <= csize; i += 4) tree->res_ids_.push_back(load_u32(c.data() + i));
      }
    }
    if (ctype >= kXmlFirstNode && ctype <= kXmlLastNode) {
      if (!tree->strings_) throw_format("XML compilé sans pool de chaînes");
      root_seen = true;
      if (chead < 16) throw_format("nœud XML trop court");
      Node n;
      n.type = ctype;
      n.line = load_u32(c.data() + 8);
      n.ext = c.subspan(chead);
      switch (ctype) {
        case kXmlStartNamespace:
        case kXmlEndNamespace:
        case kXmlEndElement:
          if (n.ext.size() < 8) throw_format("nœud XML tronqué");
          break;
        case kXmlCdata:
          if (n.ext.size() < 12) throw_format("texte XML tronqué");
          break;
        case kXmlStartElement: {
          if (n.ext.size() < 20) throw_format("élément XML tronqué");
          uint16_t start = load_u16(n.ext.data() + 8);
          n.attr_size = load_u16(n.ext.data() + 10);
          n.attr_count = load_u16(n.ext.data() + 12);
          n.id_index = load_u16(n.ext.data() + 14);
          n.class_index = load_u16(n.ext.data() + 16);
          n.style_index = load_u16(n.ext.data() + 18);
          if (n.attr_count && n.attr_size < kAttributeSize) throw_format("attributs XML trop courts");
          if (size_t(start) + size_t(n.attr_size) * n.attr_count > n.ext.size()) throw_format("attributs XML hors limites");
          n.attrs = n.ext.subspan(start, size_t(n.attr_size) * n.attr_count);
          break;
        }
        default:
          pos += csize;
          continue;  // unknown node: skipped, as ResXMLParser does
      }
      tree->nodes_.push_back(n);
    }
    pos += csize;
  }
  if (!root_seen) throw_format("XML compilé sans nœud racine");
  return tree;
}

XmlParser::XmlParser(std::shared_ptr<XmlTree> tree, uint32_t source_resid)
    : tree_(std::move(tree)), source_resid_(source_resid) {}

int32_t XmlParser::next() {
  if (event_ == kStartDocument) {
    index_ = 0;
  } else if (event_ >= kStartNamespace) {
    ++index_;
  } else {
    return event_;
  }
  event_ = index_ < tree_->nodes().size() ? int32_t(tree_->nodes()[index_].type) : kEndDocument;
  return event_;
}

const XmlTree::Node* XmlParser::node() const {
  if (event_ < kStartNamespace || index_ >= tree_->nodes().size()) return nullptr;
  return &tree_->nodes()[index_];
}

int32_t XmlParser::element_namespace() const {
  const XmlTree::Node* n = node();
  if (!n || (event_ != kStartTag && event_ != kEndTag)) return -1;
  return as_index(load_u32(n->ext.data()));
}

int32_t XmlParser::element_name() const {
  const XmlTree::Node* n = node();
  if (!n || (event_ != kStartTag && event_ != kEndTag)) return -1;
  return as_index(load_u32(n->ext.data() + 4));
}

int32_t XmlParser::text() const {
  const XmlTree::Node* n = node();
  if (!n || event_ != kText) return -1;
  return as_index(load_u32(n->ext.data()));
}

int32_t XmlParser::line() const {
  const XmlTree::Node* n = node();
  return n ? int32_t(n->line) : -1;
}

int32_t XmlParser::attribute_count() const {
  const XmlTree::Node* n = node();
  return n && event_ == kStartTag ? n->attr_count : 0;
}

std::span<const uint8_t> XmlParser::attribute(int32_t i) const {
  const XmlTree::Node* n = node();
  if (!n || event_ != kStartTag || i < 0 || i >= n->attr_count) return {};
  return n->attrs.subspan(size_t(i) * n->attr_size, kAttributeSize);
}

int32_t XmlParser::attribute_namespace(int32_t i) const {
  auto a = attribute(i);
  return a.empty() ? -2 : as_index(load_u32(a.data()));
}

int32_t XmlParser::attribute_name(int32_t i) const {
  auto a = attribute(i);
  return a.empty() ? -1 : as_index(load_u32(a.data() + 4));
}

uint32_t XmlParser::attribute_name_resid(int32_t i) const {
  int32_t name = attribute_name(i);
  if (name < 0 || size_t(name) >= tree_->resource_ids().size()) return 0;
  uint32_t id = tree_->resource_ids()[size_t(name)];
  if (id != 0 && (id >> 24) == 0) id |= uint32_t(tree_->package_id()) << 24;
  return id;
}

int32_t XmlParser::attribute_raw_value(int32_t i) const {
  auto a = attribute(i);
  return a.empty() ? -1 : as_index(load_u32(a.data() + 8));
}

Value XmlParser::attribute_value(int32_t i) const {
  auto a = attribute(i);
  if (a.empty()) return {};
  return resolve_dynamic(Value{a[15], load_u32(a.data() + 16)}, tree_->package_id());
}

int32_t XmlParser::index_of_attribute(const std::u16string* ns, std::u16string_view name) const {
  int32_t count = attribute_count();
  const StringPool& strings = *tree_->strings();
  for (int32_t i = 0; i < count; ++i) {
    int32_t n = attribute_name(i);
    auto attr_name = n >= 0 ? strings.string16(uint32_t(n)) : std::nullopt;
    if (!attr_name || *attr_name != name) continue;
    int32_t ans = attribute_namespace(i);
    auto attr_ns = ans >= 0 ? strings.string16(uint32_t(ans)) : std::nullopt;
    if (!ns) {
      if (!attr_ns) return i;
    } else if (attr_ns && *attr_ns == *ns) {
      return i;
    }
  }
  return kNameNotFound;
}

int32_t XmlParser::index_of_id() const {
  const XmlTree::Node* n = node();
  return n && event_ == kStartTag && n->id_index > 0 ? n->id_index - 1 : kNameNotFound;
}

int32_t XmlParser::index_of_class() const {
  const XmlTree::Node* n = node();
  return n && event_ == kStartTag && n->class_index > 0 ? n->class_index - 1 : kNameNotFound;
}

int32_t XmlParser::index_of_style() const {
  const XmlTree::Node* n = node();
  return n && event_ == kStartTag && n->style_index > 0 ? n->style_index - 1 : kNameNotFound;
}

int32_t XmlParser::index_of_resid(uint32_t resid) const {
  int32_t count = attribute_count();
  for (int32_t i = 0; i < count; ++i) {
    if (attribute_name_resid(i) == resid) return i;
  }
  return -1;
}

}  // namespace aa::res
