#include "res_compiler.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "apk-inspector/axml.h"
#include "axml_writer.h"
#include "common/bytes.h"
#include "common/file_io.h"
#include "common/text.h"

namespace fs = std::filesystem;

namespace aa::tool {

namespace {

// Type ids follow this order (only the types present get one).
constexpr const char* kTypeOrder[] = {"id", "layout", "string", "color", "dimen", "integer", "bool"};
constexpr uint32_t kConfigLocale = 0x0004;  // ResTable_config::CONFIG_LOCALE
constexpr uint32_t kConfigSize = 64;

bool known_type(std::string_view t) {
  return std::find(std::begin(kTypeOrder), std::end(kTypeOrder), t) != std::end(kTypeOrder);
}

bool valid_name(std::string_view n) {
  if (n.empty()) return false;
  return std::all_of(n.begin(), n.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
  });
}

// "fr", "fr-rCA": the only qualifiers apkgen understands.
struct Locale {
  std::string language, region;
};

Locale parse_qualifiers(const std::string& q) {
  Locale l;
  if (q.empty()) return l;
  auto parts = text::split(q, '-');
  auto alpha = [](const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'z'; });
  };
  if (parts.size() > 2 || parts[0].size() != 2 || !alpha(parts[0])) {
    throw std::runtime_error("qualificatif de ressource non pris en charge par apkgen : " + q);
  }
  l.language = parts[0];
  if (parts.size() == 2) {
    const std::string& r = parts[1];
    if (r.size() != 3 || r[0] != 'r' || !std::isupper(static_cast<unsigned char>(r[1])) ||
        !std::isupper(static_cast<unsigned char>(r[2]))) {
      throw std::runtime_error("région invalide (attendu rXX) : " + q);
    }
    l.region = r.substr(1);
  }
  return l;
}

void write_config(ByteWriter& w, const std::string& qualifiers) {
  Locale l = parse_qualifiers(qualifiers);
  size_t start = w.size();
  w.u32(kConfigSize);
  w.u32(0);  // mcc, mnc
  w.u8(l.language.empty() ? 0 : uint8_t(l.language[0]));
  w.u8(l.language.empty() ? 0 : uint8_t(l.language[1]));
  w.u8(l.region.empty() ? 0 : uint8_t(l.region[0]));
  w.u8(l.region.empty() ? 0 : uint8_t(l.region[1]));
  w.zeros(kConfigSize - (w.size() - start));
}

// aapt's string processing: whitespace runs collapse to one space and the
// ends are trimmed, except between double quotes (which are removed);
// backslash escapes \n \t \' \" \\ \@ \? \uXXXX.
std::string process_string(const std::string& raw, const std::string& where) {
  std::u16string in = text::utf8_to_utf16(raw), out;
  bool quoted = false, pending_space = false;
  for (size_t i = 0; i < in.size(); ++i) {
    char16_t c = in[i];
    if (c == u'"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r')) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(u' ');
      pending_space = false;
    }
    if (c != u'\\') {
      out.push_back(c);
      continue;
    }
    if (++i >= in.size()) throw std::runtime_error(where + " : barre oblique inverse finale");
    switch (in[i]) {
      case u'n': out.push_back(u'\n'); break;
      case u't': out.push_back(u'\t'); break;
      case u'u': {
        if (i + 4 >= in.size()) throw std::runtime_error(where + " : \\u incomplet");
        std::string hex = text::utf16_to_utf8(in.substr(i + 1, 4));
        if (hex.size() != 4 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
          throw std::runtime_error(where + " : \\u" + hex + " invalide");
        }
        out.push_back(char16_t(std::stoul(hex, nullptr, 16)));
        i += 4;
        break;
      }
      default: out.push_back(in[i]); break;  // \' \" \\ \@ \?
    }
  }
  return text::utf16_to_utf8(out);
}

