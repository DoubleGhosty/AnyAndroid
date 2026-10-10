// Read-only ext4 reader for Android images (system.img, APEX payloads).
//
// Supports what Android's build produces: extents and block maps, 32/64-bit
// group descriptors, flex_bg, meta_bg, sparse_super(2), hashed directories
// (read linearly) and shared (deduplicated) blocks. Inline data, encryption
// and compression are refused explicitly.
#include <algorithm>
#include <cstring>
#include <functional>

#include "common/bytes.h"
#include "image/filesystem.h"

namespace aa::image {

namespace {

constexpr uint32_t kIncompatFiletype = 0x2;
constexpr uint32_t kIncompatRecover = 0x4;
constexpr uint32_t kIncompatMetaBg = 0x10;
constexpr uint32_t kIncompatExtents = 0x40;
constexpr uint32_t kIncompat64Bit = 0x80;
constexpr uint32_t kIncompatMmp = 0x100;
constexpr uint32_t kIncompatFlexBg = 0x200;
constexpr uint32_t kIncompatCsumSeed = 0x2000;
constexpr uint32_t kIncompatLargeDir = 0x4000;
constexpr uint32_t kIncompatInlineData = 0x8000;
constexpr uint32_t kSupportedIncompat = kIncompatFiletype | kIncompatRecover | kIncompatMetaBg | kIncompatExtents |
                                        kIncompat64Bit | kIncompatMmp | kIncompatFlexBg | kIncompatCsumSeed |
                                        kIncompatLargeDir | kIncompatInlineData;
constexpr uint32_t kRoCompatSparseSuper = 0x1;
constexpr uint32_t kCompatSparseSuper2 = 0x200;

constexpr uint32_t kFlagExtents = 0x80000;
constexpr uint32_t kFlagInlineData = 0x10000000;

FileType type_of_mode(uint16_t mode) {
  switch (mode & 0xF000) {
    case 0x8000: return FileType::kRegular;
    case 0x4000: return FileType::kDirectory;
    case 0xA000: return FileType::kSymlink;
    default: return FileType::kOther;
  }
}

bool is_power_of(uint32_t n, uint32_t base) {
  while (n > 1 && n % base == 0) n /= base;
  return n == 1;
}

class Ext4 final : public FileSystem {
 public:
  explicit Ext4(std::shared_ptr<const Source> src) : src_(std::move(src)) {
    auto sb = src_->read(1024, 1024);
    ByteReader r(sb);
    inodes_count_ = r.u32_at(0x0);
    uint64_t blocks_lo = r.u32_at(0x4);
    first_data_block_ = r.u32_at(0x14);
    uint32_t log_block = r.u32_at(0x18);
    blocks_per_group_ = r.u32_at(0x20);
    inodes_per_group_ = r.u32_at(0x28);
    uint32_t rev = r.u32_at(0x4C);
    inode_size_ = rev >= 1 ? r.u16_at(0x58) : 128;
    compat_ = r.u32_at(0x5C);
    incompat_ = r.u32_at(0x60);
    ro_compat_ = r.u32_at(0x64);
    first_meta_bg_ = r.u32_at(0x104);
    backup_bgs_[0] = r.u32_at(0x24C);
    backup_bgs_[1] = r.u32_at(0x250);
    if (log_block > 6) throw_format("ext4 : taille de bloc invalide");
    block_size_ = 1024u << log_block;
    blocks_count_ = blocks_lo;
    desc_size_ = 32;
    if (incompat_ & kIncompat64Bit) {
      blocks_count_ |= uint64_t(r.u32_at(0x150)) << 32;
      desc_size_ = r.u16_at(0xFE);
      if (desc_size_ < 32 || desc_size_ > block_size_ || (desc_size_ & (desc_size_ - 1))) {
        throw_format("ext4 : taille de descripteur de groupe invalide");
      }
    }
    if (uint32_t unknown = incompat_ & ~kSupportedIncompat) {
      throw_format("ext4 : fonctionnalités incompatibles non prises en charge (" + hex(unknown) + ")");
    }
    if (inode_size_ < 128 || inode_size_ > block_size_ || (inode_size_ & (inode_size_ - 1))) {
      throw_format("ext4 : taille d'inode invalide");
    }
    if (blocks_per_group_ == 0 || inodes_per_group_ == 0 || inodes_count_ == 0) throw_format("ext4 : géométrie invalide");
    groups_ = uint32_t((blocks_count_ - first_data_block_ + blocks_per_group_ - 1) / blocks_per_group_);
    if (uint64_t(groups_) * inodes_per_group_ < inodes_count_) throw_format("ext4 : nombre d'inodes incohérent");
    if (blocks_count_ * block_size_ > src_->size()) throw_format("ext4 : image tronquée");
  }

