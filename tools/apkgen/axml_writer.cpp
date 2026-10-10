#include "axml_writer.h"

#include <algorithm>
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

Attr make_attr(const std::map<std::string, std::string>& ns_by_prefix, const std::string& qname,
               const std::string& value) {
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
  if (android && (value == "true" || value == "false")) {
    a.typed = {apk::kResIntBoolean, value == "true" ? 0xFFFFFFFFu : 0u};
  } else if (android && is_integer(value)) {
    a.typed = {apk::kResIntDec, uint32_t(std::stol(value))};
  } else if (android && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    a.typed = {apk::kResIntHex, uint32_t(std::stoul(value, nullptr, 16))};
  } else if (android && value == "@null") {
    a.typed = {apk::kResReference, 0};
  } else if (android && !value.empty() && (value[0] == '@' || value[0] == '?')) {
    throw std::runtime_error("références de ressources non prises en charge par apkgen : " + value);
  } else {
    a.is_string = true;
  }
  return a;
}

struct Builder {
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
      out.push_back(make_attr(ns_by_prefix, k, v));
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
  Builder b;
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
