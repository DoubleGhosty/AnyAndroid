// Tests of the Android image readers (G2): LZ4, sparse and super images,
// ext4, EROFS (flat, chunked and compressed layouts), APEX packages and boot
// class path extraction. Images are synthesised by small builders below that
// follow the on-disk formats (mke2fs, mkfs.erofs, libsparse, liblp).
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "common/bytes.h"
#include "common/file_io.h"
#include "common/hash.h"
#include "image/aosp.h"
#include "image/filesystem.h"
#include "image/lz4.h"
#include "image/source.h"
#include "test.h"
#include "vfs/vfs.h"
#include "zip_writer.h"

using namespace aa;
using namespace aa::image;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }

void put16(Bytes& b, size_t off, uint16_t v) {
  b[off] = uint8_t(v);
  b[off + 1] = uint8_t(v >> 8);
}
void put32(Bytes& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
void put64(Bytes& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
void put(Bytes& b, size_t off, const Bytes& data) { std::copy(data.begin(), data.end(), b.begin() + off); }

Bytes random_bytes(size_t n, uint32_t seed) {
  Bytes out(n);
  uint32_t x = seed ? seed : 1;
  for (auto& c : out) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c = uint8_t(x);
  }
  return out;
}

Bytes pattern_bytes(size_t n, const std::string& motif) {
  Bytes out(n);
  for (size_t i = 0; i < n; ++i) out[i] = uint8_t(motif[i % motif.size()] + (i / 1000) % 3);
  return out;
}

// Greedy LZ4 block compressor (test fixture only).
Bytes lz4_compress(const Bytes& in) {
  Bytes out;
  auto ext = [&](size_t v) {
    while (v >= 255) {
      out.push_back(255);
      v -= 255;
    }
    out.push_back(uint8_t(v));
  };
  std::unordered_map<uint32_t, size_t> table;
  size_t n = in.size(), anchor = 0, i = 0;
  while (n >= 12 && i + 12 <= n) {
    uint32_t seq = load_u32(&in[i]);
    auto it = table.find(seq);
    size_t cand = it == table.end() ? SIZE_MAX : it->second;
    table[seq] = i;
    if (cand != SIZE_MAX && i - cand <= 65535) {
      size_t ref = cand, len = 4;
      while (i + len < n - 5 && in[ref + len] == in[i + len]) ++len;
      size_t lit = i - anchor;
      out.push_back(uint8_t((std::min<size_t>(lit, 15) << 4) | std::min<size_t>(len - 4, 15)));
      if (lit >= 15) ext(lit - 15);
      out.insert(out.end(), in.begin() + anchor, in.begin() + i);
      out.push_back(uint8_t(i - ref));
      out.push_back(uint8_t((i - ref) >> 8));
      if (len - 4 >= 15) ext(len - 4 - 15);
      i += len;
      anchor = i;
      continue;
    }
    ++i;
  }
  size_t lit = n - anchor;
  out.push_back(uint8_t(std::min<size_t>(lit, 15) << 4));
  if (lit >= 15) ext(lit - 15);
  out.insert(out.end(), in.begin() + anchor, in.end());
  return out;
}

// --- Protocol buffers -----------------------------------------------------------

void pb_varint(Bytes& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(uint8_t(v | 0x80));
    v >>= 7;
  }
  out.push_back(uint8_t(v));
}
void pb_bytes(Bytes& out, uint32_t field, const Bytes& data) {
  pb_varint(out, (field << 3) | 2);
  pb_varint(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}
void pb_int(Bytes& out, uint32_t field, uint64_t v) {
  pb_varint(out, field << 3);
  pb_varint(out, v);
}

Bytes classpaths_pb(const std::vector<std::pair<std::string, int>>& jars) {
  Bytes out;
  for (const auto& [path, cp] : jars) {
    Bytes jar;
    pb_bytes(jar, 1, bytes(path));
    pb_int(jar, 2, uint64_t(cp));
    pb_bytes(jar, 3, bytes("30"));
    pb_bytes(out, 1, jar);
  }
  return out;
}

Bytes apex_manifest_pb(const std::string& name, int64_t version) {
  Bytes out;
  pb_bytes(out, 1, bytes(name));
  pb_int(out, 2, uint64_t(version));
  pb_bytes(out, 9, bytes("ignoré"));  // unknown field
  return out;
}

Bytes jar_with_dex(const std::string& marker) {
  tool::ZipWriter z;
  Bytes dex = bytes("dex\n035");
  dex.push_back(0);
  Bytes body = bytes(marker);
  dex.insert(dex.end(), body.begin(), body.end());
  z.add("classes.dex", dex);
  z.add("META-INF/MANIFEST.MF", bytes("Manifest-Version: 1.0\r\n"));
  return z.finish();
}

// --- ext4 image builder ----------------------------------------------------------------

class Ext4Builder {
 public:
  static constexpr uint32_t kBs = 4096;
  enum class Layout { kExtents, kHole, kTree, kBlockMap };

  explicit Ext4Builder(uint32_t blocks = 512, bool bit64 = false) : blocks_(blocks), bit64_(bit64) {
    img_.assign(size_t(blocks) * kBs, 0);
    nodes_[2] = Node{kDir, {}, Layout::kExtents, {}, 2};
  }

  uint32_t root() const { return 2; }
  uint32_t mkdir(uint32_t parent, const std::string& name) { return add(parent, name, Node{kDir, {}, Layout::kExtents, {}, parent}); }
  uint32_t file(uint32_t parent, const std::string& name, Bytes data, Layout layout = Layout::kExtents) {
    return add(parent, name, Node{kFile, std::move(data), layout, {}, parent});
  }
  uint32_t symlink(uint32_t parent, const std::string& name, const std::string& target) {
    return add(parent, name, Node{kLink, bytes(target), Layout::kExtents, {}, parent});
  }

  Bytes finish() {
    for (auto& [ino, n] : nodes_) {
      if (n.kind == kDir) write_dir(ino, n);
      else if (n.kind == kLink) write_link(ino, n);
      else write_file(ino, n);
    }
    // Superblock.
    size_t sb = 1024;
    put32(img_, sb + 0x0, kInodes);
    put32(img_, sb + 0x4, blocks_);
    put32(img_, sb + 0x14, 0);
    put32(img_, sb + 0x18, 2);  // 4 KiB
    put32(img_, sb + 0x1C, 2);
    put32(img_, sb + 0x20, 32768);
    put32(img_, sb + 0x24, 32768);
    put32(img_, sb + 0x28, kInodes);
    put16(img_, sb + 0x38, 0xEF53);
    put16(img_, sb + 0x3A, 1);
    put32(img_, sb + 0x4C, 1);
    put32(img_, sb + 0x54, 11);
    put16(img_, sb + 0x58, kInodeSize);
    put32(img_, sb + 0x60, 0x2 | 0x40 | (bit64_ ? 0x80 : 0));
    put32(img_, sb + 0x64, 0x1);  // sparse_super
    if (bit64_) put16(img_, sb + 0xFE, 64);
    // Group descriptor (block 1): inode table at block 2.
    put32(img_, kBs + 8, 2);
    return img_;
  }

 private:
  enum Kind { kDir, kFile, kLink };
  static constexpr uint32_t kInodes = 64;
  static constexpr uint32_t kInodeSize = 256;
  struct Node {
    Kind kind;
    Bytes data;
    Layout layout;
    std::vector<std::pair<std::string, uint32_t>> children;
    uint32_t parent;
  };

  uint32_t add(uint32_t parent, const std::string& name, Node n) {
    uint32_t ino = next_ino_++;
    CHECK(ino <= kInodes);
    nodes_[parent].children.push_back({name, ino});
    nodes_[ino] = std::move(n);
    return ino;
  }

  uint32_t alloc(uint32_t n) {
    uint32_t b = next_block_;
    next_block_ += n;
    CHECK(next_block_ <= blocks_);
    return b;
  }

  void write_inode(uint32_t ino, uint16_t mode, uint64_t size, uint32_t flags, const uint8_t block[60]) {
    size_t off = 2 * size_t(kBs) + size_t(ino - 1) * kInodeSize;
    put16(img_, off, mode);
    put32(img_, off + 4, uint32_t(size));
    put16(img_, off + 0x1A, 1);
    put32(img_, off + 0x20, flags);
    std::memcpy(&img_[off + 0x28], block, 60);
    put32(img_, off + 0x6C, uint32_t(size >> 32));
  }

  static void extent_header(uint8_t* p, uint16_t entries, uint16_t max, uint16_t depth) {
    p[0] = 0x0A;
    p[1] = 0xF3;
    p[2] = uint8_t(entries);
    p[3] = uint8_t(entries >> 8);
    p[4] = uint8_t(max);
    p[5] = uint8_t(max >> 8);
    p[6] = uint8_t(depth);
    p[7] = uint8_t(depth >> 8);
  }
  static void leaf(uint8_t* p, uint32_t logical, uint16_t len, uint32_t start) {
    Bytes e(12, 0);
    put32(e, 0, logical);
    put16(e, 4, len);
    put32(e, 8, start);
    std::memcpy(p, e.data(), 12);
  }

  void copy_block(uint32_t blk, const Bytes& data, size_t logical) {
    size_t off = logical * kBs;
    size_t n = std::min<size_t>(kBs, data.size() - off);
    std::memcpy(&img_[size_t(blk) * kBs], &data[off], n);
  }

  void write_dir(uint32_t ino, Node& n) {
    Bytes d(kBs, 0);
    std::vector<std::tuple<std::string, uint32_t, uint8_t>> entries{{".", ino, 2}, {"..", n.parent, 2}};
    for (const auto& [name, child] : n.children) {
      Kind k = nodes_[child].kind;
      entries.emplace_back(name, child, k == kDir ? 2 : k == kLink ? 7 : 1);
    }
    size_t pos = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& [name, child, type] = entries[i];
      size_t rec = (8 + name.size() + 3) / 4 * 4;
      if (i + 1 == entries.size()) rec = kBs - pos;
      CHECK(pos + rec <= kBs);
      put32(d, pos, child);
      put16(d, pos + 4, uint16_t(rec));
      d[pos + 6] = uint8_t(name.size());
      d[pos + 7] = type;
      std::memcpy(&d[pos + 8], name.data(), name.size());
      pos += rec;
    }
    uint32_t blk = alloc(1);
    std::memcpy(&img_[size_t(blk) * kBs], d.data(), kBs);
    uint8_t block[60] = {};
    extent_header(block, 1, 4, 0);
    leaf(block + 12, 0, 1, blk);
    write_inode(ino, 0x41ED, kBs, 0x80000, block);
  }

  void write_link(uint32_t ino, Node& n) {
    uint8_t block[60] = {};
    if (n.data.size() < 60) {
      std::memcpy(block, n.data.data(), n.data.size());
      write_inode(ino, 0xA1FF, n.data.size(), 0, block);
      return;
    }
    uint32_t blk = alloc(1);
    copy_block(blk, n.data, 0);
    extent_header(block, 1, 4, 0);
    leaf(block + 12, 0, 1, blk);
    write_inode(ino, 0xA1FF, n.data.size(), 0x80000, block);
  }

  void write_file(uint32_t ino, Node& n) {
    uint8_t block[60] = {};
    uint32_t nb = uint32_t((n.data.size() + kBs - 1) / kBs);
    uint32_t flags = 0x80000;
    switch (n.layout) {
      case Layout::kExtents: {
        extent_header(block, nb ? 1 : 0, 4, 0);
        if (nb) {
          uint32_t blk = alloc(nb);
          for (uint32_t i = 0; i < nb; ++i) copy_block(blk + i, n.data, i);
          leaf(block + 12, 0, uint16_t(nb), blk);
        }
        break;
      }
      case Layout::kHole: {  // logical block 1 is a hole
        CHECK(nb >= 3);
        extent_header(block, 2, 4, 0);
        uint32_t a = alloc(1);
        copy_block(a, n.data, 0);
        uint32_t b = alloc(nb - 2);
        for (uint32_t i = 2; i < nb; ++i) copy_block(b + i - 2, n.data, i);
        leaf(block + 12, 0, 1, a);
        leaf(block + 24, 2, uint16_t(nb - 2), b);
        break;
      }
      case Layout::kTree: {  // depth-1 tree, one non-contiguous extent per block
        uint32_t leaf_blk = alloc(1);
        uint8_t* lp = &img_[size_t(leaf_blk) * kBs];
        extent_header(lp, uint16_t(nb), uint16_t((kBs - 12) / 12), 0);
        for (uint32_t i = 0; i < nb; ++i) {
          uint32_t b = alloc(2);  // leave a gap after each block
          copy_block(b, n.data, i);
          leaf(lp + 12 + 12 * i, i, 1, b);
        }
        extent_header(block, 1, 4, 1);
        Bytes idx(12, 0);
        put32(idx, 0, 0);
        put32(idx, 4, leaf_blk);
        std::memcpy(block + 12, idx.data(), 12);
        break;
      }
      case Layout::kBlockMap: {  // direct + single indirect blocks, zero blocks left sparse
        flags = 0;
        Bytes ptrs(60, 0);
        Bytes indirect(kBs, 0);
        bool need_indirect = nb > 12;
        for (uint32_t i = 0; i < nb; ++i) {
          size_t off = size_t(i) * kBs;
          size_t len = std::min<size_t>(kBs, n.data.size() - off);
          bool zero = std::all_of(n.data.begin() + off, n.data.begin() + off + len, [](uint8_t c) { return c == 0; });
          uint32_t b = 0;
          if (!zero) {
            b = alloc(1);
            copy_block(b, n.data, i);
          }
          if (i < 12) put32(ptrs, 4 * i, b);
          else put32(indirect, 4 * (i - 12), b);
        }
        if (need_indirect) {
          uint32_t ib = alloc(1);
          std::memcpy(&img_[size_t(ib) * kBs], indirect.data(), kBs);
          put32(ptrs, 48, ib);
        }
        std::memcpy(block, ptrs.data(), 60);
        break;
      }
    }
    write_inode(ino, 0x81A4, n.data.size(), flags, block);
  }

  Bytes img_;
  uint32_t blocks_;
  bool bit64_;
  uint32_t next_block_ = 6;  // 0: superblock, 1: descriptors, 2-5: inode table
  uint32_t next_ino_ = 11;
  std::map<uint32_t, Node> nodes_;
};

