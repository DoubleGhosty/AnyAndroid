#include "framework-bridge/internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "common/hash.h"
#include "common/text.h"
#include "dex-engine/runtime.h"

namespace aa::bridge {

namespace {

using rt::ApiStatus;
using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = ApiStatus;

enum class MdType : int64_t {
  kNone = 0,
  kSha1 = 1,
  kSha224 = 2,
  kSha256 = 3,
  kSha384 = 4,
  kSha512 = 5,
  kMd5 = 6,
};

struct EvpMdCtx {
  MdType type = MdType::kNone;
  std::vector<uint8_t> buffer;
};

int64_t md_type_from_name(std::string name) {
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (name == "sha1" || name == "sha-1") return int64_t(MdType::kSha1);
  if (name == "sha224" || name == "sha-224") return int64_t(MdType::kSha224);
  if (name == "sha256" || name == "sha-256") return int64_t(MdType::kSha256);
  if (name == "sha384" || name == "sha-384") return int64_t(MdType::kSha384);
  if (name == "sha512" || name == "sha-512") return int64_t(MdType::kSha512);
  if (name == "md5") return int64_t(MdType::kMd5);
  return int64_t(MdType::kNone);
}

int32_t md_size(int64_t type) {
  switch (static_cast<MdType>(type)) {
    case MdType::kSha1: return 20;
    case MdType::kSha224: return 28;
    case MdType::kSha256: return 32;
    case MdType::kSha384: return 48;
    case MdType::kSha512: return 64;
    case MdType::kMd5: return 16;
    default: return 0;
  }
}

// --- SHA-512 & SHA-384 implementation ---
constexpr uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

std::vector<uint8_t> sha512_calc(std::span<const uint8_t> data, bool is_384 = false) {
  uint64_t h[8];
  if (!is_384) {
    h[0] = 0x6a09e667f3bcc908ull; h[1] = 0xbb67ae8584caa73bull;
    h[2] = 0x3c6ef372fe94f82bull; h[3] = 0xa54ff53a5f1d36f1ull;
    h[4] = 0x510e527fade682d1ull; h[5] = 0x9b05688c2b3e6c1full;
    h[6] = 0x1f83d9abfb41bd6bull; h[7] = 0x5be0cd19137e2179ull;
  } else {
    h[0] = 0xcbbb9d5dc1059ed8ull; h[1] = 0x629a292a367cd507ull;
    h[2] = 0x9159015a3070dd17ull; h[3] = 0x152fecd8f70e5939ull;
    h[4] = 0x67332667ffc00b31ull; h[5] = 0x8eb44a8768581511ull;
    h[6] = 0xdb0c2e0d64f98fa7ull; h[7] = 0x47b5481dbefa4fa4ull;
  }

  std::vector<uint8_t> m(data.begin(), data.end());
  uint64_t bit_len = uint64_t(data.size()) * 8;
  m.push_back(0x80);
  while ((m.size() % 128) != 112) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bit_len >> (8 * i)));

  auto load_be64 = [](const uint8_t* p) -> uint64_t {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
  };

  for (size_t off = 0; off < m.size(); off += 128) {
    uint64_t w[80];
    for (int t = 0; t < 16; ++t) w[t] = load_be64(&m[off + 8 * t]);
    for (int t = 16; t < 80; ++t) {
      uint64_t s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
      uint64_t s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; ++t) {
      uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
      uint64_t ch = (e & f) ^ (~e & g);
      uint64_t t1 = hh + S1 + ch + K512[t] + w[t];
      uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
      uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint64_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  size_t out_words = is_384 ? 6 : 8;
  std::vector<uint8_t> out(out_words * 8);
  for (size_t i = 0; i < out_words; ++i) {
    for (int j = 0; j < 8; ++j) out[8 * i + j] = uint8_t(h[i] >> (56 - 8 * j));
  }
  return out;
}

// --- MD5 implementation ---
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::vector<uint8_t> md5_calc(std::span<const uint8_t> data) {
  static const uint32_t s[64] = {
      7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
      5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
      4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
      6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
  };
  static const uint32_t k[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
      0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
      0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
      0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
      0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
      0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2d7d25c5, 0xeb86d391
  };

  uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

  std::vector<uint8_t> m(data.begin(), data.end());
  uint64_t bit_len = uint64_t(data.size()) * 8;
  m.push_back(0x80);
  while ((m.size() % 64) != 56) m.push_back(0);
  for (int i = 0; i < 8; ++i) m.push_back(uint8_t(bit_len >> (8 * i)));

  auto load_le32 = [](const uint8_t* p) -> uint32_t {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  };

  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t m_words[16];
    for (int i = 0; i < 16; ++i) m_words[i] = load_le32(&m[off + 4 * i]);
    uint32_t A = a0, B = b0, C = c0, D = d0;
    for (int i = 0; i < 64; ++i) {
      uint32_t F, g;
      if (i < 16) {
        F = (B & C) | (~B & D);
        g = i;
      } else if (i < 32) {
        F = (D & B) | (~D & C);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        F = B ^ C ^ D;
        g = (3 * i + 5) % 16;
      } else {
        F = C ^ (B | ~D);
        g = (7 * i) % 16;
      }
      uint32_t dTemp = D;
      D = C;
      C = B;
      B = B + rotl32(A + F + k[i] + m_words[g], s[i]);
      A = dTemp;
    }
    a0 += A; b0 += B; c0 += C; d0 += D;
  }
  std::vector<uint8_t> out(16);
  auto store_le32 = [](uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
  };
  store_le32(&out[0], a0);
  store_le32(&out[4], b0);
  store_le32(&out[8], c0);
  store_le32(&out[12], d0);
  return out;
}

std::vector<uint8_t> compute_digest(MdType type, std::span<const uint8_t> data) {
  switch (type) {
    case MdType::kSha1: {
      auto arr = aa::hash::sha1(data);
      return std::vector<uint8_t>(arr.begin(), arr.end());
    }
    case MdType::kSha256: {
      auto arr = aa::hash::sha256(data);
      return std::vector<uint8_t>(arr.begin(), arr.end());
    }
    case MdType::kSha224: {
      auto arr = aa::hash::sha256(data);
      return std::vector<uint8_t>(arr.begin(), arr.begin() + 28);
    }
    case MdType::kSha384: return sha512_calc(data, true);
    case MdType::kSha512: return sha512_calc(data, false);
    case MdType::kMd5: return md5_calc(data);
    default: return {};
  }
}

EvpMdCtx* get_ctx(Runtime& rt, Handle h) {
  if (h == kNull) return nullptr;
  rt::Object& o = rt.nonnull(h, "NativeRef");
  rt::Field* f = find_field(o.klass, "address", "J");
  if (!f) return nullptr;
  uint64_t low = o.fields.at(f->slot);
  uint64_t high = o.fields.at(f->slot + 1);
  return reinterpret_cast<EvpMdCtx*>((high << 32) | low);
}

}  // namespace

