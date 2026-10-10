#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "resources/asset_manager.h"
#include "resources/res_config.h"
#include "resources/res_table.h"
#include "resources/res_xml.h"
#include "test.h"

using namespace aa::res;
using namespace aa;

namespace {

// Res_value types the runtime has no constant for.
constexpr uint8_t kIntDec = 0x10, kIntColorArgb8 = 0x1c;
// TYPE_DIMENSION data: COMPLEX_UNIT_DIP, radix 23p0.
constexpr uint32_t dip(uint32_t v) { return (v << 8) | 1; }
// ResTable_type flags.
constexpr uint8_t kDense = 0x00, kSparse = 0x01, kOffset16 = 0x02;
constexpr uint32_t kNoIndex = 0xffffffffu;  // ResStringPool_ref without a string

struct ChunkWriter {
  std::vector<uint8_t> data;

  void u8(uint8_t v) { data.push_back(v); }
  void u16(uint16_t v) {
    data.push_back(v & 0xff);
    data.push_back(v >> 8);
  }
  void u32(uint32_t v) {
    data.push_back(v & 0xff);
    data.push_back((v >> 8) & 0xff);
    data.push_back((v >> 16) & 0xff);
    data.push_back(v >> 24);
  }
  void append(const std::vector<uint8_t>& v) { data.insert(data.end(), v.begin(), v.end()); }
  void pad() {
    while (data.size() % 4 != 0) u8(0);
  }

  void patch_size(size_t offset) {
    uint32_t sz = data.size() - offset;
    data[offset + 4] = sz & 0xff;
    data[offset + 5] = (sz >> 8) & 0xff;
    data[offset + 6] = (sz >> 16) & 0xff;
    data[offset + 7] = (sz >> 24) & 0xff;
  }
};

// Independent of aa::text, so that the pools test the runtime's decoding
// against a second implementation.
std::string to_utf8(std::u16string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
      c = 0x10000 + ((c - 0xd800) << 10) + (uint32_t(s[i + 1]) - 0xdc00);
      ++i;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xc0 | (c >> 6));
      out += char(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += char(0xe0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3f));
      out += char(0x80 | (c & 0x3f));
    } else {
      out += char(0xf0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3f));
      out += char(0x80 | ((c >> 6) & 0x3f));
      out += char(0x80 | (c & 0x3f));
    }
  }
  return out;
}

// ResStringPool chunk. UTF-8 strings carry two lengths (UTF-16 units, then
// bytes), each on one byte, or two above 0x7f; UTF-16 strings carry their
// length in units on one u16, or two above 0x7fff. `styles[i]` lists the
// spans of string i as (name, first char, last char) triples.
std::vector<uint8_t> encode_string_pool(const std::vector<std::u16string>& strings, bool utf8,
                                        const std::vector<std::vector<uint32_t>>& styles = {}) {
  ChunkWriter str_data;
  std::vector<uint32_t> str_offsets;
  auto length8 = [&](size_t v) {
    if (v > 0x7f) str_data.u8(uint8_t(0x80 | (v >> 8)));
    str_data.u8(uint8_t(v));
  };
  for (const auto& s : strings) {
    str_offsets.push_back(uint32_t(str_data.data.size()));
    if (utf8) {
      std::string bytes = to_utf8(s);
      length8(s.size());
      length8(bytes.size());
      for (char c : bytes) str_data.u8(uint8_t(c));
      str_data.u8(0);
    } else {
      if (s.size() > 0x7fff) str_data.u16(uint16_t(0x8000 | (s.size() >> 16)));
      str_data.u16(uint16_t(s.size()));
      for (char16_t c : s) str_data.u16(c);
      str_data.u16(0);
    }
  }
  str_data.pad();

  ChunkWriter style_data;  // ResStringPool_span arrays, each ended by END (0xffffffff)
  std::vector<uint32_t> style_offsets;
  for (const auto& spans : styles) {
    style_offsets.push_back(uint32_t(style_data.data.size()));
    for (uint32_t v : spans) style_data.u32(v);
    style_data.u32(0xffffffffu);
  }
  if (!styles.empty()) {
    style_data.u32(0xffffffffu);  // aapt ends the style area with two more END markers
    style_data.u32(0xffffffffu);
  }

  ChunkWriter w;
  w.u16(0x0001);  // kChunkStringPool
  w.u16(28);      // header size
  w.u32(0);       // size, patched below
  w.u32(strings.size());
  w.u32(styles.size());
  w.u32(utf8 ? 0x100 : 0);  // flags
  const uint32_t strings_start = 28 + 4 * (strings.size() + styles.size());
  w.u32(strings_start);
  w.u32(styles.empty() ? 0 : strings_start + str_data.data.size());  // styles_start
  for (uint32_t o : str_offsets) w.u32(o);
  for (uint32_t o : style_offsets) w.u32(o);
  w.append(str_data.data);
  w.append(style_data.data);
  w.patch_size(0);
  return w.data;
}

// ASCII strings.
std::vector<uint8_t> make_string_pool(const std::vector<std::string>& strings, bool utf8 = true) {
  std::vector<std::u16string> wide;
  for (const auto& s : strings) wide.emplace_back(s.begin(), s.end());
  return encode_string_pool(wide, utf8);
}

// ResTable_config, 64 bytes as in current AOSP; offsets as in ResourceTypes.h.
std::vector<uint8_t> config_bytes(const ResConfig& c) {
  ChunkWriter w;
  w.u32(64);
  w.u16(c.mcc);
  w.u16(c.mnc);
  for (char ch : c.language) w.u8(uint8_t(ch));
  for (char ch : c.country) w.u8(uint8_t(ch));
  w.u8(c.orientation);
  w.u8(c.touchscreen);
  w.u16(c.density);
  w.u8(c.keyboard);
  w.u8(c.navigation);
  w.u8(c.input_flags);
  w.u8(c.grammatical_inflection);
  w.u16(c.screen_width);
  w.u16(c.screen_height);
  w.u16(c.sdk_version);
  w.u16(c.minor_version);
  w.u8(c.screen_layout);
  w.u8(c.ui_mode);
  w.u16(c.smallest_screen_width_dp);
  w.u16(c.screen_width_dp);
  w.u16(c.screen_height_dp);
  for (char ch : c.locale_script) w.u8(uint8_t(ch));
  for (char ch : c.locale_variant) w.u8(uint8_t(ch));
  w.u8(c.screen_layout2);
  w.u8(c.color_mode);
  w.u16(0);  // screenConfigPad2
  w.u8(c.locale_script_was_computed ? 1 : 0);
  for (char ch : c.locale_numbering_system) w.u8(uint8_t(ch));
  while (w.data.size() < 64) w.u8(0);
  return w.data;
}

// Locale qualifiers as aapt2 compiles "values-fr-rCA": language and region,
// no script.
ResConfig locale_qualifier(std::string_view language, std::string_view region = "") {
  ResConfig c{};
  if (language.size() == 2) {
    c.language[0] = language[0];
    c.language[1] = language[1];
  }
  if (region.size() == 2) {
    c.country[0] = region[0];
    c.country[1] = region[1];
  }
  return c;
}

// UI_MODE_TYPE_NORMAL with UI_MODE_NIGHT_NO / UI_MODE_NIGHT_YES.
constexpr uint8_t kUiDay = 0x11, kUiNight = 0x21;