// --- EROFS image builder ---------------------------------------------------------------------

struct ZSpec {
  std::vector<uint64_t> starts;  // extent boundaries (first is 0)
  std::string kinds;             // per extent: 'z' LZ4, 'p' plain (uncompressed)
  bool compact = false;
  bool compacted_2b = false;
  bool big = false;
  bool tail_inline = false;
  uint8_t alg = 0;
  bool fragment = false;
};

class ErofsBuilder {
 public:
  static constexpr uint32_t kBs = 4096;
  enum class Layout { kPlain, kInline, kChunked, kExtended, kCompressed };

  explicit ErofsBuilder(bool zero_padding = true) : zero_padding_(zero_padding) {
    img_.assign(kBs, 0);
    root_ = new_node(Node{kDir, {}, Layout::kInline, {}, 0, {}});
    nodes_[root_].parent = root_;
  }

  uint64_t root() const { return root_; }
  uint64_t mkdir(uint64_t parent, const std::string& name) { return add(parent, name, Node{kDir, {}, Layout::kInline, {}, parent, {}}); }
  uint64_t file(uint64_t parent, const std::string& name, Bytes data, Layout layout = Layout::kPlain) {
    return add(parent, name, Node{kFile, std::move(data), layout, {}, parent, {}});
  }
  uint64_t compressed(uint64_t parent, const std::string& name, Bytes data, ZSpec z) {
    return add(parent, name, Node{kFile, std::move(data), Layout::kCompressed, {}, parent, std::move(z)});
  }
  uint64_t symlink(uint64_t parent, const std::string& name, const std::string& target) {
    return add(parent, name, Node{kLink, bytes(target), Layout::kInline, {}, parent, {}});
  }

  Bytes finish() {
    for (auto& [nid, n] : nodes_) {
      if (n.kind == kDir) write_dir(nid, n);
      else if (n.layout == Layout::kCompressed) write_compressed(nid, n);
      else write_flat(nid, n);
    }
    size_t sb = 1024;
    put32(img_, sb, 0xE0F5E1E2);
    img_[sb + 12] = 12;
    put16(img_, sb + 14, uint16_t(root_));
    put32(img_, sb + 36, uint32_t(img_.size() / kBs));
    put32(img_, sb + 40, kMetaBlk);
    uint32_t features = (zero_padding_ ? 0x1 : 0) | features_;
    put32(img_, sb + 80, features);
    if (features & 0x2) put16(img_, sb + 84, 1);  // LZ4 only
    return img_;
  }

 private:
  enum Kind { kDir, kFile, kLink };
  static constexpr uint32_t kMetaBlk = 1;
  struct Node {
    Kind kind;
    Bytes data;
    Layout layout;
    std::vector<std::pair<std::string, uint64_t>> children;
    uint64_t parent;
    ZSpec z;
  };

  uint32_t alloc(uint32_t n) {
    uint32_t b = uint32_t(img_.size() / kBs);
    img_.resize(img_.size() + size_t(n) * kBs, 0);
    return b;
  }
  // Every inode gets its own metadata block (inline data and indexes fit in it).
  uint64_t new_node(Node n) {
    uint32_t blk = alloc(1);
    uint64_t nid = uint64_t(blk - kMetaBlk) * (kBs / 32);
    nodes_[nid] = std::move(n);
    return nid;
  }
  uint64_t add(uint64_t parent, const std::string& name, Node n) {
    uint64_t nid = new_node(std::move(n));
    nodes_[parent].children.push_back({name, nid});
    return nid;
  }
  size_t pos_of(uint64_t nid) const { return size_t(kMetaBlk) * kBs + size_t(nid) * 32; }

  void inode(uint64_t nid, uint8_t layout, uint16_t mode, uint64_t size, uint32_t u, bool extended = false,
             uint16_t xattr_count = 0) {
    size_t p = pos_of(nid);
    put16(img_, p, uint16_t((extended ? 1 : 0) | (layout << 1)));
    put16(img_, p + 2, xattr_count);
    put16(img_, p + 4, mode);
    if (extended) {
      put64(img_, p + 8, size);
      put32(img_, p + 16, u);
      put32(img_, p + 44, 1);
    } else {
      put16(img_, p + 6, 1);
      put32(img_, p + 8, uint32_t(size));
      put32(img_, p + 16, u);
    }
  }

