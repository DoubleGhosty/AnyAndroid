#include "apk-inspector/axml.h"

#include <cstdio>
#include <cstring>

#include "common/text.h"

namespace aa::apk {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr uint64_t kMaxDecodedPoolBytes = 64ull << 20;

// Public android.R.attr ids of the attributes AnyAndroid reads or writes.
struct AttrId {
  const char* name;
  uint32_t id;
};
constexpr AttrId kAndroidAttrs[] = {
    {"theme", 0x01010000},       {"label", 0x01010001},          {"icon", 0x01010002},
    {"name", 0x01010003},        {"permission", 0x01010006},     {"protectionLevel", 0x01010009},
    {"sharedUserId", 0x0101000b}, {"hasCode", 0x0101000c},       {"persistent", 0x0101000d},
    {"authorities", 0x01010018}, {"value", 0x01010024},          {"resource", 0x01010025},
    {"enabled", 0x0101000e},     {"debuggable", 0x0101000f},     {"exported", 0x01010010},
    {"process", 0x01010011},     {"textSize", 0x01010095},       {"textStyle", 0x01010097},
    {"textColor", 0x01010098},   {"gravity", 0x010100af},        {"orientation", 0x010100c4},
    {"id", 0x010100d0},          {"background", 0x010100d4},     {"padding", 0x010100d5},
    {"visibility", 0x010100dc},  {"layout_width", 0x010100f4},   {"layout_height", 0x010100f5},
    {"text", 0x0101014f},        {"hint", 0x01010150},           {"layout_weight", 0x01010181},
    {"minSdkVersion", 0x0101020c}, {"versionCode", 0x0101021b},  {"versionName", 0x0101021c},
    {"inputType", 0x01010220},   {"onClick", 0x0101026f},        {"targetSdkVersion", 0x01010270},
    {"maxSdkVersion", 0x01010271}, {"glEsVersion", 0x01010281},  {"required", 0x0101028e},
};

}  // namespace

uint32_t android_attr_id(std::string_view name) {
  for (const auto& a : kAndroidAttrs) {
    if (name == a.name) return a.id;
  }
  return 0;
}

std::string_view android_attr_name(uint32_t id) {
  for (const auto& a : kAndroidAttrs) {
    if (id == a.id) return a.name;
  }
  return {};
}

StringPool StringPool::parse(std::span<const uint8_t> chunk, uint32_t max_strings) {
  ByteReader r(chunk);
  uint16_t type = r.u16();
  uint16_t header_size = r.u16();
  uint32_t size = r.u32();
  if (type != kChunkStringPool) throw_format("pool de chaînes attendu");
  if (header_size < 28 || size > chunk.size() || header_size > size) throw_format("en-tête de pool de chaînes invalide");
  uint32_t count = r.u32();
  uint32_t style_count = r.u32();
  uint32_t flags = r.u32();
  uint32_t strings_start = r.u32();
  r.u32();  // styles_start
  if (count > max_strings) throw_format("trop de chaînes dans le pool (" + std::to_string(count) + ")");
  if (uint64_t(header_size) + (uint64_t(count) + style_count) * 4 > size) throw_format("table d'index du pool hors limites");
  if (count > 0 && strings_start >= size) throw_format("début des chaînes hors limites");

  const bool utf8 = (flags & 0x100) != 0;
  ByteReader pool = r.sub(0, size);
  StringPool out;
  out.strings_.reserve(count);
  uint64_t decoded_bytes = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t pos = uint64_t(strings_start) + pool.u32_at(header_size + 4ull * i);
    if (pos >= size) throw_format("chaîne " + std::to_string(i) + " hors limites");
    ByteReader s = pool.sub(static_cast<size_t>(pos), size - static_cast<size_t>(pos));
    std::string value;
    if (utf8) {
      auto read_len = [&s]() {
        uint32_t v = s.u8();
        if (v & 0x80) v = ((v & 0x7F) << 8) | s.u8();
        return v;
      };
      read_len();  // length in UTF-16 units (informational)
      uint32_t n = read_len();
      auto b = s.bytes(n);
      value.assign(b.begin(), b.end());
    } else {
      uint32_t n = s.u16();
      if (n & 0x8000) n = ((n & 0x7FFF) << 16) | s.u16();
      s.require(size_t(n) * 2, "chaîne UTF-16");
      std::u16string u;
      u.resize(n);
      for (uint32_t k = 0; k < n; ++k) u[k] = char16_t(s.u16());
      value = text::utf16_to_utf8(u);
    }
    decoded_bytes += value.size();
    if (decoded_bytes > kMaxDecodedPoolBytes) throw_format("pool de chaînes trop volumineux une fois décodé");
    out.strings_.push_back(std::move(value));
  }
  return out;
}

