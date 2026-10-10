#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xml_text.h"

// Compiles an Android res/ directory into resources.arsc and binary XML files,
// like a minimal aapt2: values (string, color, integer, bool, dimen, item
// type="id"), layouts with @+id/ declarations, and language qualifiers
// (values-fr, values-fr-rCA). Resource ids are deterministic: types in a fixed
// order, entries sorted by name.
namespace aa::tool {

class ResCompiler {
 public:
  explicit ResCompiler(std::string package) : package_(std::move(package)) {}

  // Reads <dir>/values*/ *.xml and <dir>/layout*/ *.xml. Throws on errors.
  void add_directory(const std::string& dir);
  // Building blocks of add_directory (also used by tests).
  void add_values(const XmlNode& resources, const std::string& qualifiers, const std::string& where);
  void add_layout(const std::string& name, const std::string& qualifiers, XmlNode root);

  // Assigns resource ids; call once after the last add.
  void finalize();
  // "@string/app_name", "@+id/ok", "@layout/main" (package-local resources).
  std::optional<uint32_t> resolve(std::string_view ref) const;

  std::vector<uint8_t> write_arsc() const;
  // Compiled files to store in the APK: path ("res/layout/main.xml") -> bytes.
  std::vector<std::pair<std::string, std::vector<uint8_t>>> compile_files() const;
  // R.txt ("int string app_name 0x7f040000"), for writing smali by hand.
  std::string r_txt() const;

 private:
  struct Value {
    uint8_t type = 0;
    uint32_t data = 0;
    std::string string;     // kTypeString
    std::string reference;  // unresolved "@type/name" (kTypeReference)
    std::string where;
  };
  struct Layout {
    std::string path;  // "res/layout-fr/main.xml"
    XmlNode root;
  };
  using Configs = std::map<std::string, Value>;  // qualifiers -> value

  void define(const std::string& type, const std::string& name, const std::string& qualifiers, Value v);
  void collect_ids(const XmlNode& n);
  Value parse_value(const std::string& type, const XmlNode& n, const std::string& where) const;

  std::string package_;
  std::map<std::string, std::map<std::string, Configs>> entries_;  // type -> name -> configs
  std::vector<Layout> layouts_;
  std::map<std::string, std::map<std::string, uint32_t>> ids_;  // assigned by finalize()
  std::vector<std::string> type_order_;
  bool finalized_ = false;
};

}  // namespace aa::tool