  void write_dir(uint64_t nid, Node& n) {
    std::vector<std::tuple<std::string, uint64_t, uint8_t>> entries{{".", nid, 2}, {"..", n.parent, 2}};
    for (const auto& [name, child] : n.children) {
      Kind k = nodes_[child].kind;
      entries.emplace_back(name, child, k == kDir ? 2 : k == kLink ? 7 : 1);
    }
    std::sort(entries.begin(), entries.end());
    Bytes d(entries.size() * 12, 0);
    std::string names;
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& [name, child, type] = entries[i];
      put64(d, i * 12, child);
      put16(d, i * 12 + 8, uint16_t(entries.size() * 12 + names.size()));
      d[i * 12 + 10] = type;
      names += name;
    }
    d.insert(d.end(), names.begin(), names.end());
    CHECK(d.size() + 32 <= kBs);
    inode(nid, 2, 0x41ED, d.size(), 0);
    put(img_, pos_of(nid) + 32, d);
  }

  void write_flat(uint64_t nid, Node& n) {
    const Bytes& data = n.data;
    uint16_t mode = n.kind == kLink ? 0xA1FF : 0x81A4;
    size_t p = pos_of(nid);
    uint64_t nblocks = (data.size() + kBs - 1) / kBs;
    switch (n.layout) {
      case Layout::kPlain: {
        uint32_t b = nblocks ? alloc(uint32_t(nblocks)) : 0;
        if (nblocks) std::memcpy(&img_[size_t(b) * kBs], data.data(), data.size());
        inode(nid, 0, mode, data.size(), b);
        break;
      }
      case Layout::kInline:
      case Layout::kExtended: {
        bool ext = n.layout == Layout::kExtended;
        uint16_t xattrs = ext ? 3 : 0;
        size_t tail_at = p + (ext ? 64 : 32) + (xattrs ? 12 + 4 * (xattrs - 1) : 0);
        size_t head = nblocks ? (nblocks - 1) * kBs : 0;
        uint32_t b = head ? alloc(uint32_t(head / kBs)) : 0;
        if (head) std::memcpy(&img_[size_t(b) * kBs], data.data(), head);
        CHECK(tail_at % kBs + (data.size() - head) <= kBs);
        std::memcpy(&img_[tail_at], data.data() + head, data.size() - head);
        inode(nid, 2, mode, data.size(), b, ext, xattrs);
        break;
      }
      case Layout::kChunked: {  // one block per chunk, 8-byte indexes, zero blocks are holes
        features_ |= 0x4;
        size_t table = p + 32;
        for (uint64_t i = 0; i < nblocks; ++i) {
          size_t off = i * kBs, len = std::min<size_t>(kBs, data.size() - off);
          bool zero = std::all_of(data.begin() + off, data.begin() + off + len, [](uint8_t c) { return c == 0; });
          uint32_t b = 0xFFFFFFFF;
          if (!zero) {
            b = alloc(1);
            std::memcpy(&img_[size_t(b) * kBs], data.data() + off, len);
          }
          put32(img_, table + i * 8 + 4, b);
        }
        inode(nid, 4, mode, data.size(), 0x20);
        break;
      }
      case Layout::kCompressed: break;
    }
  }

  void write_compressed(uint64_t nid, Node& n) {
    const ZSpec& z = n.z;
    const Bytes& data = n.data;
    const uint64_t lcs = kBs, size = data.size();
    const uint64_t N = (size + lcs - 1) / lcs;
    const size_t K = z.starts.size();
    CHECK(z.kinds.size() == K && z.starts[0] == 0);
    if (z.big) features_ |= 0x2;
    if (z.tail_inline) features_ |= 0x10;

    struct Ext {
      uint64_t start, end, lcn;
      char kind;
      uint32_t blk = 0, nblk = 1;
    };
    std::vector<Ext> ext;
    Bytes idata;
    uint32_t total_blocks = 0;
    for (size_t k = 0; k < K; ++k) {
      Ext e{z.starts[k], k + 1 < K ? z.starts[k + 1] : size, z.starts[k] / lcs, z.kinds[k]};
      Bytes chunk(data.begin() + e.start, data.begin() + e.end);
      if (z.tail_inline && k + 1 == K) {
        idata = e.kind == 'p' ? chunk : lz4_compress(chunk);
      } else if (e.kind == 'p') {
        CHECK(chunk.size() <= kBs);
        e.blk = alloc(1);
        std::memcpy(&img_[size_t(e.blk) * kBs], chunk.data(), chunk.size());
      } else {
        Bytes c = lz4_compress(chunk);
        e.nblk = uint32_t((c.size() + kBs - 1) / kBs);
        CHECK(e.nblk == 1 || z.big);
        e.blk = alloc(e.nblk);
        size_t room = size_t(e.nblk) * kBs;
        size_t at = size_t(e.blk) * kBs + (zero_padding_ ? room - c.size() : 0);
        std::memcpy(&img_[at], c.data(), c.size());
      }
      total_blocks += e.nblk;
      ext.push_back(e);
    }

    // Logical cluster table.
    struct L {
      uint8_t type;
      uint32_t lo;      // clusterofs (head) or delta0 / CBLKCNT (nonhead)
      uint32_t delta1;  // nonhead: distance to the next head
      uint32_t blk;     // head: pcluster address
    };
    std::vector<L> lc(N);
    for (uint64_t lcn = 0; lcn < N; ++lcn) {
      const Ext* head = nullptr;
      const Ext* owner = nullptr;
      for (const auto& e : ext) {
        if (e.lcn == lcn) {
          CHECK(!head);
          head = &e;
        }
        if (e.start <= lcn * lcs && lcn * lcs < e.end) owner = &e;
      }
      if (head) {
        lc[lcn] = L{uint8_t(head->kind == 'p' ? 0 : 1), uint32_t(head->start % lcs), 0, head->blk};
        continue;
      }
      CHECK(owner);
      uint32_t d0 = uint32_t(lcn - owner->lcn);
      uint64_t next = N;
      for (const auto& e : ext) {
        if (e.lcn > lcn) {
          next = e.lcn;
          break;
        }
      }
      uint32_t lo = d0;
      if (z.big && d0 == 1) lo = 0x800 | owner->nblk;
      lc[lcn] = L{2, lo, uint32_t(next - lcn), 0};
    }

    size_t p = pos_of(nid);
    size_t end = p + 32;
    size_t hdr = (end + 7) / 8 * 8;
    uint16_t advise = (z.compacted_2b ? 0x1 : 0) | (z.big ? 0x6 : 0) | (z.tail_inline ? 0x8 : 0);
    put16(img_, hdr + 2, uint16_t(idata.size()));
    put16(img_, hdr + 4, advise);
    img_[hdr + 6] = uint8_t(z.alg | (z.alg << 4));
    img_[hdr + 7] = z.fragment ? 0x80 : 0;
    size_t tail_off = 0;
    if (!z.compact) {
      size_t base = hdr + 16;
      for (uint64_t i = 0; i < N; ++i) {
        size_t q = base + i * 8;
        if (lc[i].type == 2) {
          put16(img_, q, 2);
          put16(img_, q + 4, uint16_t(lc[i].lo));
          put16(img_, q + 6, uint16_t(lc[i].delta1));
        } else {
          put16(img_, q, lc[i].type);
          put16(img_, q + 2, uint16_t(lc[i].lo));
          put32(img_, q + 4, lc[i].blk);
        }
      }
      tail_off = base + N * 8;
    } else {
      size_t ebase = hdr + 8;
      uint64_t c4 = (32 - ebase % 32) / 4;
      if (c4 == 8) c4 = 0;
      uint64_t c2 = (z.compacted_2b && c4 < N) ? (N - c4) / 16 * 16 : 0;
      size_t q = ebase;
      uint64_t i = 0;
      auto pack = [&](uint32_t vcnt, uint32_t unit) {
        uint32_t packsize = vcnt * unit;
        uint32_t encodebits = (packsize - 4) * 8 / vcnt;
        std::vector<uint32_t> vals(vcnt, 0);
        std::vector<uint8_t> types(vcnt, 0);
        int first_head = -1;
        for (uint32_t j = 0; j < vcnt && i + j < N; ++j) {
          const L& l = lc[i + j];
          types[j] = l.type;
          uint32_t lo = l.lo;
          if (l.type == 2 && !(lo & 0x800) && j + 1 == vcnt) lo = std::min<uint32_t>(l.delta1, 4095);
          vals[j] = lo;
          if (l.type != 2 && first_head < 0) first_head = int(j);
        }
        for (uint32_t j = 0; j < vcnt; ++j) {
          uint32_t v = (uint32_t(types[j]) << 12) | vals[j];
          uint32_t bit = encodebits * j;
          for (int k = 0; k < 3; ++k) img_[q + bit / 8 + k] |= uint8_t((v << (bit & 7)) >> (8 * k));
        }
        uint32_t stored = 0;
        if (first_head >= 0) {  // same scan as the kernel (zmap.c, unpack_compacted_index)
          int s = first_head;
          int64_t nblk;
          if (!z.big) {
            nblk = 1;
            while (s > 0) {
              --s;
              if (types[s] == 2) s -= int(vals[s]);
              if (s >= 0) ++nblk;
            }
          } else {
            nblk = 0;
            while (s > 0) {
              --s;
              uint32_t lo = vals[s];
              if (types[s] == 2) {
                if (lo & 0x800) {
                  --s;
                  nblk += lo & 0x7FF;
                  continue;
                }
                s -= int(lo) - 2;
                continue;
              }
              ++nblk;
            }
          }
          stored = uint32_t(int64_t(lc[i + first_head].blk) - nblk);
        }
        put32(img_, q + packsize - 4, stored);
        q += packsize;
        i += vcnt;
      };
      while (i < c4) pack(2, 4);
      uint64_t end2 = c4 + c2;
      while (i < end2) pack(16, 2);
      while (i < N) pack(2, 4);
      tail_off = q;  // the last pack holds lcluster N - 1
    }
    if (z.tail_inline) {
      CHECK(tail_off % kBs + idata.size() <= kBs);
      put(img_, tail_off, idata);
    }
    inode(nid, z.compact ? 3 : 1, 0x81A4, size, total_blocks);
  }

  Bytes img_;
  bool zero_padding_;
  uint32_t features_ = 0;
  uint64_t root_ = 0;
  std::map<uint64_t, Node> nodes_;
};

// --- Sparse and super images --------------------------------------------------------------------