const std::string& StringPool::at(uint32_t index) const {
  if (index >= strings_.size()) throw_format("index de chaîne invalide " + std::to_string(index));
  return strings_[index];
}

std::string XmlAttribute::display() const {
  char buf[64];
  switch (value.type) {
    case kResString: return string_value;
    case kResReference:
      if (value.data == 0) return "@null";
      std::snprintf(buf, sizeof buf, "@0x%08x", value.data);
      return buf;
    case kResAttribute: std::snprintf(buf, sizeof buf, "?0x%08x", value.data); return buf;
    case kResIntDec: return std::to_string(int32_t(value.data));
    case kResIntHex: std::snprintf(buf, sizeof buf, "0x%x", value.data); return buf;
    case kResIntBoolean: return value.data ? "true" : "false";
    case kResFloat: {
      float f;
      std::memcpy(&f, &value.data, 4);
      std::snprintf(buf, sizeof buf, "%g", double(f));
      return buf;
    }
    case kResDimension: {
      static const float mults[4] = {1.0f / 256, 1.0f / 32768, 1.0f / 8388608, 1.0f / 2147483648.0f};
      static const char* units[] = {"px", "dp", "sp", "pt", "in", "mm"};
      float v = float(int32_t(value.data & 0xFFFFFF00)) * mults[(value.data >> 4) & 3];
      unsigned unit = value.data & 0xF;
      std::snprintf(buf, sizeof buf, "%g%s", double(v), unit < 6 ? units[unit] : "?");
      return buf;
    }
    case kResColorArgb8:
    case kResColorRgb8:
    case kResColorArgb4:
    case kResColorRgb4: std::snprintf(buf, sizeof buf, "#%08x", value.data); return buf;
    case kResNull: return raw;
    default: std::snprintf(buf, sizeof buf, "(type 0x%02x) 0x%08x", value.type, value.data); return buf;
  }
}

bool XmlAttribute::as_bool(bool fallback) const {
  if (value.type == kResIntBoolean || value.type == kResIntDec || value.type == kResIntHex) return value.data != 0;
  if (value.type == kResString) {
    if (string_value == "true") return true;
    if (string_value == "false") return false;
  }
  return fallback;
}

int64_t XmlAttribute::as_int(int64_t fallback) const {
  if (value.type == kResIntDec || value.type == kResIntHex || value.type == kResIntBoolean) return int32_t(value.data);
  if (value.type == kResString) {
    try {
      size_t used = 0;
      long long v = std::stoll(string_value, &used, 0);
      if (used == string_value.size()) return v;
    } catch (...) {
    }
  }
  return fallback;
}

const XmlAttribute* XmlElement::android_attr(std::string_view wanted) const {
  uint32_t id = android_attr_id(wanted);
  for (const auto& a : attributes) {
    if (a.ns == kAndroidNs && a.name == wanted) return &a;
  }
  if (id != 0) {
    for (const auto& a : attributes) {
      if (a.resource_id == id) return &a;
    }
  }
  return nullptr;
}

const XmlAttribute* XmlElement::plain_attr(std::string_view wanted) const {
  for (const auto& a : attributes) {
    if (a.ns.empty() && a.name == wanted) return &a;
  }
  return nullptr;
}