// A device configuration as the framework sets it (script computed).
ResConfig device(std::string_view locale, uint16_t density, uint16_t sdk, uint8_t ui_mode = kUiDay) {
  ResConfig c{};
  c.set_bcp47_locale(locale);
  c.density = density;
  c.sdk_version = sdk;
  c.ui_mode = ui_mode;
  return c;
}

// --- ResTable_entry encodings ---

std::vector<uint8_t> simple_entry(uint32_t key, uint8_t type, uint32_t value) {
  ChunkWriter w;
  w.u16(8);  // entry size
  w.u16(0);  // flags
  w.u32(key);
  w.u16(8);  // Res_value size
  w.u8(0);
  w.u8(type);
  w.u32(value);
  return w.data;
}

// FLAG_COMPACT: 16-bit key, the value type in the high byte of the flags.
std::vector<uint8_t> compact_entry(uint16_t key, uint8_t type, uint32_t value) {
  ChunkWriter w;
  w.u16(key);
  w.u16(uint16_t(0x0008 | (type << 8)));
  w.u32(value);
  return w.data;
}

struct MapEntry {
  uint32_t key;
  uint8_t type;
  uint32_t value;
};

std::vector<uint8_t> bag_entry(uint32_t key, uint32_t parent, const std::vector<MapEntry>& map) {
  ChunkWriter w;
  w.u16(16);      // ResTable_map_entry size
  w.u16(0x0001);  // kEntryComplex
  w.u32(key);
  w.u32(parent);
  w.u32(map.size());
  for (const MapEntry& m : map) {
    w.u32(m.key);
    w.u16(8);
    w.u8(0);
    w.u8(m.type);
    w.u32(m.value);
  }
  return w.data;
}

// --- Tables ---

// One ResTable_type chunk: entries by increasing index.
struct Variant {
  ResConfig config;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> entries;
  uint8_t flags = kDense;
};

struct TypeDef {
  uint8_t id;
  std::vector<uint32_t> spec_flags;  // one per entry
  std::vector<Variant> variants;
};

std::vector<uint8_t> make_type_spec(uint8_t id, const std::vector<uint32_t>& flags) {
  ChunkWriter w;
  w.u16(0x0202);  // kChunkTypeSpec
  w.u16(16);
  w.u32(0);
  w.u8(id);
  w.u8(0);
  w.u16(0);
  w.u32(flags.size());
  for (uint32_t f : flags) w.u32(f);
  w.patch_size(0);
  return w.data;
}

std::vector<uint8_t> make_type(uint8_t id, uint32_t entry_count, const Variant& v) {
  std::vector<uint8_t> entries;
  std::vector<std::pair<uint16_t, uint32_t>> placed;  // (index, offset in entries)
  for (const auto& [index, bytes] : v.entries) {
    placed.emplace_back(index, uint32_t(entries.size()));
    entries.insert(entries.end(), bytes.begin(), bytes.end());
  }
  ChunkWriter offsets;
  if (v.flags & kSparse) {
    // ResTable_sparseTypeEntry: (index, offset / 4).
    for (const auto& [index, offset] : placed) {
      offsets.u16(index);
      offsets.u16(uint16_t(offset / 4));
    }
  } else {
    for (uint32_t i = 0; i < entry_count; ++i) {
      auto it = std::find_if(placed.begin(), placed.end(), [&](const auto& p) { return uint32_t(p.first) == i; });
      if (v.flags & kOffset16) {
        offsets.u16(it == placed.end() ? 0xffff : uint16_t(it->second / 4));
      } else {
        offsets.u32(it == placed.end() ? 0xffffffffu : it->second);
      }
    }
  }
  offsets.pad();

  ChunkWriter w;
  w.u16(0x0201);  // kChunkType
  w.u16(84);      // header: 20 + ResTable_config (64)
  w.u32(0);       // size, patched below
  w.u8(id);
  w.u8(v.flags);
  w.u16(0);
  w.u32((v.flags & kSparse) ? placed.size() : entry_count);
  w.u32(84 + offsets.data.size());  // entriesStart
  w.append(config_bytes(v.config));
  w.append(offsets.data);
  w.append(entries);
  w.patch_size(0);
  return w.data;
}

std::vector<uint8_t> make_package(uint32_t id, std::u16string_view name, const std::vector<std::string>& types,
                                  const std::vector<std::string>& keys, const std::vector<TypeDef>& defs) {
  ChunkWriter w;
  w.u16(0x0200);  // kChunkPackage
  w.u16(288);
  w.u32(0);
  w.u32(id);
  for (size_t i = 0; i < 128; ++i) w.u16(i < name.size() ? name[i] : 0);
  auto type_strings = make_string_pool(types);
  auto key_strings = make_string_pool(keys);
  w.u32(288);  // typeStrings
  w.u32(types.size());
  w.u32(288 + type_strings.size());  // keyStrings
  w.u32(keys.size());
  w.u32(0);  // typeIdOffset
  w.append(type_strings);
  w.append(key_strings);
  for (const TypeDef& def : defs) {
    w.append(make_type_spec(def.id, def.spec_flags));
    for (const Variant& v : def.variants) w.append(make_type(def.id, def.spec_flags.size(), v));
  }
  w.patch_size(0);
  return w.data;
}

std::vector<uint8_t> make_table(const std::vector<uint8_t>& values, const std::vector<std::vector<uint8_t>>& packages) {
  ChunkWriter w;
  w.u16(0x0002);  // kChunkTable
  w.u16(12);
  w.u32(0);
  w.u32(packages.size());
  w.append(values);
  for (const auto& p : packages) w.append(p);
  w.patch_size(0);
  return w.data;
}

std::shared_ptr<ApkAssets> load(const char* path, std::vector<uint8_t> bytes, uint32_t flags = 0) {
  auto apk = ApkAssets::load_table(path, std::move(bytes), flags);
  CHECK(apk != nullptr);
  CHECK(apk->table() != nullptr);
  return apk;
}

