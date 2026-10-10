#include "image/lz4.h"

#include <algorithm>

#include "common/bytes.h"

namespace aa::image {

std::vector<uint8_t> lz4_decompress(std::span<const uint8_t> in, size_t max_out) {
  std::vector<uint8_t> out;
  out.reserve(max_out);
  size_t ip = 0;
  auto byte = [&]() -> uint8_t {
    if (ip >= in.size()) throw_format("flux LZ4 tronqué");
    return in[ip++];
  };
  auto length = [&](size_t base) {
    size_t len = base;
    if (base == 15) {
      uint8_t b;
      do {
        b = byte();
        len += b;
        if (len > (1u << 30)) throw_format("longueur LZ4 aberrante");
      } while (b == 255);
    }
    return len;
  };
  while (ip < in.size() && out.size() < max_out) {
    uint8_t token = byte();
    size_t lit = length(token >> 4);
    if (lit > in.size() - ip) throw_format("littéraux LZ4 hors du flux");
    size_t take = std::min(lit, max_out - out.size());
    out.insert(out.end(), in.begin() + ip, in.begin() + ip + take);
    ip += lit;
    if (ip == in.size() || out.size() >= max_out) break;  // last sequence: literals only
    size_t offset = byte();
    offset |= size_t(byte()) << 8;
    if (offset == 0 || offset > out.size()) throw_format("distance LZ4 invalide");
    size_t match = length(token & 15) + 4;
    size_t from = out.size() - offset;
    for (size_t i = 0; i < match && out.size() < max_out; ++i) out.push_back(out[from + i]);
  }
  return out;
}

}  // namespace aa::image