XmlDocument parse_axml(std::span<const uint8_t> data, const AxmlLimits& limits) {
  ByteReader r(data);
  if (r.u16() != kChunkXml) throw_format("XML binaire attendu (signature AXML absente)");
  uint16_t header_size = r.u16();
  uint32_t size = r.u32();
  if (header_size < 8 || size > data.size() || header_size > size) throw_format("en-tête AXML invalide");

  XmlDocument doc;
  StringPool pool;
  bool have_pool = false;
  std::vector<uint32_t> resource_map;
  std::vector<XmlElement*> stack;

  size_t pos = header_size;
  while (pos + 8 <= size) {
    uint16_t ctype = r.u16_at(pos);
    uint16_t chdr = r.u16_at(pos + 2);
    uint32_t csize = r.u32_at(pos + 4);
    if (csize < 8 || csize > size - pos || chdr < 8 || chdr > csize) {
      throw_format("bloc AXML invalide à " + hex(pos));
    }
    auto chunk = data.subspan(pos, csize);
    ByteReader c(chunk);
    switch (ctype) {
      case kChunkStringPool:
        pool = StringPool::parse(chunk, limits.max_strings);
        have_pool = true;
        break;
      case kChunkXmlResourceMap: {
        uint32_t n = (csize - chdr) / 4;
        resource_map.resize(n);
        for (uint32_t i = 0; i < n; ++i) resource_map[i] = c.u32_at(chdr + 4ull * i);
        break;
      }
      case kChunkXmlStartElement: {
        if (!have_pool) throw_format("élément avant le pool de chaînes");
        if (chdr < 16) throw_format("en-tête d'élément trop court");
        if (++doc.element_count > limits.max_elements) throw_format("trop d'éléments XML");
        if (stack.size() >= limits.max_depth) throw_format("imbrication XML trop profonde");
        auto el = std::make_unique<XmlElement>();
        el->line = c.u32_at(8);
        c.seek(chdr);
        uint32_t ns = c.u32();
        uint32_t name = c.u32();
        uint16_t attr_start = c.u16();
        uint16_t attr_size = c.u16();
        uint16_t attr_count = c.u16();
        el->ns = ns == kNone ? "" : pool.at(ns);
        el->name = pool.at(name);
        if (attr_count > limits.max_attributes) throw_format("trop d'attributs sur <" + text::printable(el->name) + ">");
        if (attr_count > 0 && attr_size < 20) throw_format("taille d'attribut invalide");
        for (uint32_t i = 0; i < attr_count; ++i) {
          ByteReader a = c.sub(chdr + attr_start + size_t(i) * attr_size, 20);
          XmlAttribute attr;
          uint32_t ans = a.u32();
          uint32_t aname = a.u32();
          uint32_t araw = a.u32();
          a.u16();  // Res_value.size
          a.u8();   // res0
          attr.value.type = a.u8();
          attr.value.data = a.u32();
          attr.ns = ans == kNone ? "" : pool.at(ans);
          attr.name = pool.at(aname);
          if (aname < resource_map.size()) attr.resource_id = resource_map[aname];
          if (attr.name.empty() && attr.resource_id) attr.name = std::string(android_attr_name(attr.resource_id));
          if (araw != kNone) attr.raw = pool.at(araw);
          if (attr.value.type == kResString) attr.string_value = pool.at(attr.value.data);
          el->attributes.push_back(std::move(attr));
        }
        XmlElement* raw_el = el.get();
        if (stack.empty()) {
          if (doc.root) throw_format("plusieurs éléments racine");
          doc.root = std::move(el);
        } else {
          stack.back()->children.push_back(std::move(el));
        }
        stack.push_back(raw_el);
        break;
      }
      case kChunkXmlEndElement:
        if (stack.empty()) throw_format("fin d'élément sans ouverture");
        stack.pop_back();
        break;
      default:
        break;  // namespaces, CDATA and unknown chunks carry nothing we need
    }
    pos += csize;
  }
  if (!doc.root) throw_format("document AXML vide");
  if (!stack.empty()) throw_format("document AXML tronqué (éléments non fermés)");
  return doc;
}

std::string to_text_xml(const XmlElement& el, int indent) {
  std::string pad(size_t(indent) * 2, ' ');
  std::string s = pad + "<" + el.name;
  for (const auto& a : el.attributes) {
    std::string prefix = a.ns == kAndroidNs ? "android:" : (a.ns.empty() ? "" : "{" + a.ns + "}");
    s += " " + prefix + a.name + "=\"" + text::printable(a.display()) + "\"";
  }
  if (el.children.empty()) return s + "/>\n";
  s += ">\n";
  for (const auto& c : el.children) s += to_text_xml(*c, indent + 1);
  return s + pad + "</" + el.name + ">\n";
}

}  // namespace aa::apk
