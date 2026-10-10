#include <string>

#include "common/bytes.h"
#include "common/hash.h"
#include "common/json.h"
#include "common/text.h"
#include "test.h"

using namespace aa;

namespace {
std::span<const uint8_t> as_bytes(const std::string& s) {
  return std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
}  // namespace

TEST(hash_known_vectors) {
  CHECK_EQ(hash::to_hex(hash::sha256(as_bytes(""))),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(hash::to_hex(hash::sha256(as_bytes("abc"))),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(hash::to_hex(hash::sha1(as_bytes("abc"))), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
  CHECK_EQ(hash::crc32(as_bytes("123456789")), 0xCBF43926u);
  CHECK_EQ(hash::adler32(as_bytes("Wikipedia")), 0x11E60398u);
  // Multi-block message (> 64 bytes) exercises the block loop.
  std::string long_msg(200, 'a');
  CHECK_EQ(hash::to_hex(hash::sha1(as_bytes(long_msg))).size(), size_t(40));
}

TEST(leb128_round_trip) {
  for (int32_t v : {0, 1, -1, 63, 64, -64, -65, 127, 128, 300, -300, 0x7fffffff, int32_t(0x80000000)}) {
    ByteWriter w;
    w.sleb128(v);
    w.uleb128(uint32_t(v));
    ByteReader r(w.buffer());
    CHECK_EQ(r.sleb128(), v);
    CHECK_EQ(r.uleb128(), uint32_t(v));
    CHECK(r.at_end());
  }
}

TEST(byte_reader_bounds) {
  std::vector<uint8_t> data = {1, 2, 3};
  ByteReader r(data);
  CHECK_EQ(r.u16(), uint16_t(0x0201));
  CHECK_THROWS(r.u16(), FormatError);
  CHECK_THROWS(r.sub(2, 5), FormatError);
  CHECK_THROWS(r.u32_at(SIZE_MAX - 1), FormatError);
}

TEST(utf_conversions) {
  std::string s = "Bonjour é 😀";
  CHECK(text::utf16_to_utf8(text::utf8_to_utf16(s)) == s);
  CHECK_EQ(text::utf8_to_utf16("😀").size(), size_t(2));
  // Lone surrogate survives a round trip (WTF-8).
  std::u16string lone = {u'a', char16_t(0xD800), u'b'};
  CHECK(text::utf8_to_utf16(text::utf16_to_utf8(lone)) == lone);
  // MUTF-8: NUL encoded on two bytes.
  std::vector<uint8_t> m = {'a', 0xC0, 0x80, 'b', 0};
  CHECK(text::mutf8_to_utf16(m) == (std::u16string{u'a', u'\0', u'b'}));
  CHECK_EQ(text::descriptor_to_java("[[Ljava/lang/String;"), std::string("java.lang.String[][]"));
  CHECK_EQ(text::java_to_descriptor("com.example.A"), std::string("Lcom/example/A;"));
}

TEST(json_writer_escapes) {
  JsonWriter w(false);
  w.begin_object().field("a", "x\"y\n").field("n", 3).key("l").begin_array().value(true).null().end_array().end_object();
  CHECK_EQ(w.str(), std::string("{\"a\":\"x\\\"y\\n\",\"n\":3,\"l\":[true,null]}"));
}