void register_libcore_crypto(rt::NativeRegistry& r) {
  const char* nc = "Lcom/android/org/conscrypt/NativeCrypto;";

  r.bind(nc, "EVP_get_digestbyname", "(Ljava/lang/String;)J",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           Handle name_h = a.h(0);
           if (name_h == kNull) return 0;
           std::string name = rt.string_utf8(name_h);
           return rt::ret_long(md_type_from_name(name));
         });

  r.bind(nc, "EVP_MD_size", "(J)I",
         [](Runtime&, NativeArgs& a) -> uint64_t {
           return rt::ret_int(md_size(a.i64(0)));
         });

  r.bind(nc, "EVP_MD_CTX_create", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t {
           auto* ctx = new EvpMdCtx();
           return rt::ret_long(reinterpret_cast<int64_t>(ctx));
         });

  r.bind(nc, "EVP_MD_CTX_destroy", "(J)V",
         [](Runtime&, NativeArgs& a) -> uint64_t {
           int64_t addr = a.i64(0);
           if (addr != 0) {
             delete reinterpret_cast<EvpMdCtx*>(addr);
           }
           return 0;
         });

  r.bind(nc, "EVP_MD_CTX_cleanup", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (auto* ctx = get_ctx(rt, a.h(0))) {
             ctx->buffer.clear();
             ctx->type = MdType::kNone;
           }
           return 0;
         });

  r.bind(nc, "EVP_MD_CTX_copy_ex",
         "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto* dst = get_ctx(rt, a.h(0));
           auto* src = get_ctx(rt, a.h(1));
           if (dst && src) {
             *dst = *src;
             return rt::ret_int(1);
           }
           return rt::ret_int(0);
         });

  r.bind(nc, "EVP_DigestInit_ex", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;J)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (auto* ctx = get_ctx(rt, a.h(0))) {
             ctx->type = static_cast<MdType>(a.i64(1));
             ctx->buffer.clear();
             return rt::ret_int(1);
           }
           return rt::ret_int(0);
         });

  r.bind(nc, "EVP_DigestUpdate", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BII)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (auto* ctx = get_ctx(rt, a.h(0))) {
             Handle arr_h = a.h(1);
             int32_t off = a.i32(2);
             int32_t len = a.i32(3);
             if (arr_h != kNull && len > 0) {
               auto& arr = rt.array(arr_h, "[B");
               if (off >= 0 && off + len <= int32_t(arr.data.size())) {
                 ctx->buffer.insert(ctx->buffer.end(), arr.data.begin() + off, arr.data.begin() + off + len);
               }
             }
           }
           return 0;
         });

  r.bind(nc, "EVP_DigestUpdateDirect", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;JI)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (auto* ctx = get_ctx(rt, a.h(0))) {
             int64_t addr = a.i64(1);
             int32_t len = a.i32(3);
             if (addr != 0 && len > 0) {
               const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
               ctx->buffer.insert(ctx->buffer.end(), p, p + len);
             }
           }
           return 0;
         });

  r.bind(nc, "EVP_DigestFinal_ex", "(Lcom/android/org/conscrypt/NativeRef$EVP_MD_CTX;[BI)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (auto* ctx = get_ctx(rt, a.h(0))) {
             Handle out_h = a.h(1);
             int32_t out_off = a.i32(2);
             std::vector<uint8_t> digest = compute_digest(ctx->type, ctx->buffer);
             if (out_h != kNull) {
               auto& arr = rt.array(out_h, "[B");
               if (out_off >= 0 && out_off + int32_t(digest.size()) <= int32_t(arr.data.size())) {
                 std::memcpy(arr.data.data() + out_off, digest.data(), digest.size());
               }
             }
             ctx->buffer.clear();
             return rt::ret_int(int32_t(digest.size()));
           }
           return rt::ret_int(0);
         });
}

}  // namespace aa::bridge