Bytes to_sparse(const Bytes& raw, uint32_t bs = 4096) {
  CHECK(raw.size() % bs == 0);
  Bytes out(28, 0);
  uint32_t chunks = 0;
  auto chunk = [&](uint16_t type, uint32_t blocks, const Bytes& payload) {
    size_t at = out.size();
    out.resize(at + 12);
    put16(out, at, type);
    put32(out, at + 4, blocks);
    put32(out, at + 8, uint32_t(12 + payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    ++chunks;
  };
  uint32_t nblocks = uint32_t(raw.size() / bs);
  uint32_t i = 0;
  while (i < nblocks) {
    auto blk = [&](uint32_t b) { return Bytes(raw.begin() + size_t(b) * bs, raw.begin() + size_t(b + 1) * bs); };
    auto fill_value = [&](uint32_t b, uint32_t& v) {
      v = load_u32(&raw[size_t(b) * bs]);
      for (uint32_t k = 0; k < bs; k += 4) {
        if (load_u32(&raw[size_t(b) * bs + k]) != v) return false;
      }
      return true;
    };
    uint32_t v;
    if (fill_value(i, v)) {
      uint32_t j = i + 1, w;
      while (j < nblocks && fill_value(j, w) && w == v) ++j;
      if (v == 0) {
        chunk(0xCAC3, j - i, {});
      } else {
        Bytes f(4);
        put32(f, 0, v);
        chunk(0xCAC2, j - i, f);
      }
      i = j;
    } else {
      uint32_t j = i + 1, w;
      while (j < nblocks && !fill_value(j, w)) ++j;
      Bytes payload;
      for (uint32_t b = i; b < j; ++b) {
        Bytes d = blk(b);
        payload.insert(payload.end(), d.begin(), d.end());
      }
      chunk(0xCAC1, j - i, payload);
      i = j;
    }
  }
  chunk(0xCAC4, 0, Bytes(4, 0));
  put32(out, 0, 0xED26FF3A);
  put16(out, 4, 1);
  put16(out, 8, 28);
  put16(out, 10, 12);
  put32(out, 12, bs);
  put32(out, 16, nblocks);
  put32(out, 20, chunks);
  return out;
}

// super.img with one partition split in two extents at swapped physical positions.
Bytes make_super(const Bytes& partition, const std::string& name) {
  CHECK(partition.size() % 1024 == 0);
  uint64_t half = partition.size() / 2 / 512 * 512;
  uint64_t first_at = 1u << 20, second_at = first_at + partition.size() + (64u << 10);
  Bytes img(size_t(second_at + (partition.size() - half) + 4096) / 4096 * 4096, 0);
  std::copy(partition.begin(), partition.begin() + half, img.begin() + second_at);
  std::copy(partition.begin() + half, partition.end(), img.begin() + first_at);

  Bytes geo(52, 0);
  put32(geo, 0, 0x616c4467);
  put32(geo, 4, 52);
  put32(geo, 40, 65536);
  put32(geo, 44, 2);
  put32(geo, 48, 4096);
  auto sum = hash::sha256(geo);
  std::copy(sum.begin(), sum.end(), geo.begin() + 8);
  put(img, 4096, geo);
  put(img, 8192, geo);

  Bytes tables;
  auto append = [&](const Bytes& b) { tables.insert(tables.end(), b.begin(), b.end()); };
  Bytes part(52, 0);
  std::copy(name.begin(), name.end(), part.begin());
  put32(part, 40, 0);  // first extent
  put32(part, 44, 2);  // extent count
  append(part);
  Bytes e1(24, 0), e2(24, 0);
  put64(e1, 0, half / 512);
  put64(e1, 12, second_at / 512);
  put64(e2, 0, (partition.size() - half) / 512);
  put64(e2, 12, first_at / 512);
  append(e1);
  append(e2);
  Bytes group(48, 0);
  std::string g = "default";
  std::copy(g.begin(), g.end(), group.begin());
  append(group);
  Bytes dev(64, 0);
  append(dev);

  Bytes header(128, 0);
  put32(header, 0, 0x414C5030);
  put16(header, 4, 10);
  put32(header, 8, 128);
  put32(header, 44, uint32_t(tables.size()));
  auto tsum = hash::sha256(tables);
  std::copy(tsum.begin(), tsum.end(), header.begin() + 48);
  put32(header, 80, 0);
  put32(header, 84, 1);
  put32(header, 88, 52);
  put32(header, 92, 52);
  put32(header, 96, 2);
  put32(header, 100, 24);
  put32(header, 104, 52 + 48);
  put32(header, 108, 1);
  put32(header, 112, 48);
  put32(header, 116, 52 + 48 + 48);
  put32(header, 120, 1);
  put32(header, 124, 64);
  auto hsum = hash::sha256(header);
  std::copy(hsum.begin(), hsum.end(), header.begin() + 12);
  put(img, 12288, header);
  put(img, 12288 + 128, tables);
  return img;
}

std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / ("anyandroid-test-" + name);
}

std::shared_ptr<const Source> mem(Bytes b) { return std::make_shared<MemorySource>(std::move(b)); }

std::string names_of(const std::vector<DirEntry>& entries) {
  std::vector<std::string> n;
  for (const auto& e : entries) n.push_back(e.name);
  std::sort(n.begin(), n.end());
  std::string s;
  for (const auto& x : n) s += (s.empty() ? "" : ",") + x;
  return s;
}

}  // namespace

// --- LZ4 ---------------------------------------------------------------------------------------------

TEST(lz4_decodes_literals_and_overlapping_matches) {
  // "abc" + match(offset 3, length 9) + "X"
  Bytes in{0x35, 'a', 'b', 'c', 0x03, 0x00, 0x10, 'X'};
  CHECK_EQ(str(lz4_decompress(in, 100)), std::string("abcabcabcabcX"));
  CHECK_EQ(str(lz4_decompress(in, 5)), std::string("abcab"));  // partial decoding
}

TEST(lz4_round_trip_with_long_lengths) {
  for (const Bytes& data : {pattern_bytes(70000, "AnyAndroid "), random_bytes(5000, 7), Bytes(300, 'z'), Bytes{}}) {
    Bytes c = lz4_compress(data);
    CHECK(lz4_decompress(c, data.size()) == data);
  }
}

TEST(lz4_rejects_malformed_input) {
  CHECK_THROWS(lz4_decompress(Bytes{0x35, 'a', 'b', 'c', 0x00, 0x00, 0x10, 'X'}, 100), FormatError);  // offset 0
  CHECK_THROWS(lz4_decompress(Bytes{0x14, 'a', 0x05, 0x00, 0x10, 'X'}, 100), FormatError);  // offset > output
  CHECK_THROWS(lz4_decompress(Bytes{0xF0, 0xFF}, 100), FormatError);                       // truncated length
  CHECK_THROWS(lz4_decompress(Bytes{0x50, 'a', 'b'}, 100), FormatError);                   // literals past end
}

// --- ext4 ---------------------------------------------------------------------------------------------

TEST(ext4_reads_all_block_layouts) {
  for (bool bit64 : {false, true}) {
    Ext4Builder b(1024, bit64);
    uint32_t sys = b.mkdir(b.root(), "system");
    uint32_t fw = b.mkdir(sys, "framework");
    Bytes small = bytes("ro.system.build.version.sdk=37\n");
    Bytes big = pattern_bytes(3 * 4096 + 123, "ext4 ");
    Bytes holed = random_bytes(4 * 4096 + 10, 3);
    std::fill(holed.begin() + 4096, holed.begin() + 8192, 0);
    Bytes tree = random_bytes(5 * 4096 + 7, 4);
    Bytes mapped = random_bytes(14 * 4096 + 99, 5);
    std::fill(mapped.begin() + 5 * 4096, mapped.begin() + 6 * 4096, 0);  // sparse direct block
    b.file(sys, "build.prop", small);
    b.file(fw, "framework.jar", big);
    b.file(fw, "holed.bin", holed, Ext4Builder::Layout::kHole);
    b.file(fw, "tree.bin", tree, Ext4Builder::Layout::kTree);
    b.file(fw, "mapped.bin", mapped, Ext4Builder::Layout::kBlockMap);
    b.file(fw, "empty", {});
    b.symlink(b.root(), "fw", "system/framework");
    b.symlink(sys, "abs", "/system/framework/framework.jar");
    std::string long_target = "/system/" + std::string(70, 'x');
    b.symlink(sys, "long", long_target);

    auto fs = open_filesystem(mem(b.finish()));
    CHECK_EQ(fs->kind(), std::string("ext4"));
    CHECK_EQ(names_of(fs->list("/")), std::string("fw,system"));
    CHECK_EQ(names_of(fs->list("/system/framework")), std::string("empty,framework.jar,holed.bin,mapped.bin,tree.bin"));
    CHECK(fs->read_file("/system/build.prop", 1 << 20) == small);
    CHECK(fs->read_file("/system/framework/framework.jar", 1 << 20) == big);
    CHECK(fs->read_file("/system/framework/holed.bin", 1 << 20) == holed);
    CHECK(fs->read_file("/system/framework/tree.bin", 1 << 20) == tree);
    CHECK(fs->read_file("/system/framework/mapped.bin", 1 << 20) == mapped);
    CHECK(fs->read_file("/system/framework/empty", 1 << 20).empty());
    // Symbolic links: relative, absolute, through a directory link, long target.
    CHECK(fs->read_file("/fw/framework.jar", 1 << 20) == big);
    CHECK(fs->read_file("/system/abs", 1 << 20) == big);
    // ".." after a directory link goes to the parent of its target (POSIX).
    CHECK(fs->read_file("/system/../fw/../build.prop", 1 << 20) == small);
    auto link = fs->stat("/system/long", false);
    CHECK(link && link->type == FileType::kSymlink);
    CHECK_EQ(fs->read_link(link->inode), long_target);
    CHECK(!fs->stat("/system/absent"));
    CHECK_THROWS(fs->read_file("/system/framework/framework.jar", 100), FormatError);  // size limit
    CHECK_THROWS(fs->list("/system/build.prop"), FormatError);
  }
}

TEST(ext4_rejects_unsupported_or_corrupt_images) {
  Ext4Builder b(64);
  b.file(b.root(), "a", bytes("x"));
  Bytes img = b.finish();
  Bytes enc = img;
  put32(enc, 1024 + 0x60, load_u32(&enc[1024 + 0x60]) | 0x10000);  // encryption
  CHECK_THROWS(open_filesystem(mem(enc)), FormatError);
  Bytes truncated(img.begin(), img.begin() + 16 * 4096);
  CHECK_THROWS(open_filesystem(mem(truncated)), FormatError);
  Ext4Builder c(64);  // a symlink to itself
  c.symlink(c.root(), "boucle", "/boucle");
  auto fs = open_filesystem(mem(c.finish()));
  CHECK_THROWS(fs->read_file("/boucle", 100), FormatError);
  CHECK_THROWS(fs->stat_inode(9999), FormatError);
}

