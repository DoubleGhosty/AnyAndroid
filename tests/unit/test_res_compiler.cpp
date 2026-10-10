// apkgen's resource compiler, checked by reading its output back with
// AnyAndroid's AssetManager2 counterpart (aa::res).

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "axml_writer.h"
#include "res_compiler.h"
#include "resources/asset_manager.h"
#include "resources/res_xml.h"
#include "test.h"
#include "xml_text.h"

using namespace aa;

namespace {

tool::XmlNode xml(const char* text) { return tool::parse_xml_text(text); }

tool::ResCompiler sample() {
  tool::ResCompiler c("com.test");
  c.add_values(xml(R"(<resources>
      <string name="app_name">  Demo
          app  </string>
      <string name="quote">"  kept  "\'s</string>
      <string name="greeting">Hello, %s!</string>
      <color name="accent">#80FF0000</color>
      <color name="alias">@color/accent</color>
      <dimen name="gap">1.5dp</dimen>
      <integer name="count">-7</integer>
      <bool name="flag">true</bool>
      <item type="id" name="extra"/>
    </resources>)"), "", "values/values.xml");
  c.add_values(xml(R"(<resources><string name="greeting">Bonjour, %s !</string></resources>)"), "fr",
               "values-fr/strings.xml");
  c.add_layout("main", "", xml(R"(<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
      android:orientation="vertical" android:layout_width="match_parent" android:layout_height="wrap_content">
    <TextView android:id="@+id/label" android:text="@string/greeting" android:textSize="18sp"
        android:textColor="#FF00FF00" android:layout_weight="0.5"/>
  </LinearLayout>)"));
  c.finalize();
  return c;
}

std::shared_ptr<res::AssetManager> load(const tool::ResCompiler& c, const char* locale) {
  auto apk = res::ApkAssets::load_table("test.arsc", c.write_arsc(), 0);
  auto am = std::make_shared<res::AssetManager>();
  am->set_apk_assets({apk});
  res::ResConfig config;
  config.set_bcp47_locale(locale);
  am->set_configurations({config});
  return am;
}

int32_t next_tag(res::XmlParser& p) {
  int32_t ev;
  do {
    ev = p.next();
  } while (ev == res::XmlParser::kStartNamespace || ev == res::XmlParser::kEndNamespace);
  return ev;
}

std::u16string string_of(const res::AssetManager& am, uint32_t id) {
  auto v = am.get_resource(id, false);
  if (!v || !am.resolve_reference(*v) || v->type != res::kTypeString) return u"<absent>";
  return am.string_pool(v->cookie)->string16(v->data).value_or(u"<absent>");
}

}  // namespace

TEST(res_compiler_assigns_deterministic_ids) {
  tool::ResCompiler c = sample();
  // Types in a fixed order (id, layout, string, color, dimen, integer, bool), names sorted.
  CHECK_EQ(c.resolve("@id/extra").value_or(0), 0x7f010000u);
  CHECK_EQ(c.resolve("@+id/label").value_or(0), 0x7f010001u);
  CHECK_EQ(c.resolve("@layout/main").value_or(0), 0x7f020000u);
  CHECK_EQ(c.resolve("@string/app_name").value_or(0), 0x7f030000u);
  CHECK_EQ(c.resolve("@com.test:string/quote").value_or(0), 0x7f030002u);
  CHECK_EQ(c.resolve("@bool/flag").value_or(0), 0x7f070000u);
  CHECK(!c.resolve("@string/missing"));
  CHECK(!c.resolve("@android:string/ok"));
  CHECK(c.r_txt().find("int color accent 0x7f040000\n") != std::string::npos);
}

TEST(res_compiler_table_reads_back) {
  tool::ResCompiler c = sample();
  auto am = load(c, "en-US");
  CHECK_EQ(string_of(*am, 0x7f030000), std::u16string(u"Demo app"));  // whitespace collapsed
  CHECK_EQ(string_of(*am, 0x7f030002), std::u16string(u"  kept  's"));  // quotes keep spaces
  CHECK_EQ(string_of(*am, 0x7f030001), std::u16string(u"Hello, %s!"));
  CHECK_EQ(string_of(*am, 0x7f020000), std::u16string(u"res/layout/main.xml"));

  auto color = am->get_resource(*c.resolve("@color/alias"), false);
  CHECK(color && am->resolve_reference(*color));
  CHECK_EQ(color->type, uint8_t(0x1c));
  CHECK_EQ(color->data, 0x80FF0000u);
  auto gap = am->get_resource(*c.resolve("@dimen/gap"), false);
  CHECK_EQ(gap->type, res::kTypeDimension);
  CHECK_EQ(gap->data, (uint32_t(1.5 * 32768) << 8) | (2u << 4) | 1u);  // 1.5 in radix 8p15, dp
  CHECK_EQ(int32_t(am->get_resource(*c.resolve("@integer/count"), false)->data), -7);
  CHECK_EQ(am->get_resource(*c.resolve("@bool/flag"), false)->data, 0xFFFFFFFFu);
  CHECK_EQ(am->resource_id("com.test:string/greeting", "", ""), 0x7f030001u);
}

TEST(res_compiler_selects_locale) {
  tool::ResCompiler c = sample();
  CHECK_EQ(string_of(*load(c, "fr-FR"), 0x7f030001), std::u16string(u"Bonjour, %s !"));
  CHECK_EQ(string_of(*load(c, "fr-CA"), 0x7f030001), std::u16string(u"Bonjour, %s !"));
  CHECK_EQ(string_of(*load(c, "de-DE"), 0x7f030001), std::u16string(u"Hello, %s!"));
}

TEST(res_compiler_layout_is_typed_like_aapt) {
  tool::ResCompiler c = sample();
  auto files = c.compile_files();
  CHECK_EQ(files.size(), size_t(1));
  CHECK_EQ(files[0].first, std::string("res/layout/main.xml"));
  auto tree = res::XmlTree::parse(files[0].second, 0x7f);
  res::XmlParser p(tree, 0x7f020000);
  CHECK_EQ(next_tag(p), res::XmlParser::kStartTag);
  res::Value orientation = p.attribute_value(p.index_of_resid(0x010100c4));
  CHECK_EQ(orientation.type, uint8_t(0x10));
  CHECK_EQ(orientation.data, 1u);
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x010100f4)).data, 0xFFFFFFFFu);  // match_parent
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x010100f5)).data, 0xFFFFFFFEu);  // wrap_content
  CHECK_EQ(next_tag(p), res::XmlParser::kStartTag);
  res::Value id = p.attribute_value(p.index_of_resid(0x010100d0));
  CHECK_EQ(id.type, res::kTypeReference);
  CHECK_EQ(id.data, 0x7f010001u);
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x0101014f)).data, 0x7f030001u);
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x01010095)).data, (18u << 8) | 2u);  // 18sp
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x01010098)).data, 0xFF00FF00u);
  CHECK_EQ(p.attribute_value(p.index_of_resid(0x01010181)).type, res::kTypeFloat);
}

