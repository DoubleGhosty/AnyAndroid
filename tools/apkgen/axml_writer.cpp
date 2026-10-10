#include "axml_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>

#include "apk-inspector/axml.h"
#include "common/bytes.h"
#include "common/text.h"

namespace aa::tool {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFF;

struct Attr {
  std::string ns_uri;
  std::string name;
  uint32_t res_id = 0;
  std::string value;
  apk::ResValue typed;
  bool is_string = false;
};

struct Pool {
  std::vector<std::string> strings;
  std::map<std::string, uint32_t> index;
  uint32_t add(const std::string& s) {
    auto it = index.find(s);
    if (it != index.end()) return it->second;
    uint32_t i = uint32_t(strings.size());
    strings.push_back(s);
    index.emplace(s, i);
    return i;
  }
};

bool is_integer(const std::string& v) {
  if (v.empty()) return false;
  size_t i = v[0] == '-' ? 1 : 0;
  if (i == v.size()) return false;
  return std::all_of(v.begin() + long(i), v.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Enum values of the framework attributes AnyAndroid's layouts use.
std::optional<uint32_t> enum_value(const std::string& attr, const std::string& v) {
  if (attr == "layout_width" || attr == "layout_height") {
    if (v == "match_parent" || v == "fill_parent") return 0xFFFFFFFFu;
    if (v == "wrap_content") return 0xFFFFFFFEu;
  } else if (attr == "orientation") {
    if (v == "horizontal") return 0;
    if (v == "vertical") return 1;
  } else if (attr == "visibility") {
    if (v == "visible") return 0;
    if (v == "invisible") return 1;
    if (v == "gone") return 2;
  }
  return std::nullopt;
}

Attr make_attr(const std::map<std::string, std::string>& ns_by_prefix, const std::string& qname,
               const std::string& value, const AxmlOptions& options) {
  Attr a;
  size_t colon = qname.find(':');
  if (colon != std::string::npos) {
    std::string prefix = qname.substr(0, colon);
    auto it = ns_by_prefix.find(prefix);
    if (it == ns_by_prefix.end()) throw std::runtime_error("préfixe d'espace de noms inconnu : " + prefix);
    a.ns_uri = it->second;
    a.name = qname.substr(colon + 1);
  } else {
    a.name = qname;
  }
  a.value = value;
  bool android = a.ns_uri == apk::kAndroidNs;
  if (android) a.res_id = apk::android_attr_id(a.name);
  if (!android) {
    // aapt also resolves references of attributes without namespace
    // (<include layout="@layout/x"/>).
    auto id = !value.empty() && value[0] == '@' && options.resolve ? options.resolve(value) : std::nullopt;
    if (id) {
      a.typed = {apk::kResReference, *id};
    } else {
      a.is_string = true;
    }
  } else if (value == "true" || value == "false") {
    a.typed = {apk::kResIntBoolean, value == "true" ? 0xFFFFFFFFu : 0u};
  } else if (auto e = enum_value(a.name, value)) {
    a.typed = {apk::kResIntDec, *e};
  } else if (a.name == "layout_weight") {
    float f = std::strtof(value.c_str(), nullptr);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    a.typed = {apk::kResFloat, bits};
  } else if (is_integer(value)) {
    a.typed = {apk::kResIntDec, uint32_t(std::stol(value))};
  } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    a.typed = {apk::kResIntHex, uint32_t(std::stoul(value, nullptr, 16))};
  } else if (auto c = parse_color(value)) {
    a.typed = {c->first, c->second};
  } else if (auto d = parse_dimension(value)) {
    a.typed = {d->first, d->second};
  } else if (value == "@null") {
    a.typed = {apk::kResReference, 0};
  } else if (!value.empty() && value[0] == '@') {
    if (!options.resolve) throw std::runtime_error("références de ressources non prises en charge ici : " + value);
    auto id = options.resolve(value);
    if (!id) throw std::runtime_error("ressource inconnue : " + value);
    a.typed = {apk::kResReference, *id};
  } else if (!value.empty() && value[0] == '?') {
    throw std::runtime_error("attributs de thème (?attr) non pris en charge par apkgen : " + value);
  } else {
    a.is_string = true;
  }
  return a;
}

struct Builder {
  const AxmlOptions& options;
  Pool pool;
  ByteWriter body;
  std::map<std::string, std::string> ns_by_prefix;
  std::vector<std::pair<std::string, std::string>> namespaces;  // prefix, uri

  void collect_namespaces(const XmlNode& n) {
    for (const auto& [k, v] : n.attributes) {
      if (text::starts_with(k, "xmlns:")) {
        std::string prefix = k.substr(6);
        if (!ns_by_prefix.count(prefix)) {
          ns_by_prefix[prefix] = v;
          namespaces.emplace_back(prefix, v);
        }
      }
    }
    for (const auto& c : n.children) collect_namespaces(c);
  }

  std::vector<Attr> attrs_of(const XmlNode& n) {
    std::vector<Attr> out;
    for (const auto& [k, v] : n.attributes) {
      if (text::starts_with(k, "xmlns")) continue;
      out.push_back(make_attr(ns_by_prefix, k, v, options));
    }
    std::stable_sort(out.begin(), out.end(), [](const Attr& x, const Attr& y) {
      if ((x.res_id != 0) != (y.res_id != 0)) return x.res_id != 0;
      return x.res_id < y.res_id;
    });
    return out;
  }

  // First pass: resource attribute names must occupy the first pool slots so
  // that the resource map can be indexed by string index.
  void collect_res_names(const XmlNode& n, std::map<uint32_t, std::string>& res) {
    for (const auto& a : attrs_of(n)) {
      if (a.res_id) res[a.res_id] = a.name;
    }
    for (const auto& c : n.children) collect_res_names(c, res);
  }

  void node_header(uint16_t type, uint32_t size, int line) {
    body.u16(type);
    body.u16(16);
    body.u32(size);
    body.u32(uint32_t(line));
    body.u32(kNone);
  }

  void element(const XmlNode& n) {
    std::string name = n.name;
    std::string ns_uri;
    if (size_t colon = name.find(':'); colon != std::string::npos) {
      ns_uri = ns_by_prefix.at(name.substr(0, colon));
      name = name.substr(colon + 1);
    }
    std::vector<Attr> attrs = attrs_of(n);
    uint32_t size = 16 + 20 + 20 * uint32_t(attrs.size());
    node_header(apk::kChunkXmlStartElement, size, n.line);
    body.u32(ns_uri.empty() ? kNone : pool.add(ns_uri));
    body.u32(pool.add(name));
    body.u16(20);
    body.u16(20);
    body.u16(uint16_t(attrs.size()));
    uint16_t id_index = 0;
    for (size_t i = 0; i < attrs.size(); ++i) {
      if (attrs[i].name == "id" && attrs[i].ns_uri == apk::kAndroidNs) id_index = uint16_t(i + 1);
    }
    body.u16(id_index);
    body.u16(0);
    body.u16(0);
    for (const Attr& a : attrs) {
      body.u32(a.ns_uri.empty() ? kNone : pool.add(a.ns_uri));
      body.u32(pool.add(a.name));
      if (a.is_string) {
        uint32_t s = pool.add(a.value);
        body.u32(s);
        body.u16(8);
        body.u8(0);
        body.u8(apk::kResString);
        body.u32(s);
      } else {
        body.u32(kNone);
        body.u16(8);
        body.u8(0);
        body.u8(a.typed.type);
        body.u32(a.typed.data);
      }
    }
    for (const auto& c : n.children) element(c);
    node_header(apk::kChunkXmlEndElement, 16 + 8, n.line);
    body.u32(ns_uri.empty() ? kNone : pool.add(ns_uri));
    body.u32(pool.add(name));
  }
};

}  // namespace

std::optional<std::pair<uint8_t, uint32_t>> parse_color(std::string_view v) {
  if (v.size() < 2 || v[0] != '#') return std::nullopt;
  uint32_t x = 0;
  for (char c : v.substr(1)) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return std::nullopt;
    x = (x << 4) | uint32_t(d);
  }
  auto nibbles = [](uint32_t n) { return (n << 4) | n; };
  switch (v.size() - 1) {
    case 3:  // #RGB
      return std::pair{apk::kResColorRgb4,
                       0xFF000000u | nibbles((x >> 8) & 0xF) << 16 | nibbles((x >> 4) & 0xF) << 8 | nibbles(x & 0xF)};
    case 4:  // #ARGB
      return std::pair{apk::kResColorArgb4, nibbles(x >> 12) << 24 | nibbles((x >> 8) & 0xF) << 16 |
                                                nibbles((x >> 4) & 0xF) << 8 | nibbles(x & 0xF)};
    case 6: return std::pair{apk::kResColorRgb8, 0xFF000000u | x};
    case 8: return std::pair{apk::kResColorArgb8, x};
    default: return std::nullopt;
  }
}