// --- EROFS ----------------------------------------------------------------------------------------------

TEST(erofs_reads_flat_chunked_and_inline_layouts) {
  ErofsBuilder b;
  uint64_t sys = b.mkdir(b.root(), "system");
  uint64_t etc = b.mkdir(sys, "etc");
  Bytes plain = random_bytes(3 * 4096 + 17, 11);
  Bytes inl = pattern_bytes(2 * 4096 + 900, "inline ");
  Bytes chunked = random_bytes(4 * 4096 + 5, 12);
  std::fill(chunked.begin() + 4096, chunked.begin() + 8192, 0);  // hole chunk
  Bytes extended = pattern_bytes(4096 + 1000, "extended ");
  b.file(sys, "plain.bin", plain, ErofsBuilder::Layout::kPlain);
  b.file(sys, "inline.bin", inl, ErofsBuilder::Layout::kInline);
  b.file(etc, "chunked.bin", chunked, ErofsBuilder::Layout::kChunked);
  b.file(etc, "extended.bin", extended, ErofsBuilder::Layout::kExtended);
  b.file(etc, "small.txt", bytes("bonjour"), ErofsBuilder::Layout::kInline);
  b.symlink(b.root(), "etc", "/system/etc");

  auto fs = open_filesystem(mem(b.finish()));
  CHECK_EQ(fs->kind(), std::string("erofs"));
  CHECK_EQ(names_of(fs->list("/")), std::string("etc,system"));
  CHECK_EQ(names_of(fs->list("/system")), std::string("etc,inline.bin,plain.bin"));
  CHECK(fs->read_file("/system/plain.bin", 1 << 20) == plain);
  CHECK(fs->read_file("/system/inline.bin", 1 << 20) == inl);
  CHECK(fs->read_file("/etc/chunked.bin", 1 << 20) == chunked);
  CHECK(fs->read_file("/etc/extended.bin", 1 << 20) == extended);
  CHECK_EQ(str(fs->read_file("/system/etc/small.txt", 100)), std::string("bonjour"));
  auto st = fs->stat("/etc", false);
  CHECK(st && st->type == FileType::kSymlink);
}

TEST(erofs_reads_compressed_files_with_full_indexes) {
  for (bool padding : {true, false}) {
    ErofsBuilder b(padding);
    Bytes data = pattern_bytes(20000, "compression LZ4 ");
    Bytes raw = random_bytes(3000, 21);
    std::copy(raw.begin(), raw.end(), data.begin() + 6000);
    ZSpec z;
    z.starts = {0, 6000, 9000};  // heads in lclusters 0, 1 (ofs 1904) and 2 (ofs 808)
    z.kinds = "zpz";
    b.compressed(b.root(), "z.bin", data, z);
    auto fs = open_filesystem(mem(b.finish()));
    CHECK(fs->read_file("/z.bin", 1 << 20) == data);
  }
}

TEST(erofs_reads_compact_indexes_big_pclusters_and_inline_tail) {
  Bytes data = pattern_bytes(122000, "Android Open Source Project ");
  Bytes r1 = random_bytes(7000, 31), r2 = random_bytes(3000, 32);
  std::copy(r1.begin(), r1.end(), data.begin() + 20000);  // 2-block pcluster
  std::copy(r2.begin(), r2.end(), data.begin() + 27000);  // stored uncompressed
  ZSpec base;
  base.starts = {0, 5000, 13000, 20000, 27000, 30000, 52000, 90000, 118000};
  base.kinds = "zzzzpzzzz";
  base.big = true;
  struct Variant {
    bool compact, c2b, tail;
  };
  for (Variant v : {Variant{true, true, true}, Variant{true, true, false}, Variant{true, false, true},
                    Variant{false, false, true}, Variant{false, false, false}}) {
    ZSpec z = base;
    z.compact = v.compact;
    z.compacted_2b = v.c2b;
    z.tail_inline = v.tail;
    ErofsBuilder b;
    b.compressed(b.root(), "big.bin", data, z);
    auto fs = open_filesystem(mem(b.finish()));
    CHECK(fs->read_file("/big.bin", 1 << 20) == data);
  }
  // Small pclusters only (no big pcluster feature), compact 4B and 2B indexes.
  Bytes small = pattern_bytes(90000, "abcdefgh");
  ZSpec z;
  z.starts = {0, 4500, 9000, 30000, 61000, 70000};
  z.kinds = "zzzzzz";
  for (bool c2b : {false, true}) {
    z.compact = true;
    z.compacted_2b = c2b;
    ErofsBuilder b;
    b.compressed(b.root(), "s.bin", small, z);
    auto fs = open_filesystem(mem(b.finish()));
    CHECK(fs->read_file("/s.bin", 1 << 20) == small);
  }
}

TEST(erofs_rejects_unsupported_compression) {
  Bytes data = pattern_bytes(9000, "x");
  ZSpec z;
  z.starts = {0};
  z.kinds = "z";
  z.alg = 1;  // LZMA
  ErofsBuilder b;
  b.compressed(b.root(), "lzma.bin", data, z);
  auto fs = open_filesystem(mem(b.finish()));
  CHECK_THROWS(fs->read_file("/lzma.bin", 1 << 20), FormatError);
  z.alg = 0;
  z.fragment = true;
  ErofsBuilder c;
  c.compressed(c.root(), "frag.bin", data, z);
  auto fs2 = open_filesystem(mem(c.finish()));
  CHECK_THROWS(fs2->read_file("/frag.bin", 1 << 20), FormatError);
  Bytes bad = c.finish();
  put32(bad, 1024 + 80, 0x100);  // unknown incompatible feature
  CHECK_THROWS(open_filesystem(mem(bad)), FormatError);
}

// --- Containers: sparse and super images -------------------------------------------------------------------

TEST(sparse_image_expands_raw_fill_and_dont_care_chunks) {
  Ext4Builder b(64);
  Bytes content = pattern_bytes(10000, "sparse ");
  b.file(b.root(), "f", content);
  Bytes raw = b.finish();
  std::fill(raw.begin() + 40 * 4096, raw.begin() + 42 * 4096, 0xAB);  // fill chunk
  Bytes sparse = to_sparse(raw);
  CHECK(sparse.size() < raw.size());
  auto src = std::make_shared<SparseSource>(mem(sparse));
  CHECK_EQ(src->size(), uint64_t(raw.size()));
  CHECK(src->read(0, raw.size()) == raw);
  CHECK(src->read(40 * 4096 + 3, 10) == Bytes(10, 0xAB));
  CHECK_THROWS(src->read(raw.size() - 2, 4), FormatError);
  auto fs = open_filesystem(src);
  CHECK(fs->read_file("/f", 1 << 20) == content);

  Bytes broken = sparse;
  put32(broken, 16, load_u32(&broken[16]) + 1);  // wrong block count
  CHECK_THROWS(std::make_shared<SparseSource>(mem(broken)), FormatError);
}

TEST(super_image_maps_logical_partitions) {
  ErofsBuilder b;
  Bytes content = random_bytes(30000, 41);
  b.file(b.root(), "data.bin", content);
  Bytes part = b.finish();
  Bytes super = make_super(part, "system_a");
  auto src = mem(super);
  CHECK(is_super_image(*src));
  auto parts = read_super_partitions(*src);
  CHECK_EQ(parts.size(), size_t(1));
  CHECK_EQ(parts[0].name, std::string("system_a"));
  CHECK_EQ(parts[0].group, std::string("default"));
  auto view = std::make_shared<ExtentSource>(src, parts[0].extents);
  CHECK(view->read(0, part.size()) == part);
  CHECK(open_filesystem(view)->read_file("/data.bin", 1 << 20) == content);

  // On disk, sparse: open_image() unwraps both layers and picks system_a.
  auto path = temp_path("super.img");
  write_file(path, to_sparse(super));
  {
    OpenedImage img = open_image(path);
    CHECK_EQ(img.layers.size(), size_t(2));
    CHECK_EQ(img.layers[1], std::string("super:system_a"));
    CHECK(open_filesystem(img.source)->read_file("/data.bin", 1 << 20) == content);
    CHECK_THROWS(open_image(path, "vendor"), FormatError);
  }  // closes the file before removing it

  Bytes corrupt = super;
  corrupt[12288 + 128 + 3] ^= 1;  // tables checksum
  CHECK_THROWS(read_super_partitions(*mem(corrupt)), FormatError);
  std::filesystem::remove(path);
}

// --- APEX and boot class path ---------------------------------------------------------------------------

namespace {

Bytes make_apex(const std::string& name, int64_t version, const Bytes& payload, bool compressed) {
  tool::ZipWriter z;
  z.add("apex_manifest.pb", apex_manifest_pb(name, version), false);
  z.add("apex_payload.img", payload, false);
  z.add("apex_pubkey", bytes("clé"), false);
  Bytes apex = z.finish();
  if (!compressed) return apex;
  tool::ZipWriter c;
  c.add("apex_manifest.pb", apex_manifest_pb(name, version), false);
  c.add("original_apex", apex, true);
  return c.finish();
}

struct SystemFixture {
  Bytes core_oj = jar_with_dex("core-oj");
  Bytes core_libart = jar_with_dex("core-libart");
  Bytes framework = jar_with_dex("framework");
  Bytes icu4j = jar_with_dex("icu4j");
  Bytes debug_oj = jar_with_dex("core-oj-debug");
  Bytes icudata = pattern_bytes(50000, "icu ");
  Bytes system;

