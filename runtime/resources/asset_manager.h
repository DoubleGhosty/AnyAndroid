#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resources/res_table.h"
#include "resources/res_xml.h"

namespace aa::apk {
class ZipArchive;
}

// Resource resolution of libandroidfw's AssetManager2: an ordered set of
// ApkAssets (framework first, then the application), a device configuration,
// themes and the attribute resolution used by TypedArray.
namespace aa::res {

class ApkAssets {
 public:
  enum Flags : uint32_t { kSystem = 1, kDynamic = 2, kLoader = 4, kOverlay = 8 };

  // Throw FormatError on a malformed archive or table.
  static std::shared_ptr<ApkAssets> load_apk(std::string path, std::vector<uint8_t> bytes, uint32_t flags);
  static std::shared_ptr<ApkAssets> load_table(std::string path, std::vector<uint8_t> bytes, uint32_t flags);
  static std::shared_ptr<ApkAssets> load_empty(std::string path, uint32_t flags);
  ~ApkAssets();

  const std::string& path() const { return path_; }
  uint32_t flags() const { return flags_; }
  const LoadedArsc* table() const { return table_.get(); }
  // Archive entry ("res/layout/main.xml", "assets/data.txt"), nullopt if absent.
  std::optional<std::vector<uint8_t>> open(std::string_view entry) const;
  // Names directly under `dir` ("assets/fonts"), files and directories.
  std::vector<std::string> list(std::string_view dir) const;

 private:
  ApkAssets() = default;
  std::string path_;
  uint32_t flags_ = 0;
  std::unique_ptr<apk::ZipArchive> zip_;
  std::shared_ptr<LoadedArsc> table_;
};

// A value with where it came from (AssetManager2::SelectedValue).
struct SelectedValue {
  uint8_t type = kTypeNull;
  uint32_t data = 0;
  int32_t cookie = -1;  // index of the ApkAssets, -1 when none
  uint32_t flags = 0;   // typeSpec flags: configurations the value depends on
  uint32_t resid = 0;
  uint16_t density = 0;
};

struct BagEntry {
  uint32_t key = 0;
  Value value;
  int32_t cookie = -1;
  uint32_t style = 0;  // resource id of the style defining the entry
};

// A style, array or plural with its parents merged, sorted by key.
struct Bag {
  uint32_t type_spec_flags = 0;
  uint32_t parent = 0;
  std::vector<BagEntry> entries;
  const BagEntry* find(uint32_t key) const;
};

struct ResourceName {
  std::u16string package, type, entry;
};

class AssetManager {
 public:
  void set_apk_assets(std::vector<std::shared_ptr<ApkAssets>> apks);
  const std::vector<std::shared_ptr<ApkAssets>>& apk_assets() const { return apks_; }
  // The first configuration selects resources; the others are the user's
  // further locales (kept for getLocales).
  void set_configurations(std::vector<ResConfig> configs);
  const ResConfig& configuration() const { return config_; }

  std::optional<SelectedValue> get_resource(uint32_t resid, bool may_be_bag, uint16_t density = 0) const;
  // Follows references (up to 20). False when a resource is missing; the
  // value then holds the last one resolved.
  bool resolve_reference(SelectedValue& value) const;
  std::shared_ptr<const Bag> get_bag(uint32_t resid) const;

  std::optional<ResourceName> resource_name(uint32_t resid) const;
  // "package:type/entry" with defaults; 0 when unknown.
  uint32_t resource_id(std::string_view name, std::string_view def_type, std::string_view def_package) const;
  std::vector<std::string> locales(bool exclude_system) const;
  std::vector<std::pair<uint32_t, std::u16string>> packages() const;

  std::shared_ptr<StringPool> string_pool(int32_t cookie) const;
  uint8_t package_id_of_cookie(int32_t cookie) const;
  // A file of the APKs (cookie >= 0: that APK only; otherwise the last one
  // that has it). `out_cookie` receives the APK used.
  std::optional<std::vector<uint8_t>> open_non_asset(std::string_view path, int32_t cookie, int32_t* out_cookie) const;
  std::optional<std::vector<uint8_t>> open_asset(std::string_view name, int32_t* out_cookie) const;
  std::vector<std::string> list_assets(std::string_view dir) const;

 private:
  struct PackageRef {
    int32_t cookie = -1;
    const LoadedPackage* pkg = nullptr;
    uint8_t runtime_id = 0;
    std::array<uint8_t, 256> ids{};  // compile-time package id -> runtime id
  };
  struct Found {
    const PackageRef* ref = nullptr;
    const TypeVariant* variant = nullptr;
    Entry entry;
    uint32_t type_flags = 0;
  };

  std::optional<Found> find_entry(uint32_t resid, uint16_t density, bool stop_at_first, bool ignore_config) const;
  std::shared_ptr<const Bag> get_bag(uint32_t resid, std::vector<uint32_t>& chain) const;
  uint32_t fix_id(uint32_t id, const PackageRef& ref) const;
  Value fix_value(Value v, const PackageRef& ref) const;
  const std::vector<const TypeVariant*>& filtered(const PackageRef& ref, const TypeGroup& g) const;

  std::vector<std::shared_ptr<ApkAssets>> apks_;
  std::vector<PackageRef> refs_;
  std::array<std::vector<size_t>, 256> groups_;  // runtime package id -> refs_ indices
  ResConfig config_;
  std::vector<ResConfig> configs_;
  mutable std::mutex cache_lock_;
  mutable std::unordered_map<uint32_t, std::shared_ptr<const Bag>> bags_;
  mutable std::unordered_map<const TypeGroup*, std::vector<const TypeVariant*>> filtered_;
};

class Theme {
 public:
  explicit Theme(const AssetManager* am) : am_(am) {}

  const AssetManager* asset_manager() const { return am_; }
  bool apply_style(uint32_t resid, bool force);
  void clear();
  std::optional<SelectedValue> get_attribute(uint32_t resid) const;
  bool resolve_attribute_reference(SelectedValue& value) const;
  // Copies `other`, remapping cookies when it belongs to another manager.
  void set_to(const Theme& other);
  uint32_t changing_configurations() const { return type_spec_flags_; }

 private:
  struct ThemeEntry {
    int32_t cookie = -1;
    uint32_t type_spec_flags = 0;
    uint8_t type = kTypeNull;
    uint32_t data = 0;
  };
  const AssetManager* am_;
  std::map<uint32_t, ThemeEntry> entries_;
  uint32_t type_spec_flags_ = 0;
};

// AttributeResolution.cpp. Each attribute fills 7 values (type, data, cookie
// as seen by Java, resource id, changing configurations, density, source
// style); out_indices[0] receives the number of attributes found, followed by
// their positions. Callers size the outputs (attrs * 7, attrs + 1).
constexpr size_t kStyleNumEntries = 7;
void apply_style(const Theme& theme, const XmlParser* parser, uint32_t def_style_attr, uint32_t def_style_res,
                 std::span<const uint32_t> attrs, std::span<uint32_t> out_values, std::span<uint32_t> out_indices);
void resolve_attrs(const Theme& theme, uint32_t def_style_attr, uint32_t def_style_res,
                   std::span<const uint32_t> src_values, std::span<const uint32_t> attrs,
                   std::span<uint32_t> out_values, std::span<uint32_t> out_indices);
void retrieve_attributes(const AssetManager& am, const XmlParser* parser, std::span<const uint32_t> attrs,
                         std::span<uint32_t> out_values, std::span<uint32_t> out_indices);

inline int32_t java_cookie(int32_t cookie) { return cookie >= 0 ? cookie + 1 : -1; }

}  // namespace aa::res
