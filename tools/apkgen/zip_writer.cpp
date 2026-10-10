#include "zip_writer.h"

#include <stdexcept>

#include "common/bytes.h"
#include "common/hash.h"

namespace aa::tool {

namespace {

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
 public:
  void put(uint32_t bits, int n) {
    buf_ |= uint64_t(bits) << count_;
    count_ += n;
    while (count_ >= 8) {
      out_.push_back(uint8_t(buf_));
      buf_ >>= 8;
      count_ -= 8;
    }
  }
  // Huffman codes are sent most-significant bit first.
  void put_code(uint32_t code, int len) {
    uint32_t rev = 0;
    for (int i = 0; i < len; ++i) rev |= ((code >> i) & 1) << (len - 1 - i);
    put(rev, len);
  }
  std::vector<uint8_t> finish() {
    if (count_) out_.push_back(uint8_t(buf_));
    return std::move(out_);
  }

 private:
  std::vector<uint8_t> out_;
  uint64_t buf_ = 0;
  int count_ = 0;
};

void literal(BitWriter& w, uint32_t sym) {
  if (sym < 144) w.put_code(0x30 + sym, 8);
  else if (sym < 256) w.put_code(0x190 + sym - 144, 9);
  else if (sym < 280) w.put_code(sym - 256, 7);
  else w.put_code(0xC0 + sym - 280, 8);
}

void match(BitWriter& w, uint32_t len, uint32_t dist) {
  int i = 28;
  while (kLenBase[i] > len) --i;
  literal(w, 257 + uint32_t(i));
  w.put(len - kLenBase[i], kLenExtra[i]);
  int j = 29;
  while (kDistBase[j] > dist) --j;
  w.put_code(uint32_t(j), 5);
  w.put(dist - kDistBase[j], kDistExtra[j]);
}

}  // namespace

std::vector<uint8_t> deflate_fixed(std::span<const uint8_t> in) {
  constexpr size_t kWindow = 32768;
  constexpr size_t kHashSize = 1 << 15;
  constexpr int kMaxChain = 64;
  BitWriter w;
  w.put(1, 1);  // BFINAL
  w.put(1, 2);  // BTYPE = fixed Huffman
  std::vector<int64_t> head(kHashSize, -1);
  std::vector<int64_t> prev(in.size(), -1);
  auto hash = [&](size_t i) { return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHashSize - 1); };
  size_t i = 0;
  while (i < in.size()) {
    size_t best_len = 0, best_dist = 0;
    if (i + 2 < in.size()) {
      size_t h = hash(i);
      int64_t cand = head[h];
      for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain, cand = prev[size_t(cand)]) {
        size_t dist = i - size_t(cand);
        if (dist > kWindow) break;
        size_t len = 0;
        while (len < 258 && i + len < in.size() && in[size_t(cand) + len] == in[i + len]) ++len;
        if (len > best_len) {
          best_len = len;
          best_dist = dist;
          if (len == 258) break;
        }
      }
    }
    size_t step = best_len >= 3 ? best_len : 1;
    if (best_len >= 3) {
      match(w, uint32_t(best_len), uint32_t(best_dist));
    } else {
      literal(w, in[i]);
    }
    for (size_t k = 0; k < step; ++k, ++i) {
      if (i + 2 < in.size()) {
        size_t h = hash(i);
        prev[i] = head[h];
        head[h] = int64_t(i);
      }
    }
  }
  literal(w, 256);
  return w.finish();
}

void ZipWriter::add(const std::string& name, std::span<const uint8_t> data, const Options& options) {
  Entry e;
  e.name = name;
  e.crc = hash::crc32(data) ^ (options.corrupt_crc ? 1u : 0u);
  e.usize = uint32_t(options.declared_size_override ? options.declared_size_override : data.size());
  e.offset = uint32_t(out_.size());
  std::vector<uint8_t> compressed;
  std::span<const uint8_t> payload = data;
  e.method = 0;
  if (options.compress && !data.empty()) {
    compressed = deflate_fixed(data);
    if (compressed.size() < data.size()) {
      payload = compressed;
      e.method = 8;
    }
  }
  e.csize = uint32_t(payload.size());
  if (options.declared_size_override && e.method == 0) e.csize = e.usize;

  ByteWriter h;
  bool utf8_name = false;
  for (unsigned char c : name) utf8_name |= c >= 0x80;
  h.u32(0x04034b50);
  h.u16(20);
  h.u16(utf8_name ? 0x0800 : 0);
  h.u16(e.method);
  h.u16(0);       // time 00:00
  h.u16(0x0021);  // date 1980-01-01
  h.u32(e.crc);
  h.u32(e.csize);
  h.u32(e.usize);
  h.u16(uint16_t(name.size()));
  h.u16(0);
  h.bytes(std::span(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
  out_.insert(out_.end(), h.buffer().begin(), h.buffer().end());
  out_.insert(out_.end(), payload.begin(), payload.end());
  entries_.push_back(std::move(e));
}

std::vector<uint8_t> ZipWriter::finish() {
  ByteWriter w;
  w.bytes(out_);
  uint32_t cd_offset = uint32_t(w.size());
  for (const Entry& e : entries_) {
    bool utf8_name = false;
    for (unsigned char c : e.name) utf8_name |= c >= 0x80;
    w.u32(0x02014b50);
    w.u16(20);
    w.u16(20);
    w.u16(utf8_name ? 0x0800 : 0);
    w.u16(e.method);
    w.u16(0);
    w.u16(0x0021);
    w.u32(e.crc);
    w.u32(e.csize);
    w.u32(e.usize);
    w.u16(uint16_t(e.name.size()));
    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.u32(0);
    w.u32(e.offset);
    w.bytes(std::span(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size()));
  }
  uint32_t cd_size = uint32_t(w.size()) - cd_offset;
  w.u32(0x06054b50);
  w.u16(0);
  w.u16(0);
  w.u16(uint16_t(entries_.size()));
  w.u16(uint16_t(entries_.size()));
  w.u32(cd_size);
  w.u32(cd_offset);
  w.u16(0);
  return w.take();
}

}  // namespace aa::tool