  std::string kind() const override { return "ext4"; }
  uint64_t root() const override { return 2; }

  Stat stat_inode(uint64_t ino) const override {
    Inode in = load(ino);
    return Stat{ino, type_of_mode(in.mode), in.size, in.mode};
  }

  std::vector<DirEntry> read_dir(uint64_t ino) const override {
    Inode in = load(ino);
    if (type_of_mode(in.mode) != FileType::kDirectory) throw_format("ext4 : l'inode " + std::to_string(ino) + " n'est pas un répertoire");
    std::vector<uint8_t> data = read_data(in, 256u << 20);
    std::vector<DirEntry> out;
    for (size_t blk = 0; blk < data.size(); blk += block_size_) {
      size_t end = std::min<size_t>(data.size(), blk + block_size_);
      size_t pos = blk;
      while (pos + 8 <= end) {
        uint32_t child = load_u32(&data[pos]);
        uint16_t rec_len = load_u16(&data[pos + 4]);
        uint8_t name_len = data[pos + 6];
        uint8_t ftype = data[pos + 7];
        if (rec_len < 8 || rec_len % 4 != 0 || pos + rec_len > end || 8u + name_len > rec_len) {
          throw_format("ext4 : entrée de répertoire invalide (inode " + std::to_string(ino) + ")");
        }
        if (child != 0 && name_len > 0) {
          DirEntry e;
          e.name.assign(reinterpret_cast<const char*>(&data[pos + 8]), name_len);
          e.inode = child;
          if (incompat_ & kIncompatFiletype) {
            switch (ftype) {
              case 1: e.type = FileType::kRegular; break;
              case 2: e.type = FileType::kDirectory; break;
              case 7: e.type = FileType::kSymlink; break;
              default: e.type = FileType::kOther;
            }
          }
          if (e.name != "." && e.name != "..") out.push_back(std::move(e));
        }
        pos += rec_len;
      }
    }
    return out;
  }

  std::vector<uint8_t> read_inode(uint64_t ino, uint64_t max_size) const override {
    Inode in = load(ino);
    FileType t = type_of_mode(in.mode);
    if (t == FileType::kSymlink && !(in.flags & (kFlagExtents | kFlagInlineData)) && in.size < 60) {
      return std::vector<uint8_t>(in.block, in.block + in.size);  // fast symlink
    }
    if (t != FileType::kRegular && t != FileType::kSymlink) {
      throw_format("ext4 : l'inode " + std::to_string(ino) + " n'est pas un fichier");
    }
    return read_data(in, max_size);
  }

 private:
  struct Inode {
    uint16_t mode = 0;
    uint32_t flags = 0;
    uint64_t size = 0;
    uint8_t block[60] = {};
  };

  uint64_t group_first_block(uint64_t g) const { return first_data_block_ + g * blocks_per_group_; }

  bool group_has_super(uint64_t g) const {
    if (g == 0) return true;
    if (compat_ & kCompatSparseSuper2) return g == backup_bgs_[0] || g == backup_bgs_[1];
    if (!(ro_compat_ & kRoCompatSparseSuper)) return true;
    if (g == 1) return true;
    return is_power_of(uint32_t(g), 3) || is_power_of(uint32_t(g), 5) || is_power_of(uint32_t(g), 7);
  }

  uint64_t descriptor_offset(uint32_t g) const {
    uint64_t per_block = block_size_ / desc_size_;
    uint64_t meta_group = g / per_block;
    if ((incompat_ & kIncompatMetaBg) && meta_group >= first_meta_bg_) {
      uint64_t first = meta_group * per_block;
      uint64_t blk = group_first_block(first) + (group_has_super(first) ? 1 : 0);
      return blk * block_size_ + (g % per_block) * desc_size_;
    }
    uint64_t gdt = uint64_t(first_data_block_ + 1) * block_size_;
    return gdt + uint64_t(g) * desc_size_;
  }

  Inode load(uint64_t ino) const {
    if (ino == 0 || ino > inodes_count_) throw_format("ext4 : numéro d'inode invalide " + std::to_string(ino));
    uint32_t group = uint32_t((ino - 1) / inodes_per_group_);
    uint32_t index = uint32_t((ino - 1) % inodes_per_group_);
    auto desc = src_->read(descriptor_offset(group), desc_size_);
    uint64_t table = load_u32(&desc[8]);
    if (desc_size_ >= 64) table |= uint64_t(load_u32(&desc[0x28])) << 32;
    if (table >= blocks_count_) throw_format("ext4 : table d'inodes hors du volume");
    auto raw = src_->read(table * block_size_ + uint64_t(index) * inode_size_, 128);
    Inode in;
    in.mode = load_u16(&raw[0]);
    in.size = load_u32(&raw[4]) | (uint64_t(load_u32(&raw[0x6C])) << 32);
    in.flags = load_u32(&raw[0x20]);
    std::memcpy(in.block, &raw[0x28], 60);
    return in;
  }