std::optional<std::pair<uint8_t, uint32_t>> parse_dimension(std::string_view v) {
  static constexpr std::pair<const char*, uint32_t> kUnits[] = {
      {"px", 0}, {"dip", 1}, {"dp", 1}, {"sp", 2}, {"pt", 3}, {"in", 4}, {"mm", 5}};
  for (const auto& [suffix, unit] : kUnits) {
    std::string_view s(suffix);
    if (v.size() <= s.size() || v.substr(v.size() - s.size()) != s) continue;
    std::string number(v.substr(0, v.size() - s.size()));
    char* end = nullptr;
    double d = std::strtod(number.c_str(), &end);
    if (end != number.c_str() + number.size() || !std::isfinite(d) || std::fabs(d) >= 8388608.0) return std::nullopt;
    // Res_value complex: 24-bit mantissa, radix (23p0, 16p7, 8p15, 0p23), unit.
    double a = std::fabs(d);
    uint32_t radix = a == std::floor(a) ? 0 : a < 1 ? 3 : a < 256 ? 2 : a < 65536 ? 1 : 0;
    static constexpr int kShift[] = {0, 7, 15, 23};
    int64_t mantissa = std::llround(d * double(1 << kShift[radix]));
    return std::pair{apk::kResDimension, (uint32_t(mantissa) & 0xFFFFFFu) << 8 | radix << 4 | unit};
  }
  return std::nullopt;
}