std::vector<uint8_t> make_arsc() {
  ChunkWriter w;
  w.u16(0x0002);  // kChunkTable
  w.u16(12);
  w.u32(0);       // size placeholder
  w.u32(1);       // package count

  auto global_pool = make_string_pool({"Valeur String 1", "Valeur String 2"});
  w.append(global_pool);

  size_t pkg_start = w.data.size();
  w.u16(0x0200);  // kChunkPackage
  w.u16(288);
  w.u32(0);       // size placeholder
  w.u32(0x7f);    // id

  // name (256 bytes)
  std::u16string name = u"com.test";
  for (size_t i = 0; i < 128; ++i) w.u16(i < name.size() ? name[i] : 0);

  auto type_strings = make_string_pool({"string", "color", "style"});
  auto key_strings = make_string_pool({"app_name", "title", "primary_color", "AppTheme"});

  w.u32(288);     // typeStrings
  w.u32(3);       // lastPublicType
  w.u32(288 + type_strings.size());  // keyStrings
  w.u32(4);       // lastPublicKey
  w.u32(0);       // typeIdOffset

  w.append(type_strings);
  w.append(key_strings);

  // TypeSpec: string (id=1)
  w.u16(0x0202);
  w.u16(16);
  w.u32(16 + 2 * 4);
  w.u8(1);  // id
  w.u8(0); w.u16(0);
  w.u32(2); // entry count
  w.u32(0); w.u32(0); // flags

  // Type: string (id=1)
  size_t type_start = w.data.size();
  w.u16(0x0201);
  w.u16(84); // 20 + 64 (ResConfig)
  w.u32(0);
  w.u8(1); w.u8(0); w.u16(0);
  w.u32(2); // entry count
  w.u32(84 + 8); // entriesStart

  // ResConfig (64 bytes)
  w.u32(64);
  for (int i = 4; i < 64; ++i) w.u8(0);

  w.u32(0);   // offset entry 0
  w.u32(16);  // offset entry 1

  // Entry 0
  w.u16(8); w.u16(0); w.u32(0); // key = app_name
  w.u16(8); w.u8(0); w.u8(kTypeString); w.u32(0); // global_pool index 0

  // Entry 1
  w.u16(8); w.u16(0); w.u32(1); // key = title
  w.u16(8); w.u8(0); w.u8(kTypeString); w.u32(1); // global_pool index 1

  w.patch_size(type_start);

  // TypeSpec: style (id=3)
  w.u16(0x0202); w.u16(16); w.u32(16 + 1 * 4);
  w.u8(3); w.u8(0); w.u16(0);
  w.u32(1);
  w.u32(0); // flags

  // Type: style (id=3)
  size_t type3_start = w.data.size();
  w.u16(0x0201); w.u16(84); w.u32(0);
  w.u8(3); w.u8(0); w.u16(0);
  w.u32(1);
  w.u32(84 + 4); // entriesStart
  w.u32(64);
  for (int i = 4; i < 64; ++i) w.u8(0);
  w.u32(0); // offset entry 0

  // Entry 0 (Complex/Bag)
  w.u16(16); w.u16(0x0001 /* kEntryComplex */); w.u32(3); // key = AppTheme
  w.u32(0); // parent
  w.u32(1); // count
  // Map 0
  w.u32(0x01010000); // map key (android attribute, arbitrary)
  w.u16(8); w.u8(0); w.u8(kTypeIntBoolean); w.u32(1); // map value (true)

  w.patch_size(type3_start);

  w.patch_size(pkg_start);
  w.patch_size(0);
  return w.data;
}

// string/greeting (0x7f010000) in seven variants, each value naming its
// qualifiers. `reverse` stores the variants in the opposite order: the
// selection must not depend on it.
const std::vector<std::string> kVariantNames = {"default", "fr", "fr-CA", "hdpi", "night", "v24", "xhdpi"};

std::vector<uint8_t> make_variants_arsc(bool reverse) {
  std::vector<ResConfig> configs(kVariantNames.size());
  configs[1] = locale_qualifier("fr");
  configs[2] = locale_qualifier("fr", "CA");
  configs[3].density = 240;
  configs[4].ui_mode = 0x20;  // UI_MODE_NIGHT_YES, any type
  configs[5].sdk_version = 24;
  configs[6].density = 320;
  std::vector<Variant> variants;
  for (size_t i = 0; i < configs.size(); ++i) {
    variants.push_back(Variant{configs[i], {{0, simple_entry(0, kTypeString, uint32_t(i))}}});
  }
  if (reverse) std::reverse(variants.begin(), variants.end());
  // CONFIG_LOCALE | CONFIG_DENSITY | CONFIG_VERSION | CONFIG_UI_MODE
  TypeDef strings{1, {0x0004 | 0x0100 | 0x0400 | 0x1000}, variants};
  return make_table(make_string_pool(kVariantNames),
                    {make_package(0x7f, u"com.test", {"string"}, {"greeting"}, {strings})});
}

// integer/zero..five: a default variant with 16-bit offsets (entries 0, 1, 3)
// and a sparse -v21 variant of compact entries (1, 4, 5).
std::vector<uint8_t> make_compact_arsc() {
  Variant offset16{ResConfig{},
                   {{0, simple_entry(0, kIntDec, 10)}, {1, simple_entry(1, kIntDec, 11)}, {3, simple_entry(3, kIntDec, 13)}},
                   kOffset16};
  ResConfig v21{};
  v21.sdk_version = 21;
  Variant sparse{v21,
                 {{1, compact_entry(1, kIntDec, 111)},
                  {4, compact_entry(4, kIntDec, 114)},
                  {5, compact_entry(5, kTypeIntBoolean, 1)}},
                 kSparse};
  TypeDef integers{1, std::vector<uint32_t>(6, 0x0400), {offset16, sparse}};
  return make_table(make_string_pool({"unused"}),
                    {make_package(0x7f, u"com.test", {"integer"}, {"zero", "one", "two", "three", "four", "five"},
                                  {integers})});
}

// --- A framework and an application, for styles, themes and TypedArray ---

// Framework attributes. They need no entries of their own: styles and
// themes only use their ids.
constexpr uint32_t kAttrTextColor = 0x01010000, kAttrBackground = 0x01010001, kAttrTextSize = 0x01010002,
                   kAttrColorPrimary = 0x01010003, kAttrColorAccent = 0x01010004, kAttrLabel = 0x01010005,
                   kAttrPadding = 0x01010006, kAttrButtonStyle = 0x01010007, kAttrHint = 0x01010008,
                   kAttrMissing = 0x01010009;
constexpr uint32_t kFwRed = 0x01020000, kFwBlue = 0x01020001, kFwBase = 0x01030000;
constexpr uint32_t kAppGreen = 0x7f010000, kAppHello = 0x7f020000, kAppChild = 0x7f030000, kAppTheme = 0x7f030001,
                   kAppOverlay = 0x7f030002, kAppXmlStyle = 0x7f030003, kAppLayout = 0x7f040000;
constexpr uint32_t kRed = 0xffff0000, kBlue = 0xff0000ff, kGreen = 0xff00ff00;

// Package 0x01 "android" (cookie 0). Type spec flags use one distinct bit per
// resource so that changing-configuration masks show where they come from.
std::vector<uint8_t> make_framework_arsc() {
  TypeDef colors{2,
                 {0x0001, 0x0002},
                 {Variant{ResConfig{},
                          {{0, simple_entry(0, kIntColorArgb8, kRed)}, {1, simple_entry(1, kIntColorArgb8, kBlue)}}}}};
  // style/Base: textColor @color/red, background @color/blue, textSize 14dp.
  TypeDef styles{3,
                 {0x0004},
                 {Variant{ResConfig{},
                          {{0, bag_entry(2, 0,
                                         {{kAttrTextColor, kTypeReference, kFwRed},
                                          {kAttrBackground, kTypeReference, kFwBlue},
                                          {kAttrTextSize, kTypeDimension, dip(14)}})}}}}};
  return make_table(make_string_pool({}), {make_package(0x01, u"android", {"attr", "color", "style"},
                                                        {"red", "blue", "Base"}, {colors, styles})});
}