  explicit SystemFixture(const std::string& evil_path = {}) {
    // ART module: ext4 payload.
    Ext4Builder art(256);
    uint32_t jl = art.mkdir(art.root(), "javalib");
    art.file(jl, "core-oj.jar", core_oj);
    art.file(jl, "core-libart.jar", core_libart);
    uint32_t etc = art.mkdir(art.root(), "etc");
    uint32_t cp = art.mkdir(etc, "classpaths");
    std::vector<std::pair<std::string, int>> art_jars = {{"/apex/com.android.art/javalib/core-oj.jar", 1},
                                                         {"/apex/com.android.art/javalib/core-libart.jar", 1},
                                                         {"/apex/com.android.art/javalib/core-oj.jar", 2}};
    if (!evil_path.empty()) art_jars.push_back({evil_path, 1});
    art.file(cp, "bootclasspath.pb", classpaths_pb(art_jars));
    Bytes art_apex = make_apex("com.android.art", 370000000, art.finish(), false);
    // Debug build of the same module: must lose against the release build.
    Ext4Builder dbg(128);
    uint32_t djl = dbg.mkdir(dbg.root(), "javalib");
    dbg.file(djl, "core-oj.jar", debug_oj);
    uint32_t detc = dbg.mkdir(dbg.root(), "etc");
    uint32_t dcp = dbg.mkdir(detc, "classpaths");
    dbg.file(dcp, "bootclasspath.pb", classpaths_pb({{"/apex/com.android.art/javalib/core-oj.jar", 1}}));
    Bytes dbg_apex = make_apex("com.android.art", 370000001, dbg.finish(), false);
    // i18n module: compressed APEX with an EROFS payload and ICU data.
    ErofsBuilder i18n;
    uint64_t ijl = i18n.mkdir(i18n.root(), "javalib");
    i18n.file(ijl, "core-icu4j.jar", icu4j);
    // As on Android 17, the i18n jar is declared by /system's list, not by the module.
    uint64_t ietc = i18n.mkdir(i18n.root(), "etc");
    uint64_t icu = i18n.mkdir(ietc, "icu");
    ZSpec z;
    z.starts = {0, 20000};
    z.kinds = "zz";
    z.compact = true;
    i18n.compressed(icu, "icudt77l.dat", icudata, z);
    Bytes i18n_apex = make_apex("com.android.i18n", 1, i18n.finish(), true);

    ErofsBuilder s;
    uint64_t sys = s.mkdir(s.root(), "system");
    s.file(sys, "build.prop",
           bytes("# commentaire\nro.system.build.fingerprint=aosp/test:17/AA1/1:userdebug/test-keys\r\n"
                 "ro.system.build.version.sdk = 37\nro.system.build.version.release=17\n"));
    uint64_t setc = s.mkdir(sys, "etc");
    uint64_t scp = s.mkdir(setc, "classpaths");
    s.file(scp, "bootclasspath.pb",
           classpaths_pb({{"/system/framework/framework.jar", 1},
                          {"/apex/com.android.i18n/javalib/core-icu4j.jar", 1},
                          {"/system/framework/services.jar", 3}}));
    uint64_t fw = s.mkdir(sys, "framework");
    s.file(fw, "framework.jar", framework);
    uint64_t apex = s.mkdir(sys, "apex");
    s.file(apex, "com.android.art.apex", art_apex);
    s.file(apex, "com.android.art.debug.apex", dbg_apex);
    s.file(apex, "com.android.i18n.capex", i18n_apex);
    s.file(apex, "notes.txt", bytes("pas un APEX"));
    system = s.finish();
  }
};

}  // namespace

TEST(apex_opens_plain_and_compressed_packages) {
  ErofsBuilder p;
  p.file(p.root(), "x", bytes("payload"));
  Bytes payload = p.finish();
  for (bool compressed : {false, true}) {
    Apex a = open_apex(make_apex("com.android.test", 42, payload, compressed));
    CHECK_EQ(a.manifest.name, std::string("com.android.test"));
    CHECK_EQ(a.manifest.version, int64_t(42));
    CHECK_EQ(a.compressed, compressed);
    CHECK_EQ(a.payload_fs, std::string("erofs"));
    CHECK_EQ(str(a.fs->read_file("/x", 100)), std::string("payload"));
  }
  CHECK_THROWS(parse_apex_manifest(apex_manifest_pb("../evil", 1)), FormatError);
  tool::ZipWriter z;
  z.add("apex_manifest.pb", apex_manifest_pb("com.android.test", 1), false);
  CHECK_THROWS(open_apex(z.finish()), FormatError);  // no payload
}

TEST(boot_classpath_extraction_from_system_image) {
  SystemFixture fx;
  SystemImage sys(open_filesystem(mem(fx.system)));
  CHECK_EQ(sys.prop("ro.system.build.version.sdk"), std::string("37"));
  CHECK_EQ(sys.fs_path("/system/framework/framework.jar"), std::string("/system/framework/framework.jar"));
  CHECK_EQ(sys.fs_path("/vendor/x"), std::string());

  auto out = temp_path("bcp");
  std::filesystem::remove_all(out);
  BootClasspathExtraction r = extract_boot_classpath(sys, out);
  std::vector<std::string> order;
  for (const auto& j : r.jars) order.push_back(j.device_path);
  std::vector<std::string> expected = {"/apex/com.android.art/javalib/core-oj.jar",
                                       "/apex/com.android.art/javalib/core-libart.jar", "/system/framework/framework.jar",
                                       "/apex/com.android.i18n/javalib/core-icu4j.jar"};
  CHECK(order == expected);
  CHECK(read_file(out / "apex/com.android.art/javalib/core-oj.jar", 1 << 20) == fx.core_oj);  // release build wins
  CHECK(read_file(out / "system/framework/framework.jar", 1 << 20) == fx.framework);
  CHECK(read_file(out / "apex/com.android.i18n/javalib/core-icu4j.jar", 1 << 20) == fx.icu4j);
  CHECK_EQ(r.data.size(), size_t(1));
  CHECK(read_file(out / "apex/com.android.i18n/etc/icu/icudt77l.dat", 1 << 20) == fx.icudata);
  CHECK_EQ(r.jars[0].sha256, hash::to_hex(hash::sha256(fx.core_oj)));
  CHECK_EQ(r.jars[3].origin, std::string("com.android.i18n.capex"));
  CHECK_EQ(r.props["ro.system.build.fingerprint"], std::string("aosp/test:17/AA1/1:userdebug/test-keys"));
  CHECK(r.warnings.empty());

  auto list = read_boot_classpath_list(out);
  CHECK_EQ(list.size(), size_t(4));
  CHECK(list[2].file == out / "system/framework/framework.jar");
  CHECK_EQ(list[2].device_path, std::string("/system/framework/framework.jar"));
  std::filesystem::remove_all(out);
}

TEST(boot_classpath_extraction_refuses_path_traversal) {
  SystemFixture fx("/apex/com.android.art/../../../evil.jar");
  SystemImage sys(open_filesystem(mem(fx.system)));
  auto out = temp_path("bcp-evil");
  std::filesystem::remove_all(out);
  CHECK_THROWS(extract_boot_classpath(sys, out), FormatError);
  CHECK(!std::filesystem::exists(out.parent_path() / "evil.jar"));
  std::filesystem::remove_all(out);
}

// --- Virtual file system (ADR-005) ------------------------------------------------------------------

namespace {

std::filesystem::path fresh_dir(const std::string& name) {
  auto p = temp_path(name);
  std::filesystem::remove_all(p);
  return p;
}

int32_t vfs_errno(const std::function<void()>& f) {
  try {
    f();
  } catch (const vfs::VfsError& e) {
    return e.err;
  }
  return 0;
}

}  // namespace

TEST(vfs_normalizes_paths_and_encodes_host_names) {
  CHECK_EQ(vfs::Vfs::normalize("/a/./b/../c//d/"), std::string("/a/c/d"));
  CHECK_EQ(vfs::Vfs::normalize("x/y", "/data"), std::string("/data/x/y"));
  CHECK_EQ(vfs::Vfs::normalize("/../../etc"), std::string("/etc"));
  CHECK_EQ(vfs_errno([] { vfs::Vfs::normalize(std::string("/a\0b", 4)); }), vfs::kEINVAL);
  CHECK_EQ(vfs_errno([] { vfs::Vfs::normalize("/" + std::string(300, 'x')); }), vfs::kENAMETOOLONG);
  CHECK_EQ(vfs::encode_host_name("a:b"), std::string("a%3Ab"));
  CHECK_EQ(vfs::encode_host_name("CON"), std::string("%43ON"));
  CHECK_EQ(vfs::encode_host_name("nul.txt"), std::string("%6Eul.txt"));
  CHECK_EQ(vfs::encode_host_name("fin."), std::string("fin%2E"));
  CHECK_EQ(vfs::encode_host_name("100%"), std::string("100%25"));
  for (std::string n : {"a:b", "CON", "nul.txt", "fin.", "100%", "x?y*z", "é<>|", "normal.txt", "com7.log"}) {
    CHECK_EQ(vfs::decode_host_name(vfs::encode_host_name(n)), n);
  }
}

