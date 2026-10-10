#include "apk-inspector/inflate.h"

#include <string>

#include "common/bytes.h"

// Straightforward canonical-Huffman inflater written for clarity and strict
// validation rather than speed. Structure follows RFC 1951 section 3.2.
namespace aa::apk {

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLitLen = 288;
constexpr int kMaxDist = 30;

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Huffman {
  uint16_t count[kMaxBits + 1] = {};
  uint16_t symbol[kMaxLitLen] = {};
};

// Builds a canonical code from code lengths. Returns 0 for a complete code,
// > 0 for an incomplete one and < 0 for an over-subscribed one.
int build(Huffman& h, const uint8_t* lengths, int n) {
  for (auto& c : h.count) c = 0;
  for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
  if (h.count[0] == n) return 0;  // no codes: complete but useless
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return left;
  }
  uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
  for (int s = 0; s < n; ++s) {
    if (lengths[s] != 0) h.symbol[offs[lengths[s]]++] = uint16_t(s);
  }
  return left;
}

class Inflater {
 public:
  Inflater(std::span<const uint8_t> in, size_t expected) : in_(in), expected_(expected) { out_.reserve(expected); }

  std::vector<uint8_t> run() {
    bool last = false;
    while (!last) {
      last = bits(1) != 0;
      switch (bits(2)) {
        case 0: stored(); break;
        case 1: fixed(); break;
        case 2: dynamic(); break;
        default: fail("type de bloc invalide");
      }
    }
    if (out_.size() != expected_) {
      fail("taille décompressée " + std::to_string(out_.size()) + " au lieu de " + std::to_string(expected_));
    }
    return std::move(out_);
  }

 private:
  [[noreturn]] void fail(const std::string& what) { throw_format("deflate : " + what); }

  uint32_t bits(int need) {
    uint64_t val = bitbuf_;
    while (bitcnt_ < need) {
      if (pos_ >= in_.size()) fail("flux tronqué");
      val |= uint64_t(in_[pos_++]) << bitcnt_;
      bitcnt_ += 8;
    }
    bitbuf_ = uint32_t(val >> need);
    bitcnt_ -= need;
    return uint32_t(val & ((uint64_t(1) << need) - 1));
  }

  void emit(uint8_t b) {
    if (out_.size() >= expected_) fail("données plus longues que la taille annoncée");
    out_.push_back(b);
  }

  void stored() {
    bitbuf_ = 0;
    bitcnt_ = 0;  // discard remaining bits of the current byte
    if (in_.size() - pos_ < 4) fail("en-tête de bloc stocké tronqué");
    uint16_t len = uint16_t(in_[pos_] | (in_[pos_ + 1] << 8));
    uint16_t nlen = uint16_t(in_[pos_ + 2] | (in_[pos_ + 3] << 8));
    pos_ += 4;
    if (len != uint16_t(~nlen)) fail("longueur de bloc stocké incohérente");
    if (in_.size() - pos_ < len) fail("bloc stocké tronqué");
    if (expected_ - out_.size() < len) fail("données plus longues que la taille annoncée");
    out_.insert(out_.end(), in_.begin() + pos_, in_.begin() + pos_ + len);
    pos_ += len;
  }

  int decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
      code |= int(bits(1));
      int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    fail("code de Huffman invalide");
  }

  void codes(const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
      int sym = decode(lencode);
      if (sym < 256) {
        emit(uint8_t(sym));
      } else if (sym == 256) {
        return;
      } else {
        sym -= 257;
        if (sym >= 29) fail("symbole de longueur invalide");
        size_t len = kLenBase[sym] + bits(kLenExtra[sym]);
        int dsym = decode(distcode);
        if (dsym >= 30) fail("symbole de distance invalide");
        size_t dist = kDistBase[dsym] + bits(kDistExtra[dsym]);
        if (dist > out_.size()) fail("distance hors de la fenêtre");
        if (expected_ - out_.size() < len) fail("données plus longues que la taille annoncée");
        size_t from = out_.size() - dist;
        for (size_t i = 0; i < len; ++i) out_.push_back(out_[from + i]);
      }
    }
  }

  void fixed() {
    static const auto tables = [] {
      std::pair<Huffman, Huffman> t;
      uint8_t lengths[kMaxLitLen];
      int s = 0;
      for (; s < 144; ++s) lengths[s] = 8;
      for (; s < 256; ++s) lengths[s] = 9;
      for (; s < 280; ++s) lengths[s] = 7;
      for (; s < kMaxLitLen; ++s) lengths[s] = 8;
      build(t.first, lengths, kMaxLitLen);
      for (s = 0; s < kMaxDist; ++s) lengths[s] = 5;
      build(t.second, lengths, kMaxDist);
      return t;
    }();
    codes(tables.first, tables.second);
  }

  void dynamic() {
    int nlen = int(bits(5)) + 257;
    int ndist = int(bits(5)) + 1;
    int ncode = int(bits(4)) + 4;
    if (nlen > 286 || ndist > 30) fail("nombre de codes invalide");

    uint8_t lengths[kMaxLitLen + kMaxDist] = {};
    for (int i = 0; i < ncode; ++i) lengths[kCodeLenOrder[i]] = uint8_t(bits(3));
    Huffman lencode, distcode;
    if (build(lencode, lengths, 19) != 0) fail("code des longueurs incomplet");

    int index = 0;
    while (index < nlen + ndist) {
      int sym = decode(lencode);
      if (sym < 16) {
        lengths[index++] = uint8_t(sym);
        continue;
      }
      uint8_t len = 0;
      int repeat;
      if (sym == 16) {
        if (index == 0) fail("répétition sans longueur précédente");
        len = lengths[index - 1];
        repeat = 3 + int(bits(2));
      } else if (sym == 17) {
        repeat = 3 + int(bits(3));
      } else {
        repeat = 11 + int(bits(7));
      }
      if (index + repeat > nlen + ndist) fail("trop de longueurs");
      while (repeat--) lengths[index++] = len;
    }
    if (lengths[256] == 0) fail("pas de code de fin de bloc");

    // Incomplete codes are only allowed when they contain a single code.
    int err = build(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) fail("code littéral/longueur invalide");
    err = build(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) fail("code de distance invalide");
    codes(lencode, distcode);
  }

  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  uint32_t bitbuf_ = 0;
  int bitcnt_ = 0;
  size_t expected_;
  std::vector<uint8_t> out_;
};

}  // namespace

std::vector<uint8_t> inflate_raw(std::span<const uint8_t> input, size_t expected_size) {
  return Inflater(input, expected_size).run();
}

}  // namespace aa::apk