// Package 0x7f "com.test" (cookie 1).
std::vector<uint8_t> make_app_arsc() {
  ResConfig hdpi{};
  hdpi.density = 240;
  // color-hdpi/green: its only variant, so selected at any density, and the
  // density it reports is 240.
  TypeDef colors{1, {0x1000}, {Variant{hdpi, {{0, simple_entry(0, kIntColorArgb8, kGreen)}}}}};
  TypeDef strings{2, {0x0010}, {Variant{ResConfig{}, {{0, simple_entry(1, kTypeString, 0)}}}}};
  TypeDef styles{
      3,
      {0x0080, 0x0800, 0x4000, 0x2000},
      {Variant{ResConfig{},
               {// Child, parent @android:style/Base: overrides textColor, adds label and hint.
                {0, bag_entry(2, kFwBase,
                              {{kAttrTextColor, kTypeReference, kAppGreen},
                               {kAttrLabel, kTypeReference, kAppHello},
                               {kAttrHint, kIntDec, 42}})},
                // AppTheme: colorAccent is ?colorPrimary.
                {1, bag_entry(3, 0,
                              {{kAttrColorPrimary, kTypeReference, kAppGreen},
                               {kAttrColorAccent, kTypeAttribute, kAttrColorPrimary},
                               {kAttrPadding, kTypeDimension, dip(8)},
                               {kAttrButtonStyle, kTypeReference, kAppChild}})},
                // Overlay: colorPrimary @android:color/red.
                {2, bag_entry(4, 0, {{kAttrColorPrimary, kTypeReference, kFwRed}})},
                // XmlStyle: textSize 20dp, colorAccent ?colorPrimary.
                {3, bag_entry(5, 0,
                              {{kAttrTextSize, kTypeDimension, dip(20)},
                               {kAttrColorAccent, kTypeAttribute, kAttrColorPrimary}})}}}}};
  return make_table(make_string_pool({"Bonjour"}),
                    {make_package(0x7f, u"com.test", {"color", "string", "style"},
                                  {"green", "hello", "Child", "AppTheme", "Overlay", "XmlStyle"},
                                  {colors, strings, styles})});
}

void setup_framework_and_app(AssetManager& am) {
  am.set_apk_assets({load("framework-res.arsc", make_framework_arsc(), ApkAssets::kSystem),
                     load("app.arsc", make_app_arsc())});
  am.set_configurations({device("en-US", 160, 30)});
}

// --- Compiled XML ---

// Strings of the layout below. aapt2 puts the attribute names that have a
// resource id first, in the order of the resource map.
const std::vector<std::u16string> kLayoutStrings = {
    u"background", u"label", u"hint", u"android", u"http://schemas.android.com/apk/res/android",
    u"Button",     u"custom", u"style", u"Salut",  u"1",
    u"Bonjour le monde"};
constexpr uint32_t kStrBackground = 0, kStrLabel = 1, kStrHint = 2, kStrAndroid = 3, kStrAndroidNs = 4,
                   kStrButton = 5, kStrCustom = 6, kStrStyle = 7, kStrSalut = 8, kStrOne = 9, kStrText = 10;

std::vector<uint8_t> xml_node(uint16_t type, uint32_t line, const std::vector<uint8_t>& ext) {
  ChunkWriter w;
  w.u16(type);
  w.u16(16);  // ResXMLTree_node
  w.u32(0);   // size, patched below
  w.u32(line);
  w.u32(kNoIndex);  // comment
  w.append(ext);
  w.patch_size(0);
  return w.data;
}

// <Button xmlns:android="http://schemas.android.com/apk/res/android"
//         android:background="?android:attr/colorAccent" android:label="Salut"
//         android:hint="@null" custom="1" style="@style/XmlStyle">
//   Bonjour le monde
// </Button>
std::vector<uint8_t> make_layout_xml() {
  ChunkWriter w;
  w.u16(0x0003);  // kChunkXml
  w.u16(8);
  w.u32(0);  // size, patched below
  w.append(encode_string_pool(kLayoutStrings, false));

  // Resource map: string i is the name of attribute ids[i].
  w.u16(0x0180);
  w.u16(8);
  w.u32(8 + 3 * 4);
  w.u32(kAttrBackground);
  w.u32(kAttrLabel);
  w.u32(kAttrHint);

  ChunkWriter ns;  // ResXMLTree_namespaceExt
  ns.u32(kStrAndroid);
  ns.u32(kStrAndroidNs);
  w.append(xml_node(0x0100, 1, ns.data));

  ChunkWriter element;  // ResXMLTree_attrExt
  element.u32(kNoIndex);  // ns
  element.u32(kStrButton);
  element.u16(20);  // attributeStart
  element.u16(20);  // attributeSize
  element.u16(5);   // attributeCount
  element.u16(0);   // idIndex
  element.u16(0);   // classIndex
  element.u16(5);   // styleIndex, 1-based
  struct Attr {
    uint32_t ns, name, raw;
    uint8_t type;
    uint32_t value;
  };
  const Attr attrs[] = {
      {kStrAndroidNs, kStrBackground, kNoIndex, kTypeAttribute, kAttrColorAccent},
      {kStrAndroidNs, kStrLabel, kStrSalut, kTypeString, kStrSalut},
      {kStrAndroidNs, kStrHint, kNoIndex, kTypeReference, 0},  // @null
      {kNoIndex, kStrCustom, kStrOne, kIntDec, 1},
      // A reference of package 0, as in a shared library: owned by the tree's package.
      {kNoIndex, kStrStyle, kNoIndex, kTypeDynamicReference, kAppXmlStyle & 0x00ffffffu},
  };
  for (const Attr& a : attrs) {
    element.u32(a.ns);
    element.u32(a.name);
    element.u32(a.raw);
    element.u16(8);
    element.u8(0);
    element.u8(a.type);
    element.u32(a.value);
  }
  w.append(xml_node(0x0102, 2, element.data));

  ChunkWriter text;  // ResXMLTree_cdataExt
  text.u32(kStrText);
  text.u16(8);
  text.u8(0);
  text.u8(kTypeNull);
  text.u32(0);
  w.append(xml_node(0x0104, 3, text.data));

  ChunkWriter end;  // ResXMLTree_endElementExt
  end.u32(kNoIndex);
  end.u32(kStrButton);
  w.append(xml_node(0x0103, 4, end.data));
  w.append(xml_node(0x0101, 4, ns.data));
  w.patch_size(0);
  return w.data;
}

// One TypedArray slot (7 values) in readable form.
std::string style_row(size_t index, std::span<const uint32_t> v) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "#%u type=0x%02x data=0x%08x cookie=%d resid=0x%08x flags=0x%x density=%u source=0x%08x",
                unsigned(index), v[0], v[1], int(int32_t(v[2])), v[3], v[4], v[5], v[6]);
  return buf;
}

std::string join(std::span<const uint32_t> v) {
  std::string out;
  for (uint32_t x : v) {
    if (!out.empty()) out += ' ';
    out += std::to_string(x);
  }
  return out;
}

using StyleRow = std::array<uint32_t, kStyleNumEntries>;

void check_style_rows(std::span<const uint32_t> values, std::span<const StyleRow> expected) {
  CHECK_EQ(values.size(), expected.size() * kStyleNumEntries);
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK_EQ(style_row(i, values.subspan(i * kStyleNumEntries, kStyleNumEntries)), style_row(i, expected[i]));
  }
}

constexpr uint32_t kNoCookie = 0xffffffffu;  // Java cookie -1: not from a table

}  // namespace