  // Calls fn(logical_block, physical_block, count) for each mapped run.
  using RunFn = std::function<void(uint64_t, uint64_t, uint64_t)>;

  void walk_extents(const uint8_t* node, size_t node_size, int depth_left, const RunFn& fn) const {
    if (node_size < 12 || load_u16(node) != 0xF30A) throw_format("ext4 : en-tête d'extents invalide");
    uint16_t entries = load_u16(node + 2);
    uint16_t depth = load_u16(node + 6);
    if (12u + entries * 12u > node_size || depth > 5 || depth_left < 0) throw_format("ext4 : arbre d'extents invalide");
    for (uint16_t i = 0; i < entries; ++i) {
      const uint8_t* e = node + 12 + i * 12;
      if (depth == 0) {
        uint64_t logical = load_u32(e);
        uint32_t len = load_u16(e + 4);
        uint64_t start = (uint64_t(load_u16(e + 6)) << 32) | load_u32(e + 8);
        if (len > 32768) continue;  // uninitialized extent: reads as zeros
        fn(logical, start, len);
      } else {
        uint64_t leaf = (uint64_t(load_u16(e + 8)) << 32) | load_u32(e + 4);
        if (leaf >= blocks_count_) throw_format("ext4 : nœud d'extents hors du volume");
        auto child = src_->read(leaf * block_size_, block_size_);
        walk_extents(child.data(), child.size(), depth_left - 1, fn);
      }
    }
  }

  void walk_indirect(uint64_t block, int level, uint64_t& logical, uint64_t limit, const RunFn& fn) const {
    uint64_t per_block = block_size_ / 4;
    uint64_t span = 1;
    for (int i = 0; i < level; ++i) span *= per_block;
    if (block == 0) {
      logical += span * per_block;
      return;
    }
    if (block >= blocks_count_) throw_format("ext4 : bloc indirect hors du volume");
    auto data = src_->read(block * block_size_, block_size_);
    for (uint64_t i = 0; i < per_block && logical < limit; ++i) {
      uint32_t b = load_u32(&data[i * 4]);
      if (level == 0) {
        if (b) fn(logical, b, 1);
        ++logical;
      } else {
        walk_indirect(b, level - 1, logical, limit, fn);
      }
    }
  }

  std::vector<uint8_t> read_data(const Inode& in, uint64_t max_size) const {
    if (in.flags & kFlagInlineData) throw_format("ext4 : données en ligne (inline_data) non prises en charge");
    if (in.size > max_size) {
      throw_format("ext4 : fichier trop volumineux (" + std::to_string(in.size) + " octets, limite " +
                   std::to_string(max_size) + ")");
    }
    std::vector<uint8_t> out(size_t(in.size), 0);
    uint64_t nblocks = (in.size + block_size_ - 1) / block_size_;
    RunFn copy = [&](uint64_t logical, uint64_t physical, uint64_t count) {
      if (logical >= nblocks) return;
      count = std::min(count, nblocks - logical);
      if (physical >= blocks_count_ || count > blocks_count_ - physical) throw_format("ext4 : extent hors du volume");
      uint64_t off = logical * block_size_;
      uint64_t len = std::min<uint64_t>(count * block_size_, in.size - off);
      src_->read(physical * block_size_, std::span(out.data() + off, size_t(len)));
    };
    if (in.flags & kFlagExtents) {
      walk_extents(in.block, sizeof in.block, 5, copy);
    } else {
      uint64_t logical = 0;
      for (int i = 0; i < 12 && logical < nblocks; ++i, ++logical) {
        uint32_t b = load_u32(in.block + 4 * i);
        if (b) copy(logical, b, 1);
      }
      for (int level = 0; level < 3 && logical < nblocks; ++level) {
        walk_indirect(load_u32(in.block + 48 + 4 * level), level, logical, nblocks, copy);
      }
    }
    return out;
  }

  std::shared_ptr<const Source> src_;
  uint32_t block_size_ = 0;
  uint64_t blocks_count_ = 0;
  uint32_t first_data_block_ = 0;
  uint32_t blocks_per_group_ = 0;
  uint32_t inodes_per_group_ = 0;
  uint32_t inodes_count_ = 0;
  uint32_t inode_size_ = 0;
  uint32_t desc_size_ = 32;
  uint32_t groups_ = 0;
  uint32_t compat_ = 0, incompat_ = 0, ro_compat_ = 0;
  uint32_t first_meta_bg_ = 0;
  uint32_t backup_bgs_[2] = {};
};

}  // namespace

std::unique_ptr<FileSystem> open_ext4(std::shared_ptr<const Source> src) { return std::make_unique<Ext4>(std::move(src)); }

}  // namespace aa::image
