#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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

namespace {
std::string hex_digest(hash::Algorithm alg, const std::string& msg) {
  return hash::to_hex(hash::digest(alg, as_bytes(msg)));
}
}  // namespace

// RFC 1321, appendix A.5 (every MD5 step uses each K[i], so any wrong constant shows).
TEST(hash_md5_rfc1321_suite) {
  using A = hash::Algorithm;
  CHECK_EQ(hex_digest(A::kMd5, ""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
  CHECK_EQ(hex_digest(A::kMd5, "a"), std::string("0cc175b9c0f1b6a831c399e269772661"));
  CHECK_EQ(hex_digest(A::kMd5, "abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
  CHECK_EQ(hex_digest(A::kMd5, "message digest"), std::string("f96b697d7cb7938d525a2f31aaf161d0"));
  CHECK_EQ(hex_digest(A::kMd5, "abcdefghijklmnopqrstuvwxyz"), std::string("c3fcd3d76192e4007dfb496cca67e13b"));
  CHECK_EQ(hex_digest(A::kMd5, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
           std::string("d174ab98d277d9f5a5611c2c9f419d9f"));
  std::string digits;
  for (int i = 0; i < 8; ++i) digits += "1234567890";
  CHECK_EQ(hex_digest(A::kMd5, digits), std::string("57edf4a22be3c955ac49da2e2107b67a"));
}

// FIPS 180-4 examples (NIST CSRC): empty message, "abc", and the 448- and
// 896-bit messages, whose padding spills into an extra block.
TEST(hash_sha_fips180_examples) {
  using A = hash::Algorithm;
  const std::string m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const std::string m896 =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  struct Vector {
    A alg;
    const char* empty;
    const char* abc;
    const char* m448;
    const char* m896;
  };
  const Vector vectors[] = {
      {A::kSha1, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "a9993e364706816aba3e25717850c26c9cd0d89d",
       "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "a49b2446a02c645bf419f995b67091253a04a259"},
      {A::kSha224, "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f",
       "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
       "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525",
       "c97ca9a559850ce97a04a96def6d99a9e0e0e2ab14e6b8df265fc0b3"},
      {A::kSha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
      {A::kSha384, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
       "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
       "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b",
       "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039"},
      {A::kSha512,
       "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
       "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
       "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
       "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445",
       "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
       "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"},
  };
  for (const Vector& v : vectors) {
    CHECK_EQ(hex_digest(v.alg, ""), std::string(v.empty));
    CHECK_EQ(hex_digest(v.alg, "abc"), std::string(v.abc));
    CHECK_EQ(hex_digest(v.alg, m448), std::string(v.m448));
    CHECK_EQ(hex_digest(v.alg, m896), std::string(v.m896));
    CHECK_EQ(hash::Digest(v.alg).size() * 2, std::strlen(v.abc));
  }
  // The historical one-shot helpers agree with Digest.
  CHECK_EQ(hash::to_hex(hash::sha1(as_bytes(m448))), std::string(vectors[0].m448));
  CHECK_EQ(hash::to_hex(hash::sha256(as_bytes(m896))), std::string(vectors[2].m896));
}

// One million 'a' (FIPS 180-4 examples), fed in uneven chunks: the state stays
// one block whatever the message length.
TEST(hash_million_a_streamed) {
  using A = hash::Algorithm;
  const std::string chunk(997, 'a');  // prime length: chunks straddle block boundaries
  auto million = [&](A alg) {
    hash::Digest d(alg);
    size_t left = 1000000;
    while (left > 0) {
      size_t n = std::min(left, chunk.size());
      d.update(as_bytes(chunk).first(n));
      left -= n;
    }
    return hash::to_hex(d.finish());
  };
  CHECK_EQ(million(A::kSha1), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
  CHECK_EQ(million(A::kSha256), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  CHECK_EQ(million(A::kSha512),
           std::string("e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                       "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"));
  CHECK_EQ(million(A::kMd5), std::string("7707d6ae4e027c70eea2a935c2296f21"));
}

// Any split of the message gives the one-shot digest; a copied Digest carries
// on independently; finish() starts a new message.
TEST(hash_streaming_equals_one_shot) {
  using A = hash::Algorithm;
  std::string msg;
  for (int i = 0; i < 1000; ++i) msg.push_back(char((i * 131 + 7) & 0xff));
  const auto bytes = as_bytes(msg);
  for (A alg : {A::kMd5, A::kSha1, A::kSha224, A::kSha256, A::kSha384, A::kSha512}) {
    // Lengths around the padding and block boundaries of 64- and 128-byte blocks.
    for (size_t len : {0, 1, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000}) {
      const std::vector<uint8_t> expected = hash::digest(alg, bytes.first(len));
      for (size_t step : {1, 3, 63, 64, 65, 127, 128, 129, 500}) {
        hash::Digest d(alg);
        for (size_t at = 0; at < len; at += step) d.update(bytes.subspan(at, std::min(step, len - at)));
        CHECK(d.finish() == expected);
      }
    }
    hash::Digest prefix(alg);
    prefix.update(bytes.first(300));
    hash::Digest copy = prefix;
    prefix.update(bytes.subspan(300));
    CHECK(prefix.finish() == hash::digest(alg, bytes));
    copy.update(bytes.subspan(300, 200));
    CHECK(copy.finish() == hash::digest(alg, bytes.first(500)));
    // After finish(), the same object hashes a fresh message.
    copy.update(as_bytes("abc"));
    CHECK(copy.finish() == hash::digest(alg, as_bytes("abc")));
  }
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