TEST(test_string_pool) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(make_string_pool({"Bonjour", "Hello"}));
  auto pool = StringPool::parse(bytes, 0, bytes->size());
  CHECK_EQ(pool->size(), 2u);
  CHECK_EQ(pool->string8(0).value_or(""), std::string("Bonjour"));
  CHECK_EQ(pool->string8(1).value_or(""), std::string("Hello"));
  CHECK_EQ(pool->find(u"Bonjour").value_or(99), 0u);
  CHECK(!pool->string8(2).has_value());

  // UTF-8 pools: the UTF-16 length and the byte length differ outside ASCII,
  // and take two bytes above 0x7f (300: both bytes matter).
  const std::u16string long_text(300, u'x');
  auto utf8 = std::make_shared<std::vector<uint8_t>>(
      encode_string_pool({u"été", u"\U0001F600", long_text}, true));
  auto p8 = StringPool::parse(utf8, 0, utf8->size());
  CHECK(p8->utf8());
  CHECK_EQ(p8->string8(0).value_or(""), std::string("\xC3\xA9t\xC3\xA9"));
  CHECK_EQ(p8->string16(0).value_or(u""), std::u16string(u"été"));
  CHECK_EQ(p8->string16(1).value_or(u""), std::u16string(u"\U0001F600"));
  CHECK_EQ(p8->string8(2).value_or("").size(), 300u);
  CHECK_EQ(p8->string16(2).value_or(u""), long_text);
  CHECK_EQ(p8->find(long_text).value_or(99), 2u);
}

TEST(test_string_pool_utf16_and_styles) {
  // String 0 is "<i>Hello</i> <b>world</b>": span names are strings of the
  // pool, positions are UTF-16 units, the last one inclusive.
  const std::vector<std::u16string> strings = {u"Hello world", u"b", u"été", u"\U0001F600 ok", u"i"};
  auto bytes = std::make_shared<std::vector<uint8_t>>(encode_string_pool(strings, false, {{1, 6, 10, 4, 0, 4}}));
  auto pool = StringPool::parse(bytes, 0, bytes->size());
  CHECK(!pool->utf8());
  CHECK_EQ(pool->size(), 5u);
  CHECK_EQ(pool->string16(0).value_or(u""), std::u16string(u"Hello world"));
  CHECK_EQ(pool->string8(1).value_or(""), std::string("b"));
  CHECK_EQ(pool->string8(2).value_or(""), std::string("\xC3\xA9t\xC3\xA9"));
  // A character above U+FFFF: two UTF-16 units, four UTF-8 bytes.
  CHECK_EQ(pool->string16(3).value_or(u""), std::u16string(u"\U0001F600 ok"));
  CHECK_EQ(pool->string16(3).value_or(u"").size(), 5u);
  CHECK_EQ(pool->string8(3).value_or(""), std::string("\xF0\x9F\x98\x80 ok"));
  CHECK_EQ(pool->find(u"été").value_or(99), 2u);
  CHECK(!pool->string16(5).has_value());

  CHECK_EQ(join(pool->style(0)), std::string("1 6 10 4 0 4"));
  CHECK(pool->style(1).empty());  // only string 0 has a style
  CHECK(pool->style(4).empty());
}

TEST(test_res_config) {
  ResConfig fr_fr{};
  fr_fr.set_bcp47_locale("fr-FR");
  CHECK_EQ(fr_fr.bcp47_locale(), std::string("fr-FR"));
  CHECK(fr_fr.locale_script_was_computed);
  CHECK_EQ(std::string(fr_fr.locale_script, 4), std::string("Latn"));

  // match() checks the language and, when both are known, the script; the
  // region is left to is_better_than.
  CHECK(ResConfig{}.match(fr_fr));
  CHECK(locale_qualifier("fr").match(fr_fr));
  CHECK(locale_qualifier("fr", "CA").match(fr_fr));
  CHECK(!locale_qualifier("de").match(fr_fr));
  // zh-TW is Hant: values-zh and values-zh-rCN (both Hans) do not serve it.
  ResConfig zh_tw{};
  zh_tw.set_bcp47_locale("zh-TW");
  CHECK(locale_qualifier("zh", "TW").match(zh_tw));
  CHECK(!locale_qualifier("zh", "CN").match(zh_tw));
  CHECK(!locale_qualifier("zh").match(zh_tw));

  // Density never excludes a variant: resources are scaled.
  ResConfig hdpi{}, xhdpi{};
  hdpi.density = 240;
  xhdpi.density = 320;
  CHECK(hdpi.match(device("en-US", 120, 30)));
  CHECK(hdpi.match(device("en-US", 640, 30)));
  // The version is a minimum.
  ResConfig v24{};
  v24.sdk_version = 24;
  CHECK(!v24.match(device("en-US", 160, 23)));
  CHECK(v24.match(device("en-US", 160, 24)));
  // Night must equal the device's.
  ResConfig night{};
  night.ui_mode = 0x20;
  CHECK(!night.match(device("en-US", 160, 30, kUiDay)));
  CHECK(night.match(device("en-US", 160, 30, kUiNight)));

  // isBetterThan between buckets: scaling down counts twice as good as
  // scaling up, so hdpi wins while (2 * 240 - d) * 320 > d * d.
  ResConfig wanted{};
  wanted.density = 260;  // 70400 > 67600
  CHECK(hdpi.is_better_than(xhdpi, &wanted));
  CHECK(!xhdpi.is_better_than(hdpi, &wanted));
  wanted.density = 280;  // 64000 <= 78400
  CHECK(xhdpi.is_better_than(hdpi, &wanted));
  CHECK(!hdpi.is_better_than(xhdpi, &wanted));
  wanted.density = 160;  // both above: the smaller, the unqualified one counting as 160
  CHECK(ResConfig{}.is_better_than(hdpi, &wanted));
  CHECK(!hdpi.is_better_than(ResConfig{}, &wanted));

  // Without a request, isMoreSpecificThan: a set qualifier beats an unset
  // one, density excepted, the most important qualifier first.
  CHECK(v24.is_better_than(ResConfig{}, nullptr));
  CHECK(!ResConfig{}.is_better_than(v24, nullptr));
  CHECK(!hdpi.is_better_than(ResConfig{}, nullptr));
  CHECK(!ResConfig{}.is_better_than(hdpi, nullptr));
  CHECK(night.is_better_than(v24, nullptr));
  CHECK(!v24.is_better_than(night, nullptr));
  CHECK(locale_qualifier("fr").is_better_than(night, nullptr));
}

