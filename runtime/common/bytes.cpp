#include "common/bytes.h"

#include <cstdio>

namespace aa {

void throw_format(const std::string& message) { throw FormatError(message); }

ByteReader::ByteReader(std::span<const uint8_t> data, size_t pos) : data_(data), pos_(pos) {
  if (pos > data.size()) throw_format("position initiale hors limites");
}

void ByteReader::require(size_t n, const char* what) const {
  if (n > remaining()) {
    throw_format(std::string("données tronquées (") + what + ", besoin de " + std::to_string(n) +
                 " octets à " + hex(pos_) + ", reste " + std::to_string(remaining()) + ")");
  }
}

void ByteReader::seek(size_t pos) {
  if (pos > data_.size()) throw_format("déplacement hors limites vers " + hex(pos));
  pos_ = pos;
}

void ByteReader::skip(size_t n) {
  require(n, "skip");
  pos_ += n;
}

uint8_t ByteReader::u8() {
  require(1, "u8");
  return data_[pos_++];
}

uint16_t ByteReader::u16() {
  require(2, "u16");
  uint16_t v = load_u16(data_.data() + pos_);
  pos_ += 2;
  return v;
}

uint32_t ByteReader::u32() {
  require(4, "u32");
  uint32_t v = load_u32(data_.data() + pos_);
  pos_ += 4;
  return v;
}

uint64_t ByteReader::u64() {
  uint64_t lo = u32();
  uint64_t hi = u32();
  return lo | (hi << 32);
}

uint32_t ByteReader::uleb128() {
  uint32_t result = 0;
  for (int i = 0; i < 5; ++i) {
    uint8_t b = u8();
    result |= uint32_t(b & 0x7f) << (7 * i);
    if ((b & 0x80) == 0) return result;
  }
  throw_format("uleb128 trop long");
}

int32_t ByteReader::sleb128() {
  uint32_t result = 0;
  int shift = 0;
  uint8_t b = 0;
  for (int i = 0; i < 5; ++i) {
    b = u8();
    result |= uint32_t(b & 0x7f) << shift;
    shift += 7;
    if ((b & 0x80) == 0) {
      if (shift < 32 && (b & 0x40)) result |= ~uint32_t(0) << shift;
      return static_cast<int32_t>(result);
    }
  }
  throw_format("sleb128 trop long");
}

std::span<const uint8_t> ByteReader::bytes(size_t n) {
  require(n, "bytes");
  auto s = data_.subspan(pos_, n);
  pos_ += n;
  return s;
}

uint16_t ByteReader::u16_at(size_t off) const {
  if (off > data_.size() || data_.size() - off < 2) throw_format("lecture u16 hors limites à " + hex(off));
  return load_u16(data_.data() + off);
}

uint32_t ByteReader::u32_at(size_t off) const {
  if (off > data_.size() || data_.size() - off < 4) throw_format("lecture u32 hors limites à " + hex(off));
  return load_u32(data_.data() + off);
}

ByteReader ByteReader::sub(size_t off, size_t len) const {
  if (off > data_.size() || data_.size() - off < len) {
    throw_format("sous-région hors limites (" + hex(off) + " + " + std::to_string(len) + ")");
  }
  return ByteReader(data_.subspan(off, len));
}

void ByteWriter::u16(uint16_t v) {
  u8(uint8_t(v));
  u8(uint8_t(v >> 8));
}

void ByteWriter::u32(uint32_t v) {
  u16(uint16_t(v));
  u16(uint16_t(v >> 16));
}

void ByteWriter::u64(uint64_t v) {
  u32(uint32_t(v));
  u32(uint32_t(v >> 32));
}

void ByteWriter::uleb128(uint32_t v) {
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v) b |= 0x80;
    u8(b);
  } while (v);
}

void ByteWriter::sleb128(int32_t value) {
  int64_t v = value;
  for (;;) {
    uint8_t b = v & 0x7f;
    v >>= 7;
    bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
    if (!done) b |= 0x80;
    u8(b);
    if (done) break;
  }
}

void ByteWriter::align(size_t n) {
  while (buf_.size() % n) buf_.push_back(0);
}

void ByteWriter::patch_u16(size_t off, uint16_t v) {
  buf_.at(off) = uint8_t(v);
  buf_.at(off + 1) = uint8_t(v >> 8);
}

void ByteWriter::patch_u32(size_t off, uint32_t v) {
  patch_u16(off, uint16_t(v));
  patch_u16(off + 2, uint16_t(v >> 16));
}

std::string hex(uint64_t v, int digits) {
  char buf[32];
  if (digits > 0) {
    std::snprintf(buf, sizeof buf, "0x%0*llx", digits, static_cast<unsigned long long>(v));
  } else {
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  }
  return buf;
}

}  // namespace aa