TEST(res_compiler_rejects_bad_input) {
  tool::ResCompiler dup("com.test");
  dup.add_values(xml("<resources><string name=\"a\">x</string></resources>"), "", "v.xml");
  CHECK_THROWS(dup.add_values(xml("<resources><string name=\"a\">y</string></resources>"), "", "v.xml"),
               std::runtime_error);

  tool::ResCompiler no_default("com.test");
  no_default.add_values(xml("<resources><string name=\"a\">x</string></resources>"), "fr", "v.xml");
  CHECK_THROWS(no_default.finalize(), std::runtime_error);

  tool::ResCompiler bad_ref("com.test");
  bad_ref.add_values(xml("<resources><color name=\"a\">@color/b</color></resources>"), "", "v.xml");
  CHECK_THROWS(bad_ref.finalize(), std::runtime_error);

  tool::ResCompiler qualifiers("com.test");
  CHECK_THROWS(qualifiers.add_values(xml("<resources/>"), "land", "v.xml"), std::runtime_error);
  CHECK_THROWS(qualifiers.add_values(xml("<resources><string name=\"a b\">x</string></resources>"), "", "v.xml"),
               std::runtime_error);

  tool::ResCompiler layout("com.test");
  layout.add_layout("main", "", xml(R"(<TextView xmlns:android="http://schemas.android.com/apk/res/android"
      android:text="@string/missing"/>)"));
  layout.finalize();
  CHECK_THROWS(layout.compile_files(), std::runtime_error);
}

TEST(axml_literals) {
  CHECK_EQ(tool::parse_color("#abc")->second, 0xFFAABBCCu);
  CHECK_EQ(tool::parse_color("#8abc")->second, 0x88AABBCCu);
  CHECK(!tool::parse_color("#12345"));
  CHECK(!tool::parse_color("red"));
  CHECK_EQ(tool::parse_dimension("16dp")->second, (16u << 8) | 1u);
  CHECK_EQ(tool::parse_dimension("-2px")->second, ((uint32_t(-2) & 0xFFFFFFu) << 8) | 0u);
  CHECK_EQ(tool::parse_dimension("0.5mm")->second, (uint32_t(0.5 * 8388608) << 8) | (3u << 4) | 5u);
  CHECK(!tool::parse_dimension("12"));
  CHECK(!tool::parse_dimension("dp"));
}
