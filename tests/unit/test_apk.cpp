#include <string>

#include "apk-inspector/apk.h"
#include "apk-inspector/axml.h"
#include "apk-inspector/elf.h"
#include "apk-inspector/inflate.h"
#include "apk-inspector/manifest.h"
#include "apk-inspector/zip_reader.h"
#include "axml_writer.h"
#include "common/bytes.h"
#include "test.h"
#include "xml_text.h"
#include "zip_writer.h"

#include "deflate_vector.inc"

using namespace aa;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string dynamic_plaintext() {
  std::string s;
  for (int i = 0; i < 200; ++i) s += "Ligne " + std::to_string(i) + " : AnyAndroid exécute du bytecode Dalvik sur Windows. ";
  return s;
}

const char* kManifest = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- commentaire -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.example.test" android:versionCode="42" android:versionName="1.2 &amp; plus">
  <uses-sdk android:minSdkVersion="21" android:targetSdkVersion="34"/>
  <uses-permission android:name="android.permission.INTERNET"/>
  <uses-feature android:name="android.hardware.camera" android:required="false"/>
  <application android:label="Démo" android:debuggable="true">
    <activity android:name=".MainActivity" android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
    <activity android:name="Second"/>
    <service android:name="com.other.Svc"/>
  </application>
</manifest>)";

}  // namespace

TEST(inflate_dynamic_huffman_vector) {
  std::string expected = dynamic_plaintext();
  auto out = apk::inflate_raw(std::span(kDynamicDeflate, sizeof kDynamicDeflate), expected.size());
  CHECK(std::string(out.begin(), out.end()) == expected);
  // A wrong expected size is refused (bounds memory use on hostile input).
  CHECK_THROWS(apk::inflate_raw(std::span(kDynamicDeflate, sizeof kDynamicDeflate), expected.size() - 1), FormatError);
  CHECK_THROWS(apk::inflate_raw(std::span(kDynamicDeflate, 100), expected.size()), FormatError);
}

TEST(deflate_fixed_round_trip) {
  for (const std::string& s : {std::string(), std::string("a"), dynamic_plaintext(), std::string(100000, 'z')}) {
    auto in = bytes(s);
    auto packed = tool::deflate_fixed(in);
    auto out = apk::inflate_raw(packed, in.size());
    CHECK(out == in);
  }
}

TEST(zip_round_trip_and_crc) {
  tool::ZipWriter w;
  w.add("a.txt", bytes("bonjour"), false);
  w.add("dir/b.txt", bytes(dynamic_plaintext()), true);
  auto zip = apk::ZipArchive::open(w.finish());
  CHECK_EQ(zip->entries().size(), size_t(2));
  CHECK(zip->read("a.txt") == bytes("bonjour"));
  CHECK(zip->read("dir/b.txt") == bytes(dynamic_plaintext()));
  CHECK_EQ(zip->find("dir/b.txt")->method, uint16_t(8));
  CHECK(zip->find("absent") == nullptr);
}

TEST(zip_rejects_malformed) {
  CHECK_THROWS(apk::ZipArchive::open(bytes("pas une archive du tout, vraiment pas")), FormatError);
  {
    tool::ZipWriter w;
    tool::ZipWriter::Options o;
    o.corrupt_crc = true;
    w.add("x", bytes("data"), o);
    auto zip = apk::ZipArchive::open(w.finish());
    CHECK_THROWS(zip->read("x"), FormatError);
  }
  {
    tool::ZipWriter w;
    w.add("x", bytes("1"));
    w.add("x", bytes("2"));
    CHECK_THROWS(apk::ZipArchive::open(w.finish()), FormatError);
  }
  {
    tool::ZipWriter w;
    tool::ZipWriter::Options o;
    o.declared_size_override = 3ull << 30;
    w.add("bomb", std::vector<uint8_t>(1000, 0), o);
    CHECK_THROWS(apk::ZipArchive::open(w.finish()), FormatError);
  }
  {
    tool::ZipWriter w;
    w.add("ok", bytes("1"));
    auto data = w.finish();
    data.resize(data.size() - 5);  // truncated end of central directory
    CHECK_THROWS(apk::ZipArchive::open(data), FormatError);
  }
}

TEST(zip_flags_unsafe_names) {
  CHECK(apk::is_unsafe_entry_name("../evil"));
  CHECK(apk::is_unsafe_entry_name("a/../../evil"));
  CHECK(apk::is_unsafe_entry_name("/abs"));
  CHECK(apk::is_unsafe_entry_name("C:/x"));
  CHECK(apk::is_unsafe_entry_name("a\\b"));
  CHECK(!apk::is_unsafe_entry_name("res/layout/main.xml"));
  CHECK(!apk::is_unsafe_entry_name("a..b/c"));
  tool::ZipWriter w;
  w.add("../../evil.txt", bytes("x"));
  auto zip = apk::ZipArchive::open(w.finish());
  CHECK(zip->entries()[0].unsafe_name);
  CHECK(!zip->warnings().empty());
}

TEST(axml_round_trip_utf16_and_utf8) {
  for (bool utf8 : {false, true}) {
    auto bin = tool::compile_axml(tool::parse_xml_text(kManifest), {utf8});
    auto doc = apk::parse_axml(bin);
    CHECK_EQ(doc.root->name, std::string("manifest"));
    apk::Manifest m = apk::parse_manifest(doc);
    CHECK_EQ(m.package, std::string("com.example.test"));
    CHECK_EQ(m.version_code, int64_t(42));
    CHECK_EQ(m.version_name, std::string("1.2 & plus"));
    CHECK_EQ(m.min_sdk, 21);
    CHECK_EQ(m.target_sdk, 34);
    CHECK_EQ(m.app_label, std::string("Démo"));
    CHECK(m.debuggable);
    CHECK_EQ(m.permissions.size(), size_t(1));
    CHECK_EQ(m.features.size(), size_t(1));
    CHECK(!m.features[0].required);
    CHECK_EQ(m.activities.size(), size_t(2));
    CHECK_EQ(m.activities[1].name, std::string("com.example.test.Second"));
    CHECK_EQ(m.services.size(), size_t(1));
    CHECK(m.launcher_activity().has_value());
    CHECK_EQ(*m.launcher_activity(), std::string("com.example.test.MainActivity"));
    CHECK(m.activities[0].exported.value_or(false));
  }
}

TEST(axml_rejects_garbage) {
  CHECK_THROWS(apk::parse_axml(bytes("garbage!")), FormatError);
  auto bin = tool::compile_axml(tool::parse_xml_text(kManifest));
  // Truncations anywhere must fail cleanly, never crash.
  for (size_t n = 0; n < bin.size(); n += 7) {
    std::vector<uint8_t> cut(bin.begin(), bin.begin() + long(n));
    try {
      apk::parse_axml(cut);
    } catch (const FormatError&) {
    }
  }
}

TEST(elf_header_inspection) {
  std::vector<uint8_t> elf(64, 0);
  elf[0] = 0x7f, elf[1] = 'E', elf[2] = 'L', elf[3] = 'F', elf[4] = 2, elf[5] = 1;
  elf[16] = 3;
  elf[18] = 183;
  auto info = apk::inspect_elf(elf);
  CHECK(info.valid);
  CHECK_EQ(info.machine, std::string("aarch64"));
  CHECK_EQ(info.bits, 64);
  CHECK_EQ(apk::abi_machine("arm64-v8a"), std::string("aarch64"));
  CHECK(!apk::inspect_elf(bytes("MZ not elf at all")).valid);
}