// Expected picks follow ResTable_config::match then isBetterThan, whose
// qualifiers are compared in this order: locale, ..., ui mode, density, ...,
// version.
TEST(test_variant_selection) {
  for (bool reverse : {false, true}) {
    AssetManager am;
    am.set_apk_assets({load("variants.arsc", make_variants_arsc(reverse))});
    auto pick = [&](const ResConfig& config, uint16_t density = 0) -> std::string {
      am.set_configurations({config});
      auto v = am.get_resource(0x7f010000u, false, density);
      if (!v) return "<absent>";
      CHECK_EQ(v->type, kTypeString);
      CHECK_EQ(v->flags, 0x1504u);
      CHECK_EQ(v->resid, 0x7f010000u);
      auto pool = am.string_pool(v->cookie);
      CHECK(pool != nullptr);
      std::string name = pool->string8(v->data).value_or("<?>");
      // The density reported is the selected variant's.
      CHECK_EQ(v->density, name == "hdpi" ? 240 : name == "xhdpi" ? 320 : 0);
      return name;
    };

    // en-US, mdpi, API 23: fr* need French, night a night device, v24 API 24;
    // default, hdpi and xhdpi remain. At or below both densities the smaller
    // wins, an unqualified variant counting as 160: exact here.
    CHECK_EQ(pick(device("en-US", 160, 23)), "default");
    // API 30: v24 matches. Against default only the version differs (higher
    // wins); against hdpi/xhdpi the density decides first (160 is exact).
    CHECK_EQ(pick(device("en-US", 160, 30)), "v24");
    CHECK_EQ(pick(device("en-US", 160, 24)), "v24");
    // Another language: only the unlocalized variants match.
    CHECK_EQ(pick(device("de-DE", 160, 23)), "default");
    // fr-FR: fr-CA matches too (same language, both Latn) but
    // isLocaleBetterThan ranks regions by the request's ancestors (fr-FR,
    // then fr): "fr" is one, "CA" is not. The locale comes before every
    // other qualifier, so it also beats v24.
    CHECK_EQ(pick(device("fr-FR", 160, 23)), "fr");
    CHECK_EQ(pick(device("fr-FR", 160, 30)), "fr");
    // fr-CA: the exact region.
    CHECK_EQ(pick(device("fr-CA", 160, 23)), "fr-CA");
    // fr-CA at night: night matches, but the locale is compared first.
    CHECK_EQ(pick(device("fr-CA", 160, 23, kUiNight)), "fr-CA");
    // en-US at night: the ui mode comes before density and version.
    CHECK_EQ(pick(device("en-US", 160, 30, kUiNight)), "night");
    // xxhdpi: every bucket is below, the largest wins.
    CHECK_EQ(pick(device("en-US", 480, 23)), "xhdpi");
    // Between hdpi and xhdpi: (2 * 240 - d) * 320 > d * d keeps hdpi.
    CHECK_EQ(pick(device("en-US", 260, 23)), "hdpi");
    CHECK_EQ(pick(device("en-US", 280, 23)), "xhdpi");
    // An explicit density (getDrawableForDensity) replaces the device's.
    CHECK_EQ(pick(device("en-US", 160, 23), 240), "hdpi");
    CHECK_EQ(pick(device("en-US", 480, 23), 160), "default");
  }
}

TEST(test_sparse_offset16_compact_entries) {
  AssetManager am;
  am.set_apk_assets({load("compact.arsc", make_compact_arsc())});
  auto get = [&](uint32_t resid) -> std::string {
    auto v = am.get_resource(resid, false);
    if (!v) return "<absent>";
    return std::to_string(v->type) + ":" + std::to_string(v->data);
  };
  am.set_configurations({device("en-US", 160, 30)});
  CHECK_EQ(get(0x7f010000u), std::string("16:10"));
  CHECK_EQ(get(0x7f010001u), std::string("16:111"));  // the -v21 sparse variant wins
  CHECK_EQ(get(0x7f010002u), std::string("<absent>"));  // 0xffff offset, not in the sparse list
  CHECK_EQ(get(0x7f010003u), std::string("16:13"));
  CHECK_EQ(get(0x7f010004u), std::string("16:114"));
  CHECK_EQ(get(0x7f010005u), std::string("18:1"));  // compact: type from the flags' high byte
  CHECK_EQ(get(0x7f010006u), std::string("<absent>"));  // beyond the type spec
  am.set_configurations({device("en-US", 160, 19)});
  CHECK_EQ(get(0x7f010001u), std::string("16:11"));
  CHECK_EQ(get(0x7f010004u), std::string("<absent>"));

  // Compact entries name themselves with a 16-bit key.
  auto name = am.resource_name(0x7f010004u);
  CHECK(name.has_value());
  CHECK_EQ(name->type, u"integer");
  CHECK_EQ(name->entry, u"four");
  CHECK_EQ(am.resource_id("integer/five", "", "com.test"), 0x7f010005u);
  CHECK_EQ(am.resource_id("com.test:integer/three", "", ""), 0x7f010003u);
  CHECK_EQ(am.resource_id("two", "integer", "com.test"), 0u);  // no variant defines it
}

TEST(test_asset_manager_arsc) {
  auto data = make_arsc();
  auto apk = load("test.arsc", data);

  AssetManager am;
  am.set_apk_assets({apk});

  auto resid = am.resource_id("app_name", "string", "com.test");
  CHECK_EQ(resid, 0x7f010000u);

  auto val = am.get_resource(0x7f010000u, false);
  CHECK(val.has_value());
  CHECK_EQ(val->type, kTypeString);
  CHECK_EQ(val->data, 0u);

  auto pool = am.string_pool(val->cookie);
  CHECK(pool != nullptr);
  CHECK_EQ(pool->string8(val->data).value_or(""), std::string("Valeur String 1"));

  auto name = am.resource_name(0x7f010001u);
  CHECK(name.has_value());
  CHECK_EQ(name->package, u"com.test");
  CHECK_EQ(name->type, u"string");
  CHECK_EQ(name->entry, u"title");

  auto bag = am.get_bag(0x7f030000u); // style/AppTheme
  CHECK(bag != nullptr);
  CHECK_EQ(bag->entries.size(), 1u);
  CHECK_EQ(bag->entries[0].key, 0x01010000u);
  CHECK_EQ(bag->entries[0].value.type, kTypeIntBoolean);
  CHECK_EQ(bag->entries[0].value.data, 1u);
}

TEST(test_theme_and_apply_style) {
  auto apk = load("test.arsc", make_arsc());
  AssetManager am;
  am.set_apk_assets({apk});

  Theme theme(&am);
  CHECK(theme.apply_style(0x7f030000u, true));

  auto attr = theme.get_attribute(0x01010000u);
  CHECK(attr.has_value());
  CHECK_EQ(attr->type, kTypeIntBoolean);
  CHECK_EQ(attr->data, 1u);
}