std::vector<uint8_t> write_string_pool(const std::vector<std::string>& strings, bool utf8) {
  ByteWriter data;
  std::vector<uint32_t> offsets;
  for (const auto& s : strings) {
    offsets.push_back(uint32_t(data.size()));
    std::u16string u = text::utf8_to_utf16(s);
    if (utf8) {
      auto len = [&](size_t n) {
        if (n > 0x7FFF) throw std::runtime_error("chaîne trop longue pour le pool");
        if (n > 0x7F) data.u8(uint8_t(0x80 | (n >> 8)));
        data.u8(uint8_t(n & 0xFF));
      };
      len(u.size());
      len(s.size());
      data.bytes(std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
      data.u8(0);
    } else {
      if (u.size() > 0x7FFF) {
        data.u16(uint16_t(0x8000 | (u.size() >> 16)));
        data.u16(uint16_t(u.size() & 0xFFFF));
      } else {
        data.u16(uint16_t(u.size()));
      }
      for (char16_t c : u) data.u16(c);
      data.u16(0);
    }
  }
  data.align(4);
  ByteWriter w;
  uint32_t header = 28;
  uint32_t strings_start = header + 4 * uint32_t(strings.size());
  w.u16(apk::kChunkStringPool);
  w.u16(uint16_t(header));
  w.u32(strings_start + uint32_t(data.size()));
  w.u32(uint32_t(strings.size()));
  w.u32(0);
  w.u32(utf8 ? 0x100 : 0);
  w.u32(strings_start);
  w.u32(0);
  for (uint32_t o : offsets) w.u32(o);
  w.bytes(data.buffer());
  return w.take();
}

std::vector<uint8_t> compile_axml(const XmlNode& root, const AxmlOptions& options) {
  Builder b{options};
  b.collect_namespaces(root);
  std::map<uint32_t, std::string> res;
  b.collect_res_names(root, res);
  std::vector<uint32_t> res_ids;
  for (const auto& [id, name] : res) {
    if (b.pool.index.count(name)) continue;  // same name with two ids cannot happen in practice
    b.pool.add(name);
    res_ids.push_back(id);
  }

  for (const auto& [prefix, uri] : b.namespaces) {
    b.node_header(apk::kChunkXmlStartNamespace, 24, root.line);
    b.body.u32(b.pool.add(prefix));
    b.body.u32(b.pool.add(uri));
  }
  b.element(root);
  for (auto it = b.namespaces.rbegin(); it != b.namespaces.rend(); ++it) {
    b.node_header(apk::kChunkXmlEndNamespace, 24, root.line);
    b.body.u32(b.pool.add(it->first));
    b.body.u32(b.pool.add(it->second));
  }

  std::vector<uint8_t> pool = write_string_pool(b.pool.strings, options.utf8);
  ByteWriter resmap;
  if (!res_ids.empty()) {
    resmap.u16(apk::kChunkXmlResourceMap);
    resmap.u16(8);
    resmap.u32(8 + 4 * uint32_t(res_ids.size()));
    for (uint32_t id : res_ids) resmap.u32(id);
  }
  ByteWriter out;
  out.u16(apk::kChunkXml);
  out.u16(8);
  out.u32(uint32_t(8 + pool.size() + resmap.size() + b.body.size()));
  out.bytes(pool);
  out.bytes(resmap.buffer());
  out.bytes(b.body.buffer());
  return out.take();
}

}  // namespace aa::tool
