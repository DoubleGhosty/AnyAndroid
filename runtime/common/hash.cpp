#include "common/hash.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace aa::hash {

namespace {

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

uint32_t load_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint32_t load_le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t load_be64(const uint8_t* p) { return (uint64_t(load_be32(p)) << 32) | load_be32(p + 4); }
void store_be32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void store_le32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void store_be64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}
void store_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}

// RFC 1321: K[i] = floor(|sin(i + 1)| * 2^32), and the per-step rotations.
constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr uint8_t kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

// FIPS 180-4 4.2.2 and 4.2.3: fractional parts of the cube roots of the first primes.
constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr uint64_t kSha512K[80] = {
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
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

// FIPS 180-4 5.3: initial hash values.
constexpr uint32_t kMd5Iv[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
constexpr uint32_t kSha1Iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
constexpr uint32_t kSha224Iv[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
                                   0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
constexpr uint32_t kSha256Iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
constexpr uint64_t kSha384Iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
                                   0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
                                   0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
constexpr uint64_t kSha512Iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                                   0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                   0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};

void md5_compress(uint32_t h[4], const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = load_le32(p + 4 * i);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + rotl(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
}

void sha1_compress(uint32_t h[5], const uint8_t* p) {
  uint32_t w[80];
  for (int t = 0; t < 16; ++t) w[t] = load_be32(p + 4 * t);
  for (int t = 16; t < 80; ++t) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int t = 0; t < 80; ++t) {
    uint32_t f, k;
    if (t < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (t < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (t < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = rotl(a, 5) + f + e + k + w[t];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

void sha256_compress(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int t = 0; t < 16; ++t) w[t] = load_be32(p + 4 * t);
  for (int t = 16; t < 64; ++t) {
    uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int t = 0; t < 64; ++t) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + kSha256K[t] + w[t];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

void sha512_compress(uint64_t h[8], const uint8_t* p) {
  uint64_t w[80];
  for (int t = 0; t < 16; ++t) w[t] = load_be64(p + 8 * t);
  for (int t = 16; t < 80; ++t) {
    uint64_t s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
    uint64_t s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int t = 0; t < 80; ++t) {
    uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = hh + S1 + ch + kSha512K[t] + w[t];
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
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

uint32_t adler32(std::span<const uint8_t> data) {
  constexpr uint32_t kMod = 65521;
  uint32_t a = 1, b = 0;
  size_t i = 0;
  while (i < data.size()) {
    // 5552 is the largest block for which b cannot overflow 32 bits.
    size_t n = std::min<size_t>(5552, data.size() - i);
    for (size_t k = 0; k < n; ++k) {
      a += data[i + k];
      b += a;
    }
    a %= kMod;
    b %= kMod;
    i += n;
  }
  return (b << 16) | a;
}

// --- Digest ----------------------------------------------------------------------------

Digest::Digest(Algorithm algorithm) : algorithm_(algorithm) { reset(); }

size_t Digest::size() const {
  switch (algorithm_) {
    case Algorithm::kMd5: return 16;
    case Algorithm::kSha1: return 20;
    case Algorithm::kSha224: return 28;
    case Algorithm::kSha256: return 32;
    case Algorithm::kSha384: return 48;
    case Algorithm::kSha512: return 64;
  }
  return 0;
}

size_t Digest::block_size() const {
  return algorithm_ == Algorithm::kSha384 || algorithm_ == Algorithm::kSha512 ? 128 : 64;
}

void Digest::reset() {
  std::memset(h32_, 0, sizeof(h32_));
  std::memset(h64_, 0, sizeof(h64_));
  std::memset(block_, 0, sizeof(block_));
  used_ = 0;
  total_ = 0;
  switch (algorithm_) {
    case Algorithm::kMd5: std::memcpy(h32_, kMd5Iv, sizeof(kMd5Iv)); break;
    case Algorithm::kSha1: std::memcpy(h32_, kSha1Iv, sizeof(kSha1Iv)); break;
    case Algorithm::kSha224: std::memcpy(h32_, kSha224Iv, sizeof(kSha224Iv)); break;
    case Algorithm::kSha256: std::memcpy(h32_, kSha256Iv, sizeof(kSha256Iv)); break;
    case Algorithm::kSha384: std::memcpy(h64_, kSha384Iv, sizeof(kSha384Iv)); break;
    case Algorithm::kSha512: std::memcpy(h64_, kSha512Iv, sizeof(kSha512Iv)); break;
  }
}

void Digest::compress(const uint8_t* block) {
  switch (algorithm_) {
    case Algorithm::kMd5: md5_compress(h32_, block); break;
    case Algorithm::kSha1: sha1_compress(h32_, block); break;
    case Algorithm::kSha224:
    case Algorithm::kSha256: sha256_compress(h32_, block); break;
    case Algorithm::kSha384:
    case Algorithm::kSha512: sha512_compress(h64_, block); break;
  }
}

void Digest::update(std::span<const uint8_t> data) {
  if (data.empty()) return;
  const size_t bs = block_size();
  const uint8_t* p = data.data();
  size_t n = data.size();
  total_ += n;
  if (used_ > 0) {
    size_t take = std::min(n, bs - used_);
    std::memcpy(block_ + used_, p, take);
    used_ += take;
    p += take;
    n -= take;
    if (used_ < bs) return;
    compress(block_);
    used_ = 0;
  }
  for (; n >= bs; p += bs, n -= bs) compress(p);
  if (n > 0) {
    std::memcpy(block_, p, n);
    used_ = n;
  }
}

void Digest::finish(std::span<uint8_t> out) {
  if (out.size() < size()) throw std::length_error("hash::Digest::finish : sortie trop courte");
  // Padding: 0x80, zeros, then the message length in bits (64 bits; 128 for
  // SHA-384/512), little-endian for MD5 and big-endian for SHA.
  const size_t bs = block_size();
  const size_t length_bytes = bs == 128 ? 16 : 8;
  const uint64_t bits = total_ << 3;
  block_[used_++] = 0x80;
  if (used_ > bs - length_bytes) {
    std::memset(block_ + used_, 0, bs - used_);
    compress(block_);
    used_ = 0;
  }
  std::memset(block_ + used_, 0, bs - used_);
  if (algorithm_ == Algorithm::kMd5) {
    store_le64(block_ + bs - 8, bits);
  } else {
    store_be64(block_ + bs - 8, bits);
    if (bs == 128) store_be64(block_ + bs - 16, total_ >> 61);
  }
  compress(block_);

  uint8_t* o = out.data();
  switch (algorithm_) {
    case Algorithm::kMd5:
      for (int i = 0; i < 4; ++i) store_le32(o + 4 * i, h32_[i]);
      break;
    case Algorithm::kSha1:
    case Algorithm::kSha224:
    case Algorithm::kSha256:
      for (size_t i = 0; i < size() / 4; ++i) store_be32(o + 4 * i, h32_[i]);
      break;
    case Algorithm::kSha384:
    case Algorithm::kSha512:
      for (size_t i = 0; i < size() / 8; ++i) store_be64(o + 8 * i, h64_[i]);
      break;
  }
  reset();
}

std::vector<uint8_t> Digest::finish() {
  std::vector<uint8_t> out(size());
  finish(out);
  return out;
}

std::vector<uint8_t> digest(Algorithm algorithm, std::span<const uint8_t> data) {
  Digest d(algorithm);
  d.update(data);
  return d.finish();
}

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
  Digest d(Algorithm::kSha1);
  d.update(data);
  std::array<uint8_t, 20> out{};
  d.finish(out);
  return out;
}

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
  Digest d(Algorithm::kSha256);
  d.update(data);
  std::array<uint8_t, 32> out{};
  d.finish(out);
  return out;
}

std::string to_hex(std::span<const uint8_t> data) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (uint8_t b : data) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}

}  // namespace aa::hash