TEST(test_style_inheritance_and_theme) {
  AssetManager am;
  setup_framework_and_app(am);

  // Child (application) inherits from Base (framework) and overrides
  // textColor; each entry keeps the cookie and the style that define it.
  auto child = am.get_bag(kAppChild);
  CHECK(child != nullptr);
  CHECK_EQ(child->parent, kFwBase);
  CHECK_EQ(child->type_spec_flags, 0x0080u | 0x0004u);
  CHECK_EQ(child->entries.size(), 5u);
  for (size_t i = 1; i < child->entries.size(); ++i) CHECK(child->entries[i - 1].key < child->entries[i].key);
  const BagEntry* text_color = child->find(kAttrTextColor);
  CHECK(text_color != nullptr);
  CHECK_EQ(text_color->value.type, kTypeReference);
  CHECK_EQ(text_color->value.data, kAppGreen);
  CHECK_EQ(text_color->cookie, 1);
  CHECK_EQ(text_color->style, kAppChild);
  const BagEntry* background = child->find(kAttrBackground);
  CHECK(background != nullptr);
  CHECK_EQ(background->value.data, kFwBlue);
  CHECK_EQ(background->cookie, 0);
  CHECK_EQ(background->style, kFwBase);
  const BagEntry* text_size = child->find(kAttrTextSize);
  CHECK(text_size != nullptr);
  CHECK_EQ(text_size->value.type, kTypeDimension);
  CHECK_EQ(text_size->value.data, dip(14));
  const BagEntry* hint = child->find(kAttrHint);
  CHECK(hint != nullptr);
  CHECK_EQ(hint->value.data, 42u);
  CHECK_EQ(hint->style, kAppChild);
  CHECK(child->find(kAttrPadding) == nullptr);
  // The parent keeps its own values.
  auto base = am.get_bag(kFwBase);
  CHECK(base != nullptr);
  CHECK_EQ(base->entries.size(), 3u);
  CHECK(base->find(kAttrTextColor) != nullptr);
  CHECK_EQ(base->find(kAttrTextColor)->value.data, kFwRed);
  CHECK(am.get_bag(kAppGreen) == nullptr);  // not a bag

  Theme theme(&am);
  CHECK(!theme.apply_style(kAppGreen, false));
  CHECK(theme.apply_style(kAppTheme, false));
  CHECK_EQ(theme.changing_configurations(), 0x0800u);
  auto primary = theme.get_attribute(kAttrColorPrimary);
  CHECK(primary.has_value());
  CHECK_EQ(primary->type, kTypeReference);
  CHECK_EQ(primary->data, kAppGreen);
  CHECK_EQ(primary->cookie, 1);
  CHECK_EQ(primary->flags, 0x0800u);
  // get_attribute follows ?colorPrimary itself.
  auto accent = theme.get_attribute(kAttrColorAccent);
  CHECK(accent.has_value());
  CHECK_EQ(accent->type, kTypeReference);
  CHECK_EQ(accent->data, kAppGreen);
  CHECK(!theme.get_attribute(kAttrTextColor).has_value());

  // ?colorAccent -> ?colorPrimary -> @color/green.
  SelectedValue attr_value;
  attr_value.type = kTypeAttribute;
  attr_value.data = kAttrColorAccent;
  CHECK(theme.resolve_attribute_reference(attr_value));
  CHECK_EQ(attr_value.type, kIntColorArgb8);
  CHECK_EQ(attr_value.data, kGreen);
  CHECK_EQ(attr_value.cookie, 1);
  CHECK_EQ(attr_value.resid, kAppGreen);
  CHECK_EQ(attr_value.flags, 0x0800u | 0x1000u);
  CHECK_EQ(attr_value.density, 240);

  // @android:color/red.
  SelectedValue ref_value;
  ref_value.type = kTypeReference;
  ref_value.data = kFwRed;
  CHECK(theme.resolve_attribute_reference(ref_value));
  CHECK_EQ(ref_value.type, kIntColorArgb8);
  CHECK_EQ(ref_value.data, kRed);
  CHECK_EQ(ref_value.cookie, 0);
  CHECK_EQ(ref_value.resid, kFwRed);
  CHECK_EQ(ref_value.flags, 0x0001u);

  SelectedValue missing;
  missing.type = kTypeAttribute;
  missing.data = kAttrMissing;
  CHECK(!theme.resolve_attribute_reference(missing));
  CHECK_EQ(missing.type, kTypeAttribute);
  SelectedValue dangling;
  dangling.type = kTypeReference;
  dangling.data = 0x7f010005u;
  CHECK(!theme.resolve_attribute_reference(dangling));
  CHECK_EQ(dangling.resid, 0x7f010005u);

  // Without force an applied style keeps the attributes already set...
  CHECK(theme.apply_style(kAppOverlay, false));
  CHECK_EQ(theme.get_attribute(kAttrColorPrimary).value().data, kAppGreen);
  // ...with force it replaces them, and ?colorAccent follows: attribute
  // references are resolved at lookup.
  CHECK(theme.apply_style(kAppOverlay, true));
  CHECK_EQ(theme.get_attribute(kAttrColorPrimary).value().data, kFwRed);
  CHECK_EQ(theme.get_attribute(kAttrColorPrimary).value().cookie, 1);  // the overlay's package
  SelectedValue accent_again;
  accent_again.type = kTypeAttribute;
  accent_again.data = kAttrColorAccent;
  CHECK(theme.resolve_attribute_reference(accent_again));
  CHECK_EQ(accent_again.data, kRed);
  CHECK_EQ(accent_again.cookie, 0);
  CHECK_EQ(accent_again.resid, kFwRed);
  CHECK_EQ(accent_again.flags, 0x0001u | 0x0800u | 0x4000u);
  CHECK_EQ(theme.changing_configurations(), 0x0800u | 0x4000u);
}

// TypedArray resolution (AttributeResolution.cpp): XML attributes first, then
// the XML style, the default style, the theme.
TEST(test_apply_style_xml) {
  AssetManager am;
  setup_framework_and_app(am);
  Theme theme(&am);
  CHECK(theme.apply_style(kAppTheme, false));

  auto tree = XmlTree::parse(make_layout_xml(), 0x7f);
  XmlParser parser(tree, kAppLayout);
  CHECK_EQ(parser.next(), XmlParser::kStartNamespace);
  CHECK_EQ(parser.next(), XmlParser::kStartTag);

  const std::vector<uint32_t> attrs = {kAttrTextColor,   kAttrBackground, kAttrTextSize,
                                       kAttrColorPrimary, kAttrColorAccent, kAttrLabel,
                                       kAttrPadding,      kAttrHint,        kAttrMissing};
  std::vector<uint32_t> values(attrs.size() * kStyleNumEntries, 0xcdcdcdcdu);
  std::vector<uint32_t> indices(attrs.size() + 1, 0xcdcdcdcdu);
  // ?buttonStyle (Child) takes precedence over the default style resource (Base).
  apply_style(theme, &parser, kAttrButtonStyle, kFwBase, attrs, values, indices);
  // {type, data, Java cookie (cookie + 1), resid, changing configurations, density, source style}
  const StyleRow expected[] = {
      // textColor: default style (Child's override), @color/green resolved;
      // flags: bag (Child | Base) | theme (buttonStyle) | green.
      {kIntColorArgb8, kGreen, 2, kAppGreen, 0x0080 | 0x0004 | 0x0800 | 0x1000, 240, kAppChild},
      // background: XML ?colorAccent -> ?colorPrimary -> @color/green; beats Base's blue.
      {kIntColorArgb8, kGreen, 2, kAppGreen, 0x0800 | 0x1000, 240, kAppLayout},
      // textSize: the XML style (20dp) beats the default style (Base's 14dp).
      {kTypeDimension, dip(20), 2, 0, 0x2000, 0, kAppXmlStyle},
      // colorPrimary: only in the theme.
      {kIntColorArgb8, kGreen, 2, kAppGreen, 0x0800 | 0x1000, 240, 0},
      // colorAccent: the XML style's ?colorPrimary (it beats the theme's own
      // colorAccent), resolved through the theme; the style's flags are kept.
      {kIntColorArgb8, kGreen, 2, kAppGreen, 0x2000 | 0x0800 | 0x1000, 240, kAppXmlStyle},
      // label: the XML literal beats Child's @string/hello; a raw XML string
      // has no cookie (TypedArray reads it from the XML block's pool).
      {kTypeString, kStrSalut, kNoCookie, 0, 0, 0, kAppLayout},
      // padding: theme value that is not a reference.
      {kTypeDimension, dip(8), 2, 0, 0x0800, 0, 0},
      // hint: XML @null hides Child's 42 and reads as undefined.
      {kTypeNull, 0, kNoCookie, 0, 0, 0, kAppLayout},
      // Nowhere.
      {kTypeNull, 0, kNoCookie, 0, 0, 0, 0},
  };
  check_style_rows(values, expected);
  CHECK_EQ(join(std::span<const uint32_t>(indices).first(8)), std::string("7 0 1 2 3 4 5 6"));
  CHECK_EQ(indices[8], 0xcdcdcdcdu);

  // No XML; a default style attribute missing from the theme falls back to
  // the default style resource. Inherited entries report Base as source.
  const std::vector<uint32_t> attrs2 = {kAttrTextColor, kAttrBackground, kAttrLabel, kAttrHint};
  std::vector<uint32_t> values2(attrs2.size() * kStyleNumEntries, 0xcdcdcdcdu);
  std::vector<uint32_t> indices2(attrs2.size() + 1, 0xcdcdcdcdu);
  apply_style(theme, nullptr, kAttrMissing, kAppChild, attrs2, values2, indices2);
  const StyleRow expected2[] = {
      {kIntColorArgb8, kGreen, 2, kAppGreen, 0x0084 | 0x1000, 240, kAppChild},
      {kIntColorArgb8, kBlue, 1, kFwBlue, 0x0084 | 0x0002, 0, kFwBase},
      {kTypeString, 0, 2, kAppHello, 0x0084 | 0x0010, 0, kAppChild},
      {kIntDec, 42, 2, 0, 0x0084, 0, kAppChild},
  };
  check_style_rows(values2, expected2);
  CHECK_EQ(join(indices2), std::string("4 0 1 2 3"));
  CHECK_EQ(am.string_pool(1)->string8(0).value_or(""), std::string("Bonjour"));

  // obtainAttributes: XML only, no theme: ?attr stays unresolved.
  const std::vector<uint32_t> attrs3 = {kAttrBackground, kAttrLabel, kAttrHint, kAttrTextColor};
  std::vector<uint32_t> values3(attrs3.size() * kStyleNumEntries, 0);
  std::vector<uint32_t> indices3(attrs3.size() + 1, 0);
  retrieve_attributes(am, &parser, attrs3, values3, indices3);
  const StyleRow expected3[] = {
      {kTypeAttribute, kAttrColorAccent, kNoCookie, 0, 0, 0, 0},
      {kTypeString, kStrSalut, kNoCookie, 0, 0, 0, 0},
      {kTypeNull, 0, kNoCookie, 0, 0, 0, 0},
      {kTypeNull, 0, kNoCookie, 0, 0, 0, 0},
  };
  check_style_rows(values3, expected3);
  CHECK_EQ(join(std::span<const uint32_t>(indices3).first(3)), std::string("2 0 1"));
}

