#include <cstdint>
#include <string>
#include <vector>

#include "resources/asset_manager.h"
#include "resources/res_config.h"
#include "resources/res_table.h"
#include "resources/res_xml.h"
#include "test.h"

using namespace aa::res;
using namespace aa;

namespace {

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

std::vector<uint8_t> make_string_pool(const std::vector<std::string>& strings, bool utf8 = true) {
  ChunkWriter w;
  size_t start = w.data.size();
  w.u16(0x0001);  // kChunkStringPool
  w.u16(28);      // header size
  w.u32(0);       // size placeholder
  w.u32(strings.size());
  w.u32(0);       // style count
  w.u32(utf8 ? 0x100 : 0);  // flags
  w.u32(28 + strings.size() * 4);  // strings_start
  w.u32(0);       // styles_start

  uint32_t offset = 0;
  std::vector<uint8_t> str_data;
  for (const auto& s : strings) {
    w.u32(offset);
    if (utf8) {
      str_data.push_back(s.size());
      str_data.push_back(s.size());
      for (char c : s) str_data.push_back(c);
      str_data.push_back(0);
      offset += s.size() + 3;
    } else {
      str_data.push_back(s.size() & 0xff);
      str_data.push_back(s.size() >> 8);
      for (char c : s) {
        str_data.push_back(c);
        str_data.push_back(0);
      }
      str_data.push_back(0);
      str_data.push_back(0);
      offset += s.size() * 2 + 4;
    }
  }
  w.append(str_data);
  w.pad();
  w.patch_size(start);
  return w.data;
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
  size_t spec_start = w.data.size(); (void)spec_start;
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
  size_t spec3_start = w.data.size(); (void)spec3_start;
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

std::vector<uint8_t> make_xml() {
  ChunkWriter w;
  w.u16(0x0003); // kChunkXml
  w.u16(8);
  w.u32(0); // size
  
  auto str_pool = make_string_pool({"manifest", "package", "com.test"});
  w.append(str_pool);

  size_t tag_start = w.data.size(); (void)tag_start;
  w.u16(0x0102); // kStartTag
  w.u16(16);
  w.u32(36); // size (16 + 20)
  w.u32(1); // line
  w.u32(0xffffffff); // comment
  w.u32(0xffffffff); // ns
  w.u32(0); // name (manifest)
  w.u16(20); // attrStart
  w.u16(20); // attrSize
  w.u16(1); // attrCount
  w.u16(0); w.u16(0); w.u16(0); // id, class, style

  // Attr
  w.u32(0xffffffff); // ns
  w.u32(1); // name (package)
  w.u32(2); // raw value
  w.u16(8); w.u8(0); w.u8(kTypeString); w.u32(2); // value
  
  w.patch_size(tag_start);

  w.patch_size(0);
  return w.data;
}

} // namespace

TEST(test_string_pool) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(make_string_pool({"Bonjour", "Hello"}));
  auto pool = StringPool::parse(bytes, 0, bytes->size());
  CHECK_EQ(pool->size(), 2u);
  CHECK_EQ(pool->string8(0).value_or(""), std::string("Bonjour"));
  CHECK_EQ(pool->string8(1).value_or(""), std::string("Hello"));
  CHECK_EQ(pool->find(u"Bonjour").value_or(99), 0u);
}

TEST(test_res_config) {
  ResConfig a{};
  a.set_bcp47_locale("fr-FR");
  CHECK_EQ(a.bcp47_locale(), std::string("fr-FR"));
  
  ResConfig b{};
  b.set_bcp47_locale("fr");
  CHECK(a.match(b)); 
  
  ResConfig c{};
  c.density = ResConfig::kDensityMedium;
  CHECK(!a.match(c));
}

TEST(test_asset_manager_arsc) {
  auto data = make_arsc();
  auto apk = ApkAssets::load_table("test.arsc", data, 0);
  CHECK(apk != nullptr);
  CHECK(apk->table() != nullptr);

  AssetManager am;
  am.set_apk_assets({apk});

  auto resid = am.resource_id("app_name", "string", "com.test");
  CHECK_EQ(resid, 0x7f010000u);

  auto val = am.get_resource(0x7f010000u, false);
  CHECK(val.has_value());
  CHECK_EQ(val->type, kTypeString);
  CHECK_EQ(val->data, 0u);
  
  auto pool = am.string_pool(val->cookie);
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
  auto data = make_arsc();
  auto apk = ApkAssets::load_table("test.arsc", data, 0);
  AssetManager am;
  am.set_apk_assets({apk});

  Theme theme(&am);
  CHECK(theme.apply_style(0x7f030000u, true));
  
  auto attr = theme.get_attribute(0x01010000u);
  CHECK(attr.has_value());
  CHECK_EQ(attr->type, kTypeIntBoolean);
  CHECK_EQ(attr->data, 1u);
}

TEST(test_xml_parser) {
  auto data = make_xml();
  auto tree = XmlTree::parse(data, 0x7f);
  CHECK(tree != nullptr);
  CHECK_EQ(tree->strings()->size(), 3u);
  
  XmlParser parser(tree, 0);
  int32_t event = parser.next();
  CHECK_EQ(event, XmlParser::kStartTag);
  
  auto tag = tree->strings()->string8(parser.element_name());
  CHECK_EQ(tag.value_or(""), std::string("manifest"));
  
  CHECK_EQ(parser.attribute_count(), 1);
  auto attr_name = tree->strings()->string8(parser.attribute_name(0));
  CHECK_EQ(attr_name.value_or(""), std::string("package"));
  
  auto attr_val = tree->strings()->string8(parser.attribute_value(0).data);
  CHECK_EQ(attr_val.value_or(""), std::string("com.test"));
}