TEST(vfs_sandboxed_data_areas) {
  auto root = fresh_dir("vfs-data");
  vfs::LayoutOptions lo;
  lo.data_root = root;
  lo.package = "com.example.app";
  auto v = vfs::make_android_vfs(lo);

  v->write_file("/data/local/tmp/a.txt", bytes("bonjour"));
  CHECK_EQ(str(v->read_file("/data/local/tmp/a.txt")), std::string("bonjour"));
  CHECK(std::filesystem::exists(root / "com.example.app" / "tmp" / "a.txt"));
  // Both Android paths of the private directory reach the same storage.
  v->write_file("/data/data/com.example.app/prefs.xml", bytes("<map/>"));
  CHECK_EQ(str(v->read_file("/data/user/0/com.example.app/prefs.xml")), std::string("<map/>"));
  // Linux names Windows refuses are stored encoded and listed decoded.
  v->write_file("/data/local/tmp/aux:1?", bytes("x"));
  auto names = v->list("/data/local/tmp");
  CHECK(std::find(names.begin(), names.end(), "aux:1?") != names.end());
  CHECK(std::filesystem::exists(root / "com.example.app" / "tmp" / "aux%3A1%3F"));

  // Descriptors: append, pread/pwrite, lseek, truncate.
  int32_t fd = v->open("/data/local/tmp/b.bin", vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_EXCL, 0600);
  CHECK(fd >= 3);
  Bytes data = bytes("0123456789");
  CHECK_EQ(v->write(fd, data), size_t(10));
  CHECK_EQ(v->lseek(fd, 2, 0), int64_t(2));
  Bytes two(2);
  CHECK_EQ(v->read(fd, two), size_t(2));
  CHECK_EQ(str(two), std::string("23"));
  CHECK_EQ(v->pwrite(fd, bytes("AB"), 8), size_t(2));
  CHECK_EQ(v->pread(fd, two, 8), size_t(2));
  CHECK_EQ(str(two), std::string("AB"));
  v->ftruncate(fd, 4);
  CHECK_EQ(v->fstat(fd).size, uint64_t(4));
  v->close(fd);
  CHECK_EQ(vfs_errno([&] { v->close(fd); }), vfs::kEBADF);
  CHECK_EQ(vfs_errno([&] { v->open("/data/local/tmp/b.bin", vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_EXCL, 0600); }),
           vfs::kEEXIST);
  int32_t ap = v->open("/data/local/tmp/b.bin", vfs::kO_WRONLY | vfs::kO_APPEND, 0);
  v->write(ap, bytes("!"));
  v->close(ap);
  CHECK_EQ(str(v->read_file("/data/local/tmp/b.bin")), std::string("0123!"));

  // dup and dup2 share the open file description: one offset, one file. The
  // copy takes the lowest free descriptor, and closing it leaves the original open.
  int32_t src = v->open("/data/local/tmp/b.bin", vfs::kO_RDWR, 0);
  int32_t copy = v->dup(src);
  CHECK_EQ(copy, src + 1);
  CHECK_EQ(v->read(src, two), size_t(2));
  CHECK_EQ(v->lseek(copy, 0, 1), int64_t(2));
  CHECK_EQ(v->read(copy, two), size_t(2));
  CHECK_EQ(str(two), std::string("23"));
  v->close(copy);
  CHECK_EQ(v->lseek(src, 0, 1), int64_t(4));
  int32_t other = v->open("/data/local/tmp/a.txt", vfs::kO_RDONLY, 0);
  CHECK_EQ(other, copy);  // the freed number is reused
  v->dup2(src, other);
  CHECK_EQ(v->fstat(other).size, uint64_t(5));
  CHECK_EQ(v->lseek(other, 1, 0), int64_t(1));
  CHECK_EQ(v->lseek(src, 0, 1), int64_t(1));
  CHECK_EQ(vfs_errno([&] { v->dup2(src, 2); }), vfs::kEBADF);
  CHECK_EQ(vfs_errno([&] { v->dup(other + 1); }), vfs::kEBADF);
  v->close(other);
  v->close(src);

  // Directories.
  v->mkdir("/data/local/tmp/d", 0700);
  CHECK_EQ(vfs_errno([&] { v->mkdir("/data/local/tmp/d", 0700); }), vfs::kEEXIST);
  v->write_file("/data/local/tmp/d/f", bytes("1"));
  CHECK_EQ(vfs_errno([&] { v->rmdir("/data/local/tmp/d"); }), vfs::kENOTEMPTY);
  v->rename("/data/local/tmp/d/f", "/data/local/tmp/g");
  v->rmdir("/data/local/tmp/d");
  CHECK(!v->exists("/data/local/tmp/d"));
  CHECK(v->exists("/data/local/tmp/g"));
  CHECK_EQ(vfs_errno([&] { v->rename("/data/local/tmp/g", "/sdcard/g"); }), vfs::kEXDEV);  // other mount
  CHECK_EQ(vfs_errno([&] { v->read_file("/data/local/tmp/nope"); }), vfs::kENOENT);
  // ".." cannot leave the virtual root, hence never the sandbox.
  CHECK_EQ(vfs_errno([&] { v->write_file("/data/local/tmp/../../../../evil", bytes("x")); }), vfs::kEROFS);
  CHECK(!std::filesystem::exists(root / "evil"));

  // /dev.
  Bytes buf(16, 0);
  int32_t z = v->open("/dev/null", vfs::kO_RDWR, 0);
  CHECK_EQ(v->read(z, buf), size_t(0));
  CHECK_EQ(v->write(z, data), data.size());
  v->close(z);
  int32_t rnd = v->open("/dev/urandom", vfs::kO_RDONLY, 0);
  CHECK_EQ(v->read(rnd, buf), size_t(16));
  CHECK(std::any_of(buf.begin(), buf.end(), [](uint8_t c) { return c != 0; }));
  v->close(rnd);
  std::filesystem::remove_all(root);
}

// Android's SELinux policy lets an application create symbolic links in its
// data (app_data_file:lnk_file create_file_perms) but grants no `link` on its
// files: hard links fail with EACCES once the kernel's checks pass.
TEST(vfs_symbolic_and_hard_links_in_host_areas) {
  auto root = fresh_dir("vfs-links");
  vfs::LayoutOptions lo;
  lo.data_root = root;
  lo.package = "com.example.app";
  auto v = vfs::make_android_vfs(lo);
  const std::string d = "/data/data/com.example.app/";

  v->write_file(d + "cible.txt", bytes("contenu"));
  v->symlink("cible.txt", d + "rel");
  v->symlink(d + "sub", d + "abs");  // dangling until sub exists
  vfs::VStat l = v->stat(d + "rel", false);
  CHECK(l.type == vfs::NodeType::kSymlink);
  CHECK_EQ(l.mode, uint32_t(0120777));
  CHECK_EQ(l.size, uint64_t(9));
  CHECK_EQ(v->readlink(d + "rel"), std::string("cible.txt"));
  CHECK_EQ(str(v->read_file(d + "rel")), std::string("contenu"));
  CHECK(v->stat(d + "rel").type == vfs::NodeType::kFile);
  CHECK_EQ(vfs_errno([&] { v->stat(d + "abs"); }), vfs::kENOENT);
  v->mkdir(d + "sub", 0700);
  v->write_file(d + "abs/f", bytes("par le lien"));  // a link inside a path
  CHECK_EQ(str(v->read_file(d + "sub/f")), std::string("par le lien"));
  auto names = v->list(d);
  CHECK(std::find(names.begin(), names.end(), "rel") != names.end());
  CHECK(std::find(names.begin(), names.end(), "abs") != names.end());
  CHECK_EQ(vfs_errno([&] { v->readlink(d + "cible.txt"); }), vfs::kEINVAL);

  CHECK_EQ(vfs_errno([&] { v->symlink("x", d + "rel"); }), vfs::kEEXIST);
  CHECK_EQ(vfs_errno([&] { v->symlink("x", d + "abs"); }), vfs::kEEXIST);
  CHECK_EQ(vfs_errno([&] { v->symlink("", d + "vide"); }), vfs::kENOENT);
  CHECK_EQ(vfs_errno([&] { v->symlink(std::string(4096, 'x'), d + "long"); }), vfs::kENAMETOOLONG);
  CHECK_EQ(vfs_errno([&] { v->symlink("x", d + "absent/l"); }), vfs::kENOENT);
  CHECK_EQ(vfs_errno([&] { v->symlink("x", d + "cible.txt/l"); }), vfs::kENOTDIR);
  CHECK_EQ(vfs_errno([&] { v->symlink("x", "/data"); }), vfs::kEEXIST);
  CHECK_EQ(vfs_errno([&] { v->open(d + "rel", vfs::kO_RDONLY | vfs::kO_NOFOLLOW, 0); }), vfs::kELOOP);
  CHECK_EQ(vfs_errno([&] { v->open(d + "abs", vfs::kO_WRONLY | vfs::kO_CREAT | vfs::kO_EXCL, 0600); }), vfs::kEEXIST);
  v->symlink("boucle2", d + "boucle1");
  v->symlink("boucle1", d + "boucle2");
  CHECK_EQ(vfs_errno([&] { v->read_file(d + "boucle1"); }), vfs::kELOOP);

  // rename and unlink act on the link, never on its target.
  v->rename(d + "rel", d + "rel2");
  CHECK_EQ(v->readlink(d + "rel2"), std::string("cible.txt"));
  v->unlink(d + "rel2");
  CHECK(!v->exists(d + "rel2"));
  CHECK_EQ(str(v->read_file(d + "cible.txt")), std::string("contenu"));
  CHECK_EQ(vfs_errno([&] { v->rmdir(d + "abs"); }), vfs::kENOTDIR);
  // A host file holding the marker is not a link without the host attribute.
  v->write_file(d + "faux", bytes("!<symlink>/etc"));
  CHECK(v->stat(d + "faux", false).type == vfs::NodeType::kFile);

  CHECK_EQ(vfs_errno([&] { v->link(d + "cible.txt", d + "dur"); }), vfs::kEACCES);
  CHECK_EQ(vfs_errno([&] { v->link(d + "absent", d + "dur"); }), vfs::kENOENT);
  CHECK_EQ(vfs_errno([&] { v->link(d + "cible.txt", d + "abs"); }), vfs::kEEXIST);
  CHECK_EQ(vfs_errno([&] { v->link(d + "sub", d + "dur"); }), vfs::kEPERM);
  CHECK_EQ(vfs_errno([&] { v->link(d + "cible.txt", "/sdcard/dur"); }), vfs::kEXDEV);
  CHECK(!v->exists(d + "dur"));
  std::filesystem::remove_all(root);
}