bool is_integer(const std::string& v) {
  if (v.empty()) return false;
  size_t i = v[0] == '-' ? 1 : 0;
  return i < v.size() && std::all_of(v.begin() + long(i), v.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

void ResCompiler::define(const std::string& type, const std::string& name, const std::string& qualifiers, Value v) {
  if (finalized_) throw std::logic_error("ResCompiler : ajout après finalize()");
  if (!valid_name(name)) throw std::runtime_error(v.where + " : nom de ressource invalide « " + name + " »");
  parse_qualifiers(qualifiers);  // validates
  Configs& configs = entries_[type][name];
  if (configs.count(qualifiers) && type != "id") {
    throw std::runtime_error(v.where + " : ressource " + type + "/" + name + " définie deux fois");
  }
  configs[qualifiers] = std::move(v);
}

ResCompiler::Value ResCompiler::parse_value(const std::string& type, const XmlNode& n, const std::string& where) const {
  Value v;
  v.where = where;
  std::string t = text::trim(n.text);
  if (type == "id") {
    v.type = apk::kResIntBoolean;  // what aapt stores for ids
    return v;
  }
  if (!t.empty() && t[0] == '@') {
    v.type = apk::kResReference;
    v.reference = t;
    return v;
  }
  if (type == "string") {
    v.type = apk::kResString;
    v.string = process_string(n.text, where);
  } else if (type == "color") {
    auto c = parse_color(t);
    if (!c) throw std::runtime_error(where + " : couleur invalide « " + t + " »");
    v.type = c->first;
    v.data = c->second;
  } else if (type == "dimen") {
    auto d = parse_dimension(t);
    if (!d) throw std::runtime_error(where + " : dimension invalide « " + t + " »");
    v.type = d->first;
    v.data = d->second;
  } else if (type == "integer") {
    if (!is_integer(t)) throw std::runtime_error(where + " : entier invalide « " + t + " »");
    v.type = apk::kResIntDec;
    v.data = uint32_t(std::stol(t));
  } else if (type == "bool") {
    if (t != "true" && t != "false") throw std::runtime_error(where + " : booléen invalide « " + t + " »");
    v.type = apk::kResIntBoolean;
    v.data = t == "true" ? 0xFFFFFFFFu : 0;
  }
  return v;
}

void ResCompiler::add_values(const XmlNode& resources, const std::string& qualifiers, const std::string& where) {
  if (resources.name != "resources") throw std::runtime_error(where + " : élément racine <resources> attendu");
  parse_qualifiers(qualifiers);  // validates, even for an empty file
  for (const XmlNode& n : resources.children) {
    std::string type = n.name, name;
    for (const auto& [k, v] : n.attributes) {
      if (k == "name") name = v;
      else if (k == "type" && n.name == "item") type = v;
    }
    std::string at = where + ":" + std::to_string(n.line);
    if (!known_type(type) || type == "layout") {
      throw std::runtime_error(at + " : type de ressource non pris en charge par apkgen : <" + n.name + ">");
    }
    define(type, name, qualifiers, parse_value(type, n, at));
  }
}

void ResCompiler::collect_ids(const XmlNode& n) {
  for (const auto& [k, v] : n.attributes) {
    if (text::starts_with(v, "@+id/")) {
      Value id;
      id.type = apk::kResIntBoolean;
      id.where = "@+id";
      define("id", v.substr(5), "", id);
    }
  }
  for (const XmlNode& c : n.children) collect_ids(c);
}

void ResCompiler::add_layout(const std::string& name, const std::string& qualifiers, XmlNode root) {
  std::string dir = qualifiers.empty() ? "layout" : "layout-" + qualifiers;
  Layout l{"res/" + dir + "/" + name + ".xml", std::move(root)};
  Value v;
  v.type = apk::kResString;
  v.string = l.path;
  v.where = l.path;
  define("layout", name, qualifiers, v);
  collect_ids(l.root);
  layouts_.push_back(std::move(l));
}

void ResCompiler::add_directory(const std::string& dir) {
  fs::path root = path_from_utf8(dir);
  if (!fs::is_directory(root)) throw std::runtime_error("dossier de ressources introuvable : " + dir);
  std::vector<fs::path> files;
  for (const auto& sub : fs::directory_iterator(root)) {
    if (!sub.is_directory()) continue;
    for (const auto& f : fs::directory_iterator(sub.path())) {
      if (f.is_regular_file() && f.path().extension() == ".xml") files.push_back(f.path());
    }
  }
  std::sort(files.begin(), files.end());  // deterministic output
  for (const fs::path& f : files) {
    std::string folder = path_utf8(f.parent_path().filename());
    size_t dash = folder.find('-');
    std::string kind = folder.substr(0, dash);
    std::string qualifiers = dash == std::string::npos ? "" : folder.substr(dash + 1);
    std::string where = folder + "/" + path_utf8(f.filename());
    XmlNode node;
    try {
      node = parse_xml_text(read_text_file(f));
    } catch (const std::exception& e) {
      throw std::runtime_error(where + " : " + e.what());
    }
    if (kind == "values") {
      add_values(node, qualifiers, where);
    } else if (kind == "layout") {
      add_layout(path_utf8(f.stem()), qualifiers, std::move(node));
    } else {
      throw std::runtime_error("dossier de ressources non pris en charge par apkgen : " + folder);
    }
  }
}

void ResCompiler::finalize() {
  if (finalized_) return;
  finalized_ = true;
  for (const char* t : kTypeOrder) {
    auto it = entries_.find(t);
    if (it == entries_.end()) continue;
    type_order_.push_back(t);
    uint32_t type_id = uint32_t(type_order_.size());
    uint32_t index = 0;
    for (const auto& [name, configs] : it->second) {
      if (!configs.count("")) {
        throw std::runtime_error("ressource " + std::string(t) + "/" + name +
                                 " sans valeur par défaut (dossier sans qualificatif)");
      }
      ids_[t][name] = 0x7f000000u | (type_id << 16) | index++;
    }
  }
  // Every reference must now resolve.
  for (const auto& [type, names] : entries_) {
    for (const auto& [name, configs] : names) {
      for (const auto& [q, v] : configs) {
        if (!v.reference.empty() && !resolve(v.reference)) {
          throw std::runtime_error(v.where + " : ressource inconnue " + v.reference);
        }
      }
    }
  }
}

std::optional<uint32_t> ResCompiler::resolve(std::string_view ref) const {
  if (!finalized_ || ref.empty() || ref[0] != '@') return std::nullopt;
  ref.remove_prefix(1);
  if (!ref.empty() && ref[0] == '+') ref.remove_prefix(1);
  if (text::starts_with(ref, package_ + ":")) ref.remove_prefix(package_.size() + 1);
  size_t slash = ref.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  auto t = ids_.find(std::string(ref.substr(0, slash)));
  if (t == ids_.end()) return std::nullopt;
  auto e = t->second.find(std::string(ref.substr(slash + 1)));
  if (e == t->second.end()) return std::nullopt;
  return e->second;
}

std::vector<uint8_t> ResCompiler::write_arsc() const {
  if (!finalized_) throw std::logic_error("ResCompiler : write_arsc() avant finalize()");
  // Global value pool, key pool and type pool.
  std::vector<std::string> values, keys;
  std::map<std::string, uint32_t> value_index, key_index;
  auto intern = [](std::vector<std::string>& pool, std::map<std::string, uint32_t>& index, const std::string& s) {
    auto [it, inserted] = index.emplace(s, uint32_t(pool.size()));
    if (inserted) pool.push_back(s);
    return it->second;
  };
  for (const std::string& type : type_order_) {
    for (const auto& [name, configs] : entries_.at(type)) {
      intern(keys, key_index, name);
      for (const auto& [q, v] : configs) {
        if (v.type == apk::kResString && v.reference.empty()) intern(values, value_index, v.string);
      }
    }
  }

  ByteWriter pkg;
  pkg.u16(apk::kChunkTablePackage);
  pkg.u16(288);
  pkg.u32(0);  // size, patched
  pkg.u32(0x7f);
  std::u16string name16 = text::utf8_to_utf16(package_);
  if (name16.size() >= 128) throw std::runtime_error("nom de paquet trop long : " + package_);
  for (size_t i = 0; i < 128; ++i) pkg.u16(i < name16.size() ? name16[i] : 0);
  std::vector<uint8_t> type_pool = write_string_pool(type_order_, true);
  std::vector<uint8_t> key_pool = write_string_pool(keys, true);
  pkg.u32(288);
  pkg.u32(uint32_t(type_order_.size()));
  pkg.u32(288 + uint32_t(type_pool.size()));
  pkg.u32(uint32_t(keys.size()));
  pkg.u32(0);  // typeIdOffset
  pkg.bytes(type_pool);
  pkg.bytes(key_pool);

  for (size_t t = 0; t < type_order_.size(); ++t) {
    const auto& names = entries_.at(type_order_[t]);
    uint8_t type_id = uint8_t(t + 1);
    uint32_t count = uint32_t(names.size());
    std::set<std::string> qualifiers;
    for (const auto& [name, configs] : names) {
      for (const auto& [q, v] : configs) qualifiers.insert(q);
    }

    pkg.u16(apk::kChunkTableTypeSpec);
    pkg.u16(16);
    pkg.u32(16 + 4 * count);
    pkg.u8(type_id);
    pkg.u8(0);
    pkg.u16(uint16_t(qualifiers.size()));
    pkg.u32(count);
    for (const auto& [name, configs] : names) {
      bool localized = configs.size() > 1 || !configs.count("");
      pkg.u32(localized ? kConfigLocale : 0);
    }

    for (const std::string& q : qualifiers) {  // "" sorts first
      size_t start = pkg.size();
      uint32_t header = 20 + kConfigSize;
      pkg.u16(apk::kChunkTableType);
      pkg.u16(uint16_t(header));
      pkg.u32(0);  // size, patched
      pkg.u8(type_id);
      pkg.u8(0);  // flags: dense, 32-bit offsets
      pkg.u16(0);
      pkg.u32(count);
      pkg.u32(header + 4 * count);
      write_config(pkg, q);
      ByteWriter entries;
      for (const auto& [name, configs] : names) {
        auto it = configs.find(q);
        if (it == configs.end()) {
          pkg.u32(0xFFFFFFFFu);
          continue;
        }
        const Value& v = it->second;
        pkg.u32(uint32_t(entries.size()));
        entries.u16(8);
        entries.u16(0);
        entries.u32(key_index.at(name));
        entries.u16(8);
        entries.u8(0);
        if (!v.reference.empty()) {
          entries.u8(apk::kResReference);
          entries.u32(*resolve(v.reference));
        } else {
          entries.u8(v.type);
          entries.u32(v.type == apk::kResString ? value_index.at(v.string) : v.data);
        }
      }
      pkg.bytes(entries.buffer());
      pkg.patch_u32(start + 4, uint32_t(pkg.size() - start));
    }
  }
  pkg.patch_u32(4, uint32_t(pkg.size()));

  std::vector<uint8_t> value_pool = write_string_pool(values, true);
  ByteWriter table;
  table.u16(apk::kChunkTable);
  table.u16(12);
  table.u32(uint32_t(12 + value_pool.size() + pkg.size()));
  table.u32(1);
  table.bytes(value_pool);
  table.bytes(pkg.buffer());
  return table.take();
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> ResCompiler::compile_files() const {
  if (!finalized_) throw std::logic_error("ResCompiler : compile_files() avant finalize()");
  AxmlOptions options;
  options.resolve = [this](std::string_view ref) { return resolve(ref); };
  std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
  for (const Layout& l : layouts_) {
    try {
      out.emplace_back(l.path, compile_axml(l.root, options));
    } catch (const std::exception& e) {
      throw std::runtime_error(l.path + " : " + e.what());
    }
  }
  return out;
}

std::string ResCompiler::r_txt() const {
  std::string out;
  for (const std::string& type : type_order_) {
    for (const auto& [name, id] : ids_.at(type)) out += "int " + type + " " + name + " " + hex(id, 8) + "\n";
  }
  return out;
}

}  // namespace aa::tool