TEST(test_xml_parser) {
  auto tree = XmlTree::parse(make_layout_xml(), 0x7f);
  CHECK(tree != nullptr);
  CHECK_EQ(tree->strings()->size(), uint32_t(kLayoutStrings.size()));
  CHECK(!tree->strings()->utf8());
  CHECK_EQ(join(tree->resource_ids()), join(std::vector<uint32_t>{kAttrBackground, kAttrLabel, kAttrHint}));
  auto str = [&](int32_t index) -> std::string {
    if (index < 0) return "<" + std::to_string(index) + ">";
    return tree->strings()->string8(uint32_t(index)).value_or("<absent>");
  };
  const std::u16string android = u"http://schemas.android.com/apk/res/android";
  const std::u16string other = u"http://example.com/other";

  XmlParser p(tree, 0);
  CHECK_EQ(p.event(), XmlParser::kStartDocument);
  CHECK_EQ(p.attribute_count(), 0);

  CHECK_EQ(p.next(), XmlParser::kStartNamespace);
  CHECK_EQ(p.line(), 1);
  CHECK_EQ(p.element_name(), -1);
  CHECK_EQ(p.attribute_count(), 0);

  CHECK_EQ(p.next(), XmlParser::kStartTag);
  CHECK_EQ(p.line(), 2);
  CHECK_EQ(p.element_namespace(), -1);
  CHECK_EQ(str(p.element_name()), std::string("Button"));
  CHECK_EQ(p.attribute_count(), 5);
  // android:background="?android:attr/colorAccent"
  CHECK_EQ(str(p.attribute_namespace(0)), to_utf8(android));
  CHECK_EQ(str(p.attribute_name(0)), std::string("background"));
  CHECK_EQ(p.attribute_name_resid(0), kAttrBackground);
  CHECK_EQ(p.attribute_raw_value(0), -1);
  CHECK_EQ(p.attribute_value(0).type, kTypeAttribute);
  CHECK_EQ(p.attribute_value(0).data, kAttrColorAccent);
  // android:label="Salut"
  CHECK_EQ(str(p.attribute_name(1)), std::string("label"));
  CHECK_EQ(p.attribute_name_resid(1), kAttrLabel);
  CHECK_EQ(str(p.attribute_raw_value(1)), std::string("Salut"));
  CHECK_EQ(p.attribute_value(1).type, kTypeString);
  CHECK_EQ(str(int32_t(p.attribute_value(1).data)), std::string("Salut"));
  // custom="1": no namespace, no resource id.
  CHECK_EQ(p.attribute_namespace(3), -1);
  CHECK_EQ(str(p.attribute_name(3)), std::string("custom"));
  CHECK_EQ(p.attribute_name_resid(3), 0u);
  CHECK_EQ(p.attribute_value(3).type, kIntDec);
  CHECK_EQ(p.attribute_value(3).data, 1u);
  // style: a dynamic reference of package 0 becomes the tree's package.
  CHECK_EQ(p.attribute_value(4).type, kTypeReference);
  CHECK_EQ(p.attribute_value(4).data, kAppXmlStyle);
  // Bad indices.
  CHECK_EQ(p.attribute_namespace(5), -2);
  CHECK_EQ(p.attribute_name(-1), -1);
  CHECK_EQ(p.attribute_value(5).type, kTypeNull);
  // Lookups: a null namespace only matches attributes without one.
  CHECK_EQ(p.index_of_attribute(&android, u"label"), 1);
  CHECK_EQ(p.index_of_attribute(&android, u"hint"), 2);
  CHECK_EQ(p.index_of_attribute(nullptr, u"custom"), 3);
  CHECK_EQ(p.index_of_attribute(nullptr, u"label"), -2);
  CHECK_EQ(p.index_of_attribute(&android, u"custom"), -2);
  CHECK_EQ(p.index_of_attribute(&other, u"label"), -2);
  CHECK_EQ(p.index_of_attribute(&android, u"absent"), -2);
  CHECK_EQ(p.index_of_style(), 4);
  CHECK_EQ(p.index_of_id(), -2);
  CHECK_EQ(p.index_of_class(), -2);
  CHECK_EQ(p.index_of_resid(kAttrHint), 2);
  CHECK_EQ(p.index_of_resid(kAttrTextColor), -1);

  CHECK_EQ(p.next(), XmlParser::kText);
  CHECK_EQ(p.line(), 3);
  CHECK_EQ(str(p.text()), std::string("Bonjour le monde"));
  CHECK_EQ(p.element_name(), -1);
  CHECK_EQ(p.attribute_count(), 0);
  CHECK_EQ(p.index_of_style(), -2);

  CHECK_EQ(p.next(), XmlParser::kEndTag);
  CHECK_EQ(p.line(), 4);
  CHECK_EQ(str(p.element_name()), std::string("Button"));
  CHECK_EQ(p.element_namespace(), -1);
  CHECK_EQ(p.text(), -1);
  CHECK_EQ(p.attribute_count(), 0);
  CHECK_EQ(p.index_of_resid(kAttrHint), -1);

  CHECK_EQ(p.next(), XmlParser::kEndNamespace);
  CHECK_EQ(p.element_name(), -1);
  CHECK_EQ(p.next(), XmlParser::kEndDocument);
  CHECK_EQ(p.line(), -1);
  CHECK_EQ(p.next(), XmlParser::kEndDocument);  // stays at the end

  // A document shorter than its header says.
  auto truncated = make_layout_xml();
  truncated.resize(truncated.size() - 4);
  CHECK_THROWS(XmlTree::parse(truncated, 0x7f), FormatError);
}