// SQLite, AtomicFile and SharedPreferences delete or replace files that are
// still open, as Linux allows: the name changes at once, open descriptors keep
// the file they had. Windows's default (delete pending until the last handle
// closes) would keep the name taken and make the next create fail.
namespace {

void open_files_scenario(bool posix) {
  vfs::use_posix_file_semantics(posix);
  struct Restore {
    ~Restore() { vfs::use_posix_file_semantics(true); }
  } restore;
  auto root = fresh_dir(posix ? "vfs-posix" : "vfs-posix-fallback");
  vfs::LayoutOptions lo;
  lo.data_root = root;
  lo.package = "com.example.app";
  auto v = vfs::make_android_vfs(lo);
  const std::string d = "/data/local/tmp/";
  auto pread_all = [&](int32_t fd) {
    Bytes buf(64);
    buf.resize(v->pread(fd, buf, 0));
    return str(buf);
  };
  auto listed = [&](const std::string& name) {
    auto names = v->list(d);
    return std::find(names.begin(), names.end(), name) != names.end();
  };

  // unlink of an open file: the name is free at once, the descriptor still reads.
  v->write_file(d + "journal", bytes("ancien"));
  int32_t fd = v->open(d + "journal", vfs::kO_RDONLY, 0);
  v->unlink(d + "journal");
  CHECK(!v->exists(d + "journal"));
  CHECK(!listed("journal"));
  v->write_file(d + "journal", bytes("nouveau"));
  CHECK_EQ(pread_all(fd), std::string("ancien"));
  v->close(fd);
  CHECK_EQ(str(v->read_file(d + "journal")), std::string("nouveau"));

  // rename over an open target (AtomicFile): the path shows the new content,
  // the old descriptor the old one.
  v->write_file(d + "prefs.xml", bytes("v1"));
  int32_t old = v->open(d + "prefs.xml", vfs::kO_RDONLY, 0);
  v->write_file(d + "prefs.xml.new", bytes("v2"));
  v->rename(d + "prefs.xml.new", d + "prefs.xml");
  CHECK_EQ(str(v->read_file(d + "prefs.xml")), std::string("v2"));
  CHECK(!v->exists(d + "prefs.xml.new"));
  CHECK_EQ(pread_all(old), std::string("v1"));
  v->close(old);
  CHECK_EQ(str(v->read_file(d + "prefs.xml")), std::string("v2"));

  // rename of an open source: writes through the descriptor reach the new name.
  int32_t w = v->open(d + "db", vfs::kO_RDWR | vfs::kO_CREAT, 0600);
  v->write(w, bytes("abc"));
  v->rename(d + "db", d + "db2");
  v->write(w, bytes("def"));
  v->close(w);
  CHECK_EQ(str(v->read_file(d + "db2")), std::string("abcdef"));
  CHECK(!v->exists(d + "db"));

  // The other rules of rename(2).
  v->rename(d + "db2", d + "db2");  // same name: nothing to do
  CHECK_EQ(str(v->read_file(d + "db2")), std::string("abcdef"));
  v->mkdir(d + "dir", 0700);
  v->mkdir(d + "empty", 0700);
  v->mkdir(d + "full", 0700);
  v->write_file(d + "full/x", bytes("x"));
  CHECK_EQ(vfs_errno([&] { v->rename(d + "db2", d + "dir"); }), vfs::kEISDIR);
  CHECK_EQ(vfs_errno([&] { v->rename(d + "dir", d + "db2"); }), vfs::kENOTDIR);
  CHECK_EQ(vfs_errno([&] { v->rename(d + "dir", d + "full"); }), vfs::kENOTEMPTY);
  CHECK_EQ(vfs_errno([&] { v->rename(d + "dir", d + "dir/sub"); }), vfs::kEINVAL);
  v->rename(d + "dir", d + "empty");  // an empty directory is replaced
  CHECK(!v->exists(d + "dir"));
  CHECK(v->stat(d + "empty").type == vfs::NodeType::kDirectory);
  // NTFS ignores case: renaming "db2" to "DB2" names the same host file, which
  // must not be taken for a target to replace (and deleted).
  v->rename(d + "db2", d + "DB2");
  CHECK_EQ(str(v->read_file(d + "DB2")), std::string("abcdef"));
  v->rename(d + "DB2", d + "db2");

  // Nothing left behind on the host once every descriptor is closed. The
  // fallback's hidden directory may remain, empty (also in POSIX mode where the
  // system lacks it: wine 9 refuses FileDispositionInfoEx and FileRenameInfoEx).
  std::vector<std::string> host_names;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root / "com.example.app" / "tmp")) {
    std::string name = e.path().filename().string();
    if (name.starts_with("%%")) {
      CHECK(std::filesystem::is_empty(e.path()));
    } else {
      host_names.push_back(name);
    }
  }
  std::sort(host_names.begin(), host_names.end());
  std::string joined;
  for (const auto& n : host_names) joined += n + " ";
  CHECK_EQ(joined, std::string("db2 empty full journal prefs.xml x "));
  CHECK(!listed("%%corbeille"));
  std::filesystem::remove_all(root);
}

}  // namespace

TEST(vfs_unlink_and_rename_of_open_files) { open_files_scenario(true); }

TEST(vfs_unlink_and_rename_of_open_files_fallback) { open_files_scenario(false); }

// ADR-006 J2: the application's APK under /data/app/<package>, read-only; the
// package name (from an untrusted manifest) is a path component, so it must be
// a Java package name.
TEST(vfs_serves_the_apk_and_checks_the_package_name) {
  auto root = fresh_dir("vfs-app");
  vfs::LayoutOptions lo;
  lo.data_root = root;
  lo.package = "com.example.app";
  lo.app_files["base.apk"] = std::make_shared<const std::vector<uint8_t>>(bytes("PK-apk"));
  lo.app_files["../evil"] = std::make_shared<const std::vector<uint8_t>>(bytes("x"));  // dropped
  auto v = vfs::make_android_vfs(lo);
  CHECK_EQ(str(v->read_file("/data/app/com.example.app/base.apk")), std::string("PK-apk"));
  vfs::VStat st = v->stat("/data/app/com.example.app/base.apk");
  CHECK(st.type == vfs::NodeType::kFile);
  CHECK_EQ(st.size, uint64_t(6));
  CHECK(v->stat("/data/app/com.example.app").type == vfs::NodeType::kDirectory);
  auto names = v->list("/data/app/com.example.app");
  CHECK_EQ(names.size(), size_t(1));
  CHECK_EQ(vfs_errno([&] { v->open("/data/app/com.example.app/base.apk", vfs::kO_RDWR, 0); }), vfs::kEROFS);
  CHECK_EQ(vfs_errno([&] { v->write_file("/data/app/com.example.app/other.apk", bytes("x")); }), vfs::kEROFS);
  CHECK_EQ(vfs_errno([&] { v->read_file("/data/app/com.example.app/absent.apk"); }), vfs::kENOENT);

  for (std::string bad : {"", "../../system", "a/b", "a..b", ".a", "a.", "1a", "a.1b", "a b", "a\\b"}) {
    lo.package = bad;
    bool refused = false;
    try {
      vfs::make_android_vfs(lo);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    CHECK(refused);
  }
  for (std::string good : {"exec", "com.example.app", "a_b.C1"}) {
    lo.package = good;
    CHECK(vfs::make_android_vfs(lo) != nullptr);
  }
  std::filesystem::remove_all(root);
}

TEST(vfs_serves_the_system_image_and_apex_modules) {
  // APEX module with an EROFS payload.
  ErofsBuilder payload;
  uint64_t etc = payload.mkdir(payload.root(), "etc");
  payload.file(etc, "hello.txt", bytes("depuis l'APEX"));
  Bytes apex = make_apex("com.android.demo", 1, payload.finish(), true);

  Ext4Builder img(1024);
  uint32_t sys = img.mkdir(img.root(), "system");
  img.file(sys, "build.prop", bytes("ro.system.build.version.sdk=37\n"));
  uint32_t setc = img.mkdir(sys, "etc");
  img.file(setc, "hosts", bytes("127.0.0.1 localhost\n"));
  uint32_t sapex = img.mkdir(sys, "apex");
  img.file(sapex, "com.android.demo.capex", apex);
  img.mkdir(img.root(), "data");
  img.mkdir(img.root(), "apex");
  img.symlink(img.root(), "etc", "/system/etc");
  img.symlink(img.root(), "sdcard", "/storage/emulated/0");
  img.symlink(sys, "boucle", "/system/boucle");

  auto system = std::make_shared<SystemImage>(open_filesystem(mem(img.finish())));
  auto root = fresh_dir("vfs-image");
  vfs::LayoutOptions lo;
  lo.system = system;
  lo.data_root = root;
  auto v = vfs::make_android_vfs(lo);

  CHECK_EQ(str(v->read_file("/system/build.prop")), std::string("ro.system.build.version.sdk=37\n"));
  CHECK_EQ(str(v->read_file("/etc/hosts")), std::string("127.0.0.1 localhost\n"));  // link inside the image
  CHECK_EQ(str(v->read_file("/apex/com.android.demo/etc/hello.txt")), std::string("depuis l'APEX"));
  CHECK_EQ(vfs_errno([&] { v->read_file("/apex/com.android.absent/x"); }), vfs::kENOENT);
  CHECK(v->stat("/system").type == vfs::NodeType::kDirectory);
  CHECK(v->stat("/etc", false).type == vfs::NodeType::kSymlink);
  CHECK_EQ(v->readlink("/etc"), std::string("/system/etc"));
  // A link of the image leading into a writable mount.
  v->write_file("/sdcard/note.txt", bytes("ok"));
  CHECK_EQ(str(v->read_file("/storage/emulated/0/note.txt")), std::string("ok"));
  // The image is read-only.
  CHECK_EQ(vfs_errno([&] { v->write_file("/system/build.prop", bytes("x")); }), vfs::kEROFS);
  CHECK_EQ(vfs_errno([&] { v->unlink("/system/build.prop"); }), vfs::kEROFS);
  CHECK_EQ(vfs_errno([&] { v->read_file("/system/boucle"); }), vfs::kELOOP);
  // Mount points show up in their parent directory.
  auto data = v->list("/data");
  CHECK(std::find(data.begin(), data.end(), "local") == data.end());  // /data/local itself is not a mount
  auto local = v->list("/data/local");
  CHECK(std::find(local.begin(), local.end(), "tmp") != local.end());
  std::filesystem::remove_all(root);
}
