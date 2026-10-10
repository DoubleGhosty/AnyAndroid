#include "resources/asset_manager.h"

#include <algorithm>
#include <set>

#include "apk-inspector/zip_reader.h"
#include "common/bytes.h"
#include "common/text.h"

namespace aa::res {

namespace {

constexpr uint32_t kMaxReferences = 20;

bool valid_resid(uint32_t id) { return (id & 0x00ff0000u) != 0 && (id >> 24) != 0; }

void write_value(uint32_t* out, const SelectedValue& v, uint32_t source) {
  out[0] = v.type;
  out[1] = v.data;
  out[2] = uint32_t(java_cookie(v.cookie));
  out[3] = v.resid;
  out[4] = v.flags;
  out[5] = v.density;
  out[6] = source;
}

SelectedValue from_bag(const Bag& bag, const BagEntry& e) {
  SelectedValue v;
  v.type = e.value.type;
  v.data = e.value.data;
  v.cookie = e.cookie;
  v.flags = bag.type_spec_flags;
  return v;
}

// @null (reference 0) reads as an undefined value.
void normalize_null(SelectedValue& v) {
  if (v.type == kTypeReference && v.data == 0) {
    v.type = kTypeNull;
    v.data = kDataNullUndefined;
    v.cookie = -1;
  }
}

bool undefined(const SelectedValue& v) { return v.type == kTypeNull && v.data != kDataNullEmpty; }

}  // namespace

// --- ApkAssets --------------------------------------------------------------------------

ApkAssets::~ApkAssets() = default;

std::shared_ptr<ApkAssets> ApkAssets::load_apk(std::string path, std::vector<uint8_t> bytes, uint32_t flags) {
  std::shared_ptr<ApkAssets> a(new ApkAssets());
  a->path_ = std::move(path);
  a->flags_ = flags;
  a->zip_ = apk::ZipArchive::open(std::move(bytes));
  if (a->zip_->find("resources.arsc")) {
    a->table_ = LoadedArsc::load(std::make_shared<const std::vector<uint8_t>>(a->zip_->read("resources.arsc")));
  }
  return a;
}

std::shared_ptr<ApkAssets> ApkAssets::load_table(std::string path, std::vector<uint8_t> bytes, uint32_t flags) {
  std::shared_ptr<ApkAssets> a(new ApkAssets());
  a->path_ = std::move(path);
  a->flags_ = flags;
  a->table_ = LoadedArsc::load(std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
  return a;
}

std::shared_ptr<ApkAssets> ApkAssets::load_empty(std::string path, uint32_t flags) {
  std::shared_ptr<ApkAssets> a(new ApkAssets());
  a->path_ = std::move(path);
  a->flags_ = flags;
  return a;
}

std::optional<std::vector<uint8_t>> ApkAssets::open(std::string_view entry) const {
  if (!zip_) return std::nullopt;
  const apk::ZipEntry* e = zip_->find(entry);
  if (!e || e->unsafe_name) return std::nullopt;
  return zip_->read(*e);
}

std::vector<std::string> ApkAssets::list(std::string_view dir) const {
  std::set<std::string> names;
  if (!zip_) return {};
  std::string prefix(dir);
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  for (const apk::ZipEntry& e : zip_->entries()) {
    if (e.unsafe_name || e.name.size() <= prefix.size() || e.name.compare(0, prefix.size(), prefix) != 0) continue;
    std::string rest = e.name.substr(prefix.size());
    names.insert(rest.substr(0, rest.find('/')));
  }
  return {names.begin(), names.end()};
}

// --- Bag ---------------------------------------------------------------------------

const BagEntry* Bag::find(uint32_t key) const {
  auto it = std::lower_bound(entries.begin(), entries.end(), key,
                             [](const BagEntry& e, uint32_t k) { return e.key < k; });
  return it != entries.end() && it->key == key ? &*it : nullptr;
}

// --- AssetManager ----------------------------------------------------------------------

void AssetManager::set_apk_assets(std::vector<std::shared_ptr<ApkAssets>> apks) {
  apks_ = std::move(apks);
  refs_.clear();
  for (auto& g : groups_) g.clear();
  // Packages compiled as shared libraries (id 0) receive free ids from 0x02.
  std::set<uint8_t> used;
  for (const auto& apk : apks_) {
    if (!apk || !apk->table()) continue;
    for (const auto& p : apk->table()->packages()) {
      if (p->id != 0) used.insert(uint8_t(p->id));
    }
  }
  uint8_t next_free = 0x02;
  std::map<std::u16string, uint8_t> by_name;
  for (size_t cookie = 0; cookie < apks_.size(); ++cookie) {
    const auto& apk = apks_[cookie];
    if (!apk || !apk->table()) continue;
    for (const auto& p : apk->table()->packages()) {
      PackageRef ref;
      ref.cookie = int32_t(cookie);
      ref.pkg = p.get();
      if (p->id != 0) {
        ref.runtime_id = uint8_t(p->id);
      } else {
        auto known = by_name.find(p->name);
        if (known != by_name.end()) {
          ref.runtime_id = known->second;
        } else {
          while (used.count(next_free) && next_free < 0x7e) ++next_free;
          ref.runtime_id = next_free;
          used.insert(next_free);
        }
      }
      by_name.emplace(p->name, ref.runtime_id);
      refs_.push_back(ref);
    }
  }
  for (size_t i = 0; i < refs_.size(); ++i) {
    PackageRef& ref = refs_[i];
    for (size_t id = 0; id < ref.ids.size(); ++id) ref.ids[id] = uint8_t(id);
    ref.ids[0] = ref.runtime_id;
    for (const auto& [compile_id, name] : ref.pkg->libraries) {
      auto it = by_name.find(name);
      if (it != by_name.end() && compile_id < 256) ref.ids[compile_id] = it->second;
    }
    groups_[ref.runtime_id].push_back(i);
  }
  std::lock_guard<std::mutex> guard(cache_lock_);
  bags_.clear();
  filtered_.clear();
}

void AssetManager::set_configurations(std::vector<ResConfig> configs) {
  configs_ = std::move(configs);
  config_ = configs_.empty() ? ResConfig{} : configs_.front();
  std::lock_guard<std::mutex> guard(cache_lock_);
  bags_.clear();
  filtered_.clear();
}

uint32_t AssetManager::fix_id(uint32_t id, const PackageRef& ref) const {
  if (id == 0) return 0;
  uint8_t mapped = ref.ids[id >> 24];
  return (id & 0x00ffffffu) | (uint32_t(mapped) << 24);
}

Value AssetManager::fix_value(Value v, const PackageRef& ref) const {
  if (v.type == kTypeDynamicReference || v.type == kTypeDynamicAttribute) {
    v.type = v.type == kTypeDynamicReference ? kTypeReference : kTypeAttribute;
    v.data = fix_id(v.data, ref);
  } else if ((v.type == kTypeReference || v.type == kTypeAttribute) && v.data != 0 && (v.data >> 24) == 0) {
    v.data = fix_id(v.data, ref);  // a shared library naming its own resources
  }
  return v;
}

const std::vector<const TypeVariant*>& AssetManager::filtered(const PackageRef&, const TypeGroup& g) const {
  std::lock_guard<std::mutex> guard(cache_lock_);
  auto it = filtered_.find(&g);
  if (it != filtered_.end()) return it->second;
  std::vector<const TypeVariant*> out;
  for (const TypeVariant& v : g.variants) {
    if (v.config.match(config_)) out.push_back(&v);
  }
  return filtered_.emplace(&g, std::move(out)).first->second;
}

std::optional<AssetManager::Found> AssetManager::find_entry(uint32_t resid, uint16_t density, bool stop_at_first,
                                                            bool ignore_config) const {
  if (!valid_resid(resid)) return std::nullopt;
  uint8_t pkg_id = uint8_t(resid >> 24);
  uint8_t type_id = uint8_t(resid >> 16);
  uint16_t entry_index = uint16_t(resid);
  ResConfig desired = config_;
  if (density) desired.density = density;
  bool use_filtered = !ignore_config && density == 0;

  std::optional<Found> best;
  for (size_t index : groups_[pkg_id]) {
    const PackageRef& ref = refs_[index];
    const TypeGroup* g = ref.pkg->type(type_id);
    if (!g || entry_index >= g->spec_flags.size()) continue;
    auto consider = [&](const TypeVariant& v) {
      if (!ignore_config && !use_filtered && !v.config.match(desired)) return false;
      auto offset = v.entry_offset(entry_index);
      if (!offset) return false;
      // Later packages of a group (overlays, loaders) win on equal configurations.
      if (best && !v.config.is_better_than(best->variant->config, &desired) &&
          !(best->ref != &ref && v.config == best->variant->config)) {
        return false;
      }
      auto entry = ref.pkg->read_entry(v, *offset);
      if (!entry) return false;
      best = Found{&ref, &v, std::move(*entry), g->spec_flags[entry_index]};
      return true;
    };
    if (use_filtered) {
      for (const TypeVariant* v : filtered(ref, *g)) {
        if (consider(*v) && stop_at_first) return best;
      }
    } else {
      for (const TypeVariant& v : g->variants) {
        if (consider(v) && stop_at_first) return best;
      }
    }
  }
  return best;
}

std::optional<SelectedValue> AssetManager::get_resource(uint32_t resid, bool may_be_bag, uint16_t density) const {
  auto f = find_entry(resid, density, false, false);
  if (!f) return std::nullopt;
  SelectedValue v;
  v.cookie = f->ref->cookie;
  v.flags = f->type_flags;
  v.resid = resid;
  v.density = f->variant->config.density;
  if (f->entry.complex) {
    if (!may_be_bag) return std::nullopt;
    // A bag is represented by a reference to itself.
    v.type = kTypeReference;
    v.data = resid;
    return v;
  }
  Value value = fix_value(f->entry.value, *f->ref);
  v.type = value.type;
  v.data = value.data;
  return v;
}

bool AssetManager::resolve_reference(SelectedValue& value) const {
  if (value.type != kTypeReference || value.data == 0) return true;
  const uint32_t original_flags = value.flags;
  uint32_t combined = 0;
  uint32_t resid = value.data;
  for (uint32_t i = 0;; ++i) {
    auto r = get_resource(resid, true);
    if (!r) {
      value.resid = resid;
      return false;
    }
    value = *r;
    value.flags |= combined;
    if (r->type != kTypeReference || r->data == kDataNullUndefined || r->data == resid || i == kMaxReferences) {
      value.flags |= original_flags;
      return true;
    }
    combined = r->flags;
    resid = r->data;
  }
}

std::shared_ptr<const Bag> AssetManager::get_bag(uint32_t resid) const {
  std::vector<uint32_t> chain;
  return get_bag(resid, chain);
}

std::shared_ptr<const Bag> AssetManager::get_bag(uint32_t resid, std::vector<uint32_t>& chain) const {
  {
    std::lock_guard<std::mutex> guard(cache_lock_);
    auto it = bags_.find(resid);
    if (it != bags_.end()) return it->second;
  }
  auto f = find_entry(resid, 0, false, false);
  if (!f || !f->entry.complex) return nullptr;
  auto bag = std::make_shared<Bag>();
  bag->type_spec_flags = f->type_flags;
  bag->parent = fix_id(f->entry.parent, *f->ref);
  std::vector<BagEntry> own;
  own.reserve(f->entry.map.size());
  for (const auto& [key, value] : f->entry.map) {
    own.push_back(BagEntry{fix_id(key, *f->ref), fix_value(value, *f->ref), f->ref->cookie, resid});
  }
  std::stable_sort(own.begin(), own.end(), [](const BagEntry& a, const BagEntry& b) { return a.key < b.key; });

  std::shared_ptr<const Bag> parent;
  if (bag->parent != 0 && bag->parent != resid &&
      std::find(chain.begin(), chain.end(), bag->parent) == chain.end()) {
    chain.push_back(resid);
    parent = get_bag(bag->parent, chain);
    chain.pop_back();
  }
  if (!parent) {
    bag->entries = std::move(own);
  } else {
    // Merge: the child's entries override its parent's.
    bag->type_spec_flags |= parent->type_spec_flags;
    bag->entries.reserve(own.size() + parent->entries.size());
    auto c = own.begin();
    auto p = parent->entries.begin();
    while (c != own.end() || p != parent->entries.end()) {
      if (p == parent->entries.end() || (c != own.end() && c->key < p->key)) {
        bag->entries.push_back(*c++);
      } else if (c == own.end() || p->key < c->key) {
        bag->entries.push_back(*p++);
      } else {
        bag->entries.push_back(*c++);
        ++p;
      }
    }
  }
  std::lock_guard<std::mutex> guard(cache_lock_);
  bags_[resid] = bag;
  return bag;
}

std::optional<ResourceName> AssetManager::resource_name(uint32_t resid) const {
  auto f = find_entry(resid, 0, true, true);
  if (!f) return std::nullopt;
  const LoadedPackage& pkg = *f->ref->pkg;
  ResourceName name;
  name.package = pkg.name;
  if (auto t = pkg.type_name(uint8_t(resid >> 16))) name.type = *t;
  if (pkg.key_strings) {
    if (auto k = pkg.key_strings->string16(f->entry.key)) name.entry = *k;
  }
  return name;
}

uint32_t AssetManager::resource_id(std::string_view name, std::string_view def_type,
                                   std::string_view def_package) const {
  // [@][package:][type/]entry
  std::string_view s = name;
  if (!s.empty() && s[0] == '@') s.remove_prefix(1);
  std::string_view package, type;
  size_t colon = s.find(':');
  size_t slash = s.find('/');
  if (colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash)) {
    package = s.substr(0, colon);
    if (package.empty()) return 0;
    s.remove_prefix(colon + 1);
    slash = s.find('/');
  }
  if (slash != std::string_view::npos) {
    type = s.substr(0, slash);
    if (type.empty()) return 0;
    s.remove_prefix(slash + 1);
  }
  if (package.empty()) package = def_package;
  if (type.empty()) type = def_type;
  if (s.empty() || type.empty()) return 0;
  std::u16string package16 = text::utf8_to_utf16(package);
  std::u16string type16 = text::utf8_to_utf16(type);
  std::u16string entry16 = text::utf8_to_utf16(s);
  for (const PackageRef& ref : refs_) {
    if (ref.pkg->name != package16) continue;
    auto id = ref.pkg->find_entry_by_name(type16, entry16);
    if (!id && type16 == u"attr") id = ref.pkg->find_entry_by_name(u"^attr-private", entry16);
    if (id) return (*id & 0x00ffffffu) | (uint32_t(ref.runtime_id) << 24);
  }
  return 0;
}

std::vector<std::string> AssetManager::locales(bool exclude_system) const {
  std::set<std::string> out;
  for (const PackageRef& ref : refs_) {
    if (exclude_system && (apks_[size_t(ref.cookie)]->flags() & ApkAssets::kSystem)) continue;
    for (const TypeGroup& g : ref.pkg->types) {
      for (const TypeVariant& v : g.variants) {
        if (v.config.locale_key() != 0) {
          std::string tag = v.config.bcp47_locale();
          if (!tag.empty()) out.insert(tag);
        }
      }
    }
  }
  return {out.begin(), out.end()};
}

std::vector<std::pair<uint32_t, std::u16string>> AssetManager::packages() const {
  std::vector<std::pair<uint32_t, std::u16string>> out;
  for (const PackageRef& ref : refs_) out.emplace_back(ref.runtime_id, ref.pkg->name);
  return out;
}

std::shared_ptr<StringPool> AssetManager::string_pool(int32_t cookie) const {
  if (cookie < 0 || size_t(cookie) >= apks_.size() || !apks_[size_t(cookie)]->table()) return nullptr;
  return apks_[size_t(cookie)]->table()->values();
}

uint8_t AssetManager::package_id_of_cookie(int32_t cookie) const {
  for (const PackageRef& ref : refs_) {
    if (ref.cookie == cookie) return ref.runtime_id;
  }
  return 0x7f;
}

std::optional<std::vector<uint8_t>> AssetManager::open_non_asset(std::string_view path, int32_t cookie,
                                                                 int32_t* out_cookie) const {
  if (cookie >= 0) {
    if (size_t(cookie) >= apks_.size()) return std::nullopt;
    if (out_cookie) *out_cookie = cookie;
    return apks_[size_t(cookie)]->open(path);
  }
  for (size_t i = apks_.size(); i-- > 0;) {
    if (auto data = apks_[i]->open(path)) {
      if (out_cookie) *out_cookie = int32_t(i);
      return data;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> AssetManager::open_asset(std::string_view name, int32_t* out_cookie) const {
  return open_non_asset("assets/" + std::string(name), -1, out_cookie);
}

std::vector<std::string> AssetManager::list_assets(std::string_view dir) const {
  std::set<std::string> out;
  for (const auto& apk : apks_) {
    for (auto& n : apk->list("assets/" + std::string(dir))) out.insert(std::move(n));
  }
  return {out.begin(), out.end()};
}

// --- Theme -----------------------------------------------------------------------------

bool Theme::apply_style(uint32_t resid, bool force) {
  auto bag = am_->get_bag(resid);
  if (!bag) return false;
  type_spec_flags_ |= bag->type_spec_flags;
  for (const BagEntry& e : bag->entries) {
    if (!valid_resid(e.key)) return false;
    // @empty is a value; a null value only clears with `force`.
    bool is_undefined = e.value.type == kTypeNull && e.value.data != kDataNullEmpty;
    if (!force && is_undefined) continue;
    auto it = entries_.find(e.key);
    if (it != entries_.end()) {
      if (is_undefined) {
        entries_.erase(it);
      } else if (force) {
        it->second = ThemeEntry{e.cookie, bag->type_spec_flags, e.value.type, e.value.data};
      }
    } else {
      entries_.emplace(e.key, ThemeEntry{e.cookie, bag->type_spec_flags, e.value.type, e.value.data});
    }
  }
  return true;
}

void Theme::clear() {
  entries_.clear();
  type_spec_flags_ = 0;
}

std::optional<SelectedValue> Theme::get_attribute(uint32_t resid) const {
  uint32_t flags = 0;
  for (uint32_t i = 0; i <= kMaxReferences; ++i) {
    auto it = entries_.find(resid);
    if (it == entries_.end()) return std::nullopt;
    flags |= it->second.type_spec_flags;
    if (it->second.type == kTypeAttribute) {
      resid = it->second.data;
      continue;
    }
    SelectedValue v;
    v.type = it->second.type;
    v.data = it->second.data;
    v.cookie = it->second.cookie;
    v.flags = flags;
    return v;
  }
  return std::nullopt;
}

bool Theme::resolve_attribute_reference(SelectedValue& value) const {
  if (value.type != kTypeAttribute) return am_->resolve_reference(value);
  auto result = get_attribute(value.data);
  if (!result) return false;
  bool ok = am_->resolve_reference(*result);
  if (ok) {
    result->flags |= value.flags;
    value = *result;
  }
  return ok;
}

void Theme::set_to(const Theme& other) {
  if (&other == this) return;
  type_spec_flags_ = other.type_spec_flags_;
  if (other.am_ == am_) {
    entries_ = other.entries_;
    return;
  }
  // Another manager: keep the attributes whose APK is also loaded here.
  entries_.clear();
  const auto& src = other.am_->apk_assets();
  const auto& dst = am_->apk_assets();
  auto map_cookie = [&](int32_t cookie) -> std::optional<int32_t> {
    if (cookie < 0) return cookie;
    if (size_t(cookie) >= src.size()) return std::nullopt;
    for (size_t i = 0; i < dst.size(); ++i) {
      if (dst[i] == src[size_t(cookie)] || dst[i]->path() == src[size_t(cookie)]->path()) return int32_t(i);
    }
    return std::nullopt;
  };
  for (const auto& [key, e] : other.entries_) {
    auto cookie = map_cookie(e.cookie);
    if (!cookie) continue;
    ThemeEntry copy = e;
    copy.cookie = *cookie;
    entries_.emplace(key, copy);
  }
}

// --- Attribute resolution -------------------------------------------------------------------

namespace {

std::shared_ptr<const Bag> style_bag(const Theme& theme, uint32_t def_style_attr, uint32_t def_style_res,
                                     uint32_t* flags) {
  if (def_style_attr != 0) {
    if (auto v = theme.get_attribute(def_style_attr)) {
      *flags |= v->flags;
      if (v->type == kTypeReference) def_style_res = v->data;
    }
  }
  return def_style_res != 0 ? theme.asset_manager()->get_bag(def_style_res) : nullptr;
}

std::shared_ptr<const Bag> xml_style_bag(const Theme& theme, const XmlParser* parser, uint32_t* flags) {
  if (!parser) return nullptr;
  int32_t idx = parser->index_of_style();
  if (idx < 0) return nullptr;
  Value v = parser->attribute_value(idx);
  if (v.type == kTypeAttribute) {
    if (auto r = theme.get_attribute(v.data)) {
      v.type = r->type;
      v.data = r->data;
      *flags |= r->flags;
    }
  }
  if (v.type != kTypeReference) return nullptr;
  return theme.asset_manager()->get_bag(v.data);
}

}  // namespace

void apply_style(const Theme& theme, const XmlParser* parser, uint32_t def_style_attr, uint32_t def_style_res,
                 std::span<const uint32_t> attrs, std::span<uint32_t> out_values, std::span<uint32_t> out_indices) {
  uint32_t def_flags = 0, xml_flags = 0;
  auto def_bag = style_bag(theme, def_style_attr, def_style_res, &def_flags);
  auto xml_bag = xml_style_bag(theme, parser, &xml_flags);
  uint32_t found = 0;
  for (size_t i = 0; i < attrs.size(); ++i) {
    const uint32_t ident = attrs[i];
    SelectedValue value;
    uint32_t source = 0;
    // XML attributes first, then the XML style, the default style, the theme.
    int32_t xml_index = parser ? parser->index_of_resid(ident) : -1;
    if (xml_index >= 0) {
      Value v = parser->attribute_value(xml_index);
      value.type = v.type;
      value.data = v.data;
      source = parser->source_resid();
    }
    if (undefined(value) && xml_bag) {
      if (const BagEntry* e = xml_bag->find(ident)) {
        value = from_bag(*xml_bag, *e);
        value.flags |= xml_flags;
        source = e->style;
      }
    }
    if (undefined(value) && def_bag) {
      if (const BagEntry* e = def_bag->find(ident)) {
        value = from_bag(*def_bag, *e);
        value.flags |= def_flags;
        source = e->style;
      }
    }
    if (value.type != kTypeNull) {
      theme.resolve_attribute_reference(value);
    } else if (value.data != kDataNullEmpty) {
      if (auto attr = theme.get_attribute(ident)) {
        value = *attr;
        theme.asset_manager()->resolve_reference(value);
      }
    }
    normalize_null(value);
    write_value(out_values.data() + i * kStyleNumEntries, value, source);
    if (value.type != kTypeNull || value.data == kDataNullEmpty) out_indices[++found] = uint32_t(i);
  }
  out_indices[0] = found;
}

void resolve_attrs(const Theme& theme, uint32_t def_style_attr, uint32_t def_style_res,
                   std::span<const uint32_t> src_values, std::span<const uint32_t> attrs,
                   std::span<uint32_t> out_values, std::span<uint32_t> out_indices) {
  uint32_t def_flags = 0;
  auto def_bag = style_bag(theme, def_style_attr, def_style_res, &def_flags);
  uint32_t found = 0;
  for (size_t i = 0; i < attrs.size(); ++i) {
    const uint32_t ident = attrs[i];
    SelectedValue value;
    if (i < src_values.size() && src_values[i] != 0) {
      value.type = kTypeAttribute;
      value.data = src_values[i];
    } else if (def_bag) {
      if (const BagEntry* e = def_bag->find(ident)) {
        value = from_bag(*def_bag, *e);
        value.flags |= def_flags;
      }
    }
    if (value.type != kTypeNull) {
      theme.resolve_attribute_reference(value);
    } else if (value.data != kDataNullEmpty) {
      if (auto attr = theme.get_attribute(ident)) {
        value = *attr;
        theme.asset_manager()->resolve_reference(value);
      }
    }
    normalize_null(value);
    write_value(out_values.data() + i * kStyleNumEntries, value, 0);
    if (value.type != kTypeNull || value.data == kDataNullEmpty) {
      ++found;
      if (!out_indices.empty()) out_indices[found] = uint32_t(i);
    }
  }
  if (!out_indices.empty()) out_indices[0] = found;
}

void retrieve_attributes(const AssetManager& am, const XmlParser* parser, std::span<const uint32_t> attrs,
                         std::span<uint32_t> out_values, std::span<uint32_t> out_indices) {
  uint32_t found = 0;
  for (size_t i = 0; i < attrs.size(); ++i) {
    SelectedValue value;
    int32_t xml_index = parser ? parser->index_of_resid(attrs[i]) : -1;
    if (xml_index >= 0) {
      Value v = parser->attribute_value(xml_index);
      value.type = v.type;
      value.data = v.data;
    }
    if (value.type != kTypeNull) am.resolve_reference(value);
    normalize_null(value);
    write_value(out_values.data() + i * kStyleNumEntries, value, 0);
    if (value.type != kTypeNull || value.data == kDataNullEmpty) {
      ++found;
      if (!out_indices.empty()) out_indices[found] = uint32_t(i);
    }
  }
  if (!out_indices.empty()) out_indices[0] = found;
}

}  // namespace aa::res
