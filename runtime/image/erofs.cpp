// Read-only EROFS reader (Enhanced Read-Only File System), used by recent
// Android system images and APEX payloads.
//
// Layouts: flat plain, flat inline (tail packing), chunk-based, and
// compressed files with full (legacy) or compact (2B/4B) indexes, big
// pclusters, zero padding, inline tail pclusters and interlaced plain
// pclusters. Compression: LZ4 / LZ4HC. Fragments (packed inode), LZMA,
// DEFLATE and ZSTD pclusters are refused explicitly.
//
// The index decoding follows the Linux kernel (fs/erofs/zmap.c) closely so
// that the on-disk semantics stay identical.
#include <algorithm>
#include <cstring>

#include "common/bytes.h"
#include "image/filesystem.h"
#include "image/lz4.h"

namespace aa::image {

namespace {

constexpr uint32_t kMagic = 0xE0F5E1E2;

constexpr uint32_t kFeatZeroPadding = 0x1;
constexpr uint32_t kFeatComprCfgs = 0x2;  // also BIG_PCLUSTER
constexpr uint32_t kFeatChunkedFile = 0x4;
constexpr uint32_t kFeatDeviceTable = 0x8;  // also COMPR_HEAD2
constexpr uint32_t kFeatZtailpacking = 0x10;
constexpr uint32_t kFeatFragments = 0x20;  // also DEDUPE
constexpr uint32_t kFeatXattrPrefixes = 0x40;
constexpr uint32_t kSupportedFeatures = kFeatZeroPadding | kFeatComprCfgs | kFeatChunkedFile | kFeatDeviceTable |
                                        kFeatZtailpacking | kFeatFragments | kFeatXattrPrefixes;

enum Layout : uint8_t { kFlatPlain = 0, kCompressedFull = 1, kFlatInline = 2, kCompressedCompact = 3, kChunkBased = 4 };

constexpr uint16_t kAdviseCompacted2B = 0x0001;
constexpr uint16_t kAdviseBigPcluster1 = 0x0002;
constexpr uint16_t kAdviseBigPcluster2 = 0x0004;
constexpr uint16_t kAdviseInlinePcluster = 0x0008;
constexpr uint16_t kAdviseInterlaced = 0x0010;
constexpr uint16_t kAdviseFragment = 0x0020;

enum LclusterType : uint8_t { kPlain = 0, kHead1 = 1, kNonHead = 2, kHead2 = 3 };
constexpr uint32_t kD0Cblkcnt = 1u << 11;
constexpr uint16_t kPartialRef = 1u << 15;

constexpr uint32_t kNullAddr = 0xFFFFFFFF;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

FileType type_of_mode(uint16_t mode) {
  switch (mode & 0xF000) {
    case 0x8000: return FileType::kRegular;
    case 0x4000: return FileType::kDirectory;
    case 0xA000: return FileType::kSymlink;
    default: return FileType::kOther;
  }
}

class Erofs final : public FileSystem {
 public:
  explicit Erofs(std::shared_ptr<const Source> src) : src_(std::move(src)) {
    auto sb = src_->read(1024, 128);
    ByteReader r(sb);
    if (r.u32_at(0) != kMagic) throw_format("EROFS : superbloc invalide");
    blkszbits_ = sb[12];
    if (blkszbits_ < 9 || blkszbits_ > 16) throw_format("EROFS : taille de bloc invalide");
    blksz_ = 1u << blkszbits_;
    root_nid_ = r.u16_at(14);
    meta_base_ = uint64_t(r.u32_at(40)) << blkszbits_;
    features_ = r.u32_at(80);
    if (uint32_t unknown = features_ & ~kSupportedFeatures) {
      throw_format("EROFS : fonctionnalités incompatibles non prises en charge (" + hex(unknown) + ")");
    }
    algs_ = (features_ & kFeatComprCfgs) ? r.u16_at(84) : 1;  // LZ4 only without compression configs
    extra_devices_ = r.u16_at(86);
    if (sb[90] != 0) throw_format("EROFS : taille de bloc de répertoire non prise en charge");
    if (meta_base_ > src_->size()) throw_format("EROFS : zone de métadonnées hors de l'image");
  }

  std::string kind() const override { return "erofs"; }
  uint64_t root() const override { return root_nid_; }

  Stat stat_inode(uint64_t nid) const override {
    Inode in = load(nid);
    return Stat{nid, type_of_mode(in.mode), in.size, in.mode};
  }

  std::vector<DirEntry> read_dir(uint64_t nid) const override {
    Inode in = load(nid);
    if (type_of_mode(in.mode) != FileType::kDirectory) {
      throw_format("EROFS : l'inode " + std::to_string(nid) + " n'est pas un répertoire");
    }
    std::vector<uint8_t> data = read_data(in, 256u << 20);
    std::vector<DirEntry> out;
    for (size_t blk = 0; blk < data.size(); blk += blksz_) {
      size_t len = std::min<size_t>(blksz_, data.size() - blk);
      const uint8_t* b = &data[blk];
      if (len < 12) throw_format("EROFS : bloc de répertoire tronqué");
      uint16_t first = load_u16(b + 8);
      if (first < 12 || first % 12 != 0 || first > len) throw_format("EROFS : bloc de répertoire invalide");
      size_t n = first / 12;
      for (size_t i = 0; i < n; ++i) {
        uint64_t child = load_u32(b + i * 12) | (uint64_t(load_u32(b + i * 12 + 4)) << 32);
        size_t start = load_u16(b + i * 12 + 8);
        uint8_t ftype = b[i * 12 + 10];
        size_t end = i + 1 < n ? load_u16(b + (i + 1) * 12 + 8) : len;
        if (start < first || end > len || start >= end) throw_format("EROFS : nom d'entrée de répertoire invalide");
        if (i + 1 == n) {
          const void* nul = std::memchr(b + start, 0, end - start);
          if (nul) end = size_t(static_cast<const uint8_t*>(nul) - b);
        }
        DirEntry e;
        e.name.assign(reinterpret_cast<const char*>(b + start), end - start);
        e.inode = child;
        switch (ftype) {
          case 1: e.type = FileType::kRegular; break;
          case 2: e.type = FileType::kDirectory; break;
          case 7: e.type = FileType::kSymlink; break;
          default: e.type = FileType::kOther;
        }
        if (e.name != "." && e.name != "..") out.push_back(std::move(e));
      }
    }
    return out;
  }

  std::vector<uint8_t> read_inode(uint64_t nid, uint64_t max_size) const override {
    Inode in = load(nid);
    FileType t = type_of_mode(in.mode);
    if (t != FileType::kRegular && t != FileType::kSymlink) {
      throw_format("EROFS : l'inode " + std::to_string(nid) + " n'est pas un fichier");
    }
    if (in.size > max_size) {
      throw_format("EROFS : fichier trop volumineux (" + std::to_string(in.size) + " octets, limite " +
                   std::to_string(max_size) + ")");
    }
    return read_data(in, max_size);
  }

 private:
  struct Inode {
    uint64_t nid = 0;
    uint64_t pos = 0;  // absolute position of the on-disk inode
    uint8_t layout = 0;
    uint32_t isize = 32;
    uint32_t xattr_isize = 0;
    uint16_t mode = 0;
    uint64_t size = 0;
    uint32_t u = 0;  // raw_blkaddr / compressed_blocks / chunk info
    uint64_t data_end() const { return pos + isize + xattr_isize; }
  };

  Inode load(uint64_t nid) const {
    Inode in;
    in.nid = nid;
    if (nid > (UINT64_MAX >> 6)) throw_format("EROFS : numéro d'inode invalide");
    in.pos = meta_base_ + (nid << 5);
    if (in.pos + 32 > src_->size()) throw_format("EROFS : inode hors de l'image (" + std::to_string(nid) + ")");
    auto raw = src_->read(in.pos, 32);
    uint16_t format = load_u16(&raw[0]);
    bool extended = format & 1;
    in.layout = (format >> 1) & 7;
    uint16_t icount = load_u16(&raw[2]);
    in.mode = load_u16(&raw[4]);
    in.u = load_u32(&raw[16]);
    if (extended) {
      in.isize = 64;
      if (in.pos + 64 > src_->size()) throw_format("EROFS : inode hors de l'image");
      raw = src_->read(in.pos, 64);
      in.size = load_u32(&raw[8]) | (uint64_t(load_u32(&raw[12])) << 32);
    } else {
      in.size = load_u32(&raw[8]);
    }
    in.xattr_isize = icount ? 12 + 4 * (uint32_t(icount) - 1) : 0;
    if (in.layout > kChunkBased) throw_format("EROFS : disposition de données inconnue " + std::to_string(in.layout));
    return in;
  }

  void read_blocks(uint64_t blkaddr, uint64_t offset_in, std::span<uint8_t> out) const {
    if (blkaddr == kNullAddr) throw_format("EROFS : adresse de bloc nulle");
    src_->read((blkaddr << blkszbits_) + offset_in, out);
  }

  std::vector<uint8_t> read_data(const Inode& in, uint64_t max_size) const {
    if (in.size > max_size) throw_format("EROFS : fichier trop volumineux");
    std::vector<uint8_t> out(size_t(in.size), 0);
    if (in.size == 0) return out;
    switch (in.layout) {
      case kFlatPlain: read_blocks(in.u, 0, out); break;
      case kFlatInline: {
        uint64_t nblocks = (in.size + blksz_ - 1) >> blkszbits_;
        uint64_t head = (nblocks - 1) << blkszbits_;
        if (head) read_blocks(in.u, 0, std::span(out.data(), size_t(head)));
        uint64_t tail_pos = in.data_end();
        uint64_t tail_len = in.size - head;
        if ((tail_pos & (blksz_ - 1)) + tail_len > blksz_) throw_format("EROFS : données en ligne à cheval sur deux blocs");
        src_->read(tail_pos, std::span(out.data() + head, size_t(tail_len)));
        break;
      }
      case kChunkBased: read_chunks(in, out); break;
      case kCompressedFull:
      case kCompressedCompact: read_compressed(in, out); break;
    }
    return out;
  }

  void read_chunks(const Inode& in, std::vector<uint8_t>& out) const {
    if (!(features_ & kFeatChunkedFile)) throw_format("EROFS : fichier découpé sans la fonctionnalité correspondante");
    uint16_t format = uint16_t(in.u);
    uint32_t chunkbits = blkszbits_ + (format & 0x1F);
    if (chunkbits > 40) throw_format("EROFS : taille de segment invalide");
    bool indexes = format & 0x20;
    uint32_t unit = indexes ? 8 : 4;
    uint64_t chunk = 1ull << chunkbits;
    uint64_t count = (in.size + chunk - 1) >> chunkbits;
    uint64_t table = align_up(in.data_end(), unit);
    auto entries = src_->read(table, size_t(count * unit));
    for (uint64_t i = 0; i < count; ++i) {
      uint32_t blk;
      if (indexes) {
        uint16_t device = load_u16(&entries[i * 8 + 2]);
        blk = load_u32(&entries[i * 8 + 4]);
        if (device != 0 && blk != kNullAddr) throw_format("EROFS : périphériques supplémentaires non pris en charge");
      } else {
        blk = load_u32(&entries[i * 4]);
      }
      uint64_t off = i << chunkbits;
      uint64_t len = std::min<uint64_t>(chunk, in.size - off);
      if (blk != kNullAddr) read_blocks(blk, 0, std::span(out.data() + off, size_t(len)));
    }
  }

  // --- Compressed files --------------------------------------------------------

  struct ZInfo {
    uint64_t end = 0;  // end of inode + xattrs
    uint16_t advise = 0;
    uint8_t algs[2] = {0, 0};
    uint32_t lclusterbits = 12;
    uint32_t idata_size = 0;
    uint64_t total_lclusters = 0;
    bool compact = false;
  };

  struct Lcluster {
    uint8_t type = 0;
    uint32_t clusterofs = 0;
    uint64_t pblk = 0;
    uint32_t delta0 = 0;
    uint32_t cblks = 0;  // compressed block count (first NONHEAD of a big pcluster)
    uint64_t nextpackoff = 0;
    bool partialref = false;
  };

  Lcluster load_full(const ZInfo& z, uint64_t lcn) const {
    uint64_t pos = align_up(z.end, 8) + 8 + 8 + lcn * 8;
    auto d = src_->read(pos, 8);
    Lcluster m;
    m.nextpackoff = pos + 8;
    uint16_t advise = load_u16(&d[0]);
    m.type = advise & 3;
    if (m.type == kNonHead) {
      m.clusterofs = 1u << z.lclusterbits;
      m.delta0 = load_u16(&d[4]);
      if (m.delta0 & kD0Cblkcnt) {
        if (!(z.advise & (kAdviseBigPcluster1 | kAdviseBigPcluster2))) throw_format("EROFS : CBLKCNT sans grands pclusters");
        m.cblks = m.delta0 & ~kD0Cblkcnt;
        m.delta0 = 1;
      }
    } else {
      m.partialref = advise & kPartialRef;
      m.clusterofs = load_u16(&d[2]);
      if (m.clusterofs >= (1u << z.lclusterbits)) throw_format("EROFS : clusterofs invalide");
      m.pblk = load_u32(&d[4]);
    }
    return m;
  }

  Lcluster load_compact(const ZInfo& z, uint64_t lcn) const {
    uint64_t ebase = 8 + align_up(z.end, 8);
    uint64_t c4_initial = (32 - ebase % 32) / 4;
    if (c4_initial == 32 / 4) c4_initial = 0;
    uint64_t c2b = 0;
    if ((z.advise & kAdviseCompacted2B) && c4_initial < z.total_lclusters) {
      c2b = (z.total_lclusters - c4_initial) / 16 * 16;
    }
    uint64_t pos = ebase;
    uint32_t shift;
    uint64_t l = lcn;
    if (l < c4_initial) {
      shift = 2;
    } else {
      pos += c4_initial * 4;
      l -= c4_initial;
      if (l < c2b) {
        shift = 1;
      } else {
        pos += c2b * 2;
        l -= c2b;
        shift = 2;
      }
    }
    pos += l << shift;
    return unpack_compact(z, shift, pos);
  }

  Lcluster unpack_compact(const ZInfo& z, uint32_t shift, uint64_t pos) const {
    uint32_t vcnt;
    if ((1u << shift) == 4 && z.lclusterbits <= 14) {
      vcnt = 2;
    } else if ((1u << shift) == 2 && z.lclusterbits <= 12) {
      vcnt = 16;
    } else {
      throw_format("EROFS : index compacts non pris en charge pour cette taille de cluster");
    }
    uint32_t packsize = vcnt << shift;
    uint64_t base = pos / packsize * packsize;
    Lcluster m;
    m.nextpackoff = base + packsize;
    bool big = z.advise & kAdviseBigPcluster1;
    uint32_t lobits = std::max<uint32_t>(z.lclusterbits, 12);
    uint32_t encodebits = (packsize - 4) * 8 / vcnt;
    auto in = src_->read(base, packsize);
    int i = int((pos - base) >> shift);
    auto decode = [&](int idx, uint8_t& type) {
      uint32_t bitpos = encodebits * uint32_t(idx);
      uint32_t v = load_u32(&in[bitpos / 8]) >> (bitpos & 7);
      type = (v >> lobits) & 3;
      return v & ((1u << lobits) - 1);
    };
    uint8_t type;
    uint32_t lo = decode(i, type);
    m.type = type;
    if (type == kNonHead) {
      m.clusterofs = 1u << z.lclusterbits;
      if (lo & kD0Cblkcnt) {
        if (!big) throw_format("EROFS : CBLKCNT sans grands pclusters");
        m.cblks = lo & ~kD0Cblkcnt;
        m.delta0 = 1;
        return m;
      }
      if (i + 1 != int(vcnt)) {
        m.delta0 = lo;
        return m;
      }
      // The last lcluster of a pack stores delta[1]; derive delta[0] from
      // the previous one.
      lo = decode(i - 1, type);
      if (type != kNonHead) {
        lo = 0;
      } else if (lo & kD0Cblkcnt) {
        lo = 1;
      }
      m.delta0 = lo + 1;
      return m;
    }
    m.clusterofs = lo;
    if (m.clusterofs >= (1u << z.lclusterbits)) throw_format("EROFS : clusterofs invalide");
    uint64_t nblk;
    if (!big) {
      nblk = 1;
      while (i > 0) {
        --i;
        lo = decode(i, type);
        if (type == kNonHead) i -= int(lo);
        if (i >= 0) ++nblk;
      }
    } else {
      nblk = 0;
      while (i > 0) {
        --i;
        lo = decode(i, type);
        if (type == kNonHead) {
          if (lo & kD0Cblkcnt) {
            --i;
            nblk += lo & ~kD0Cblkcnt;
            continue;
          }
          if (lo <= 1) throw_format("EROFS : index compact incohérent");
          i -= int(lo) - 2;
          continue;
        }
        ++nblk;
      }
    }
    m.pblk = uint64_t(load_u32(&in[packsize - 4])) + nblk;
    return m;
  }

  Lcluster load_lcluster(const ZInfo& z, uint64_t lcn) const { return z.compact ? load_compact(z, lcn) : load_full(z, lcn); }

  void read_compressed(const Inode& in, std::vector<uint8_t>& out) const {
    ZInfo z;
    z.end = in.data_end();
    z.compact = in.layout == kCompressedCompact;
    auto h = src_->read(align_up(z.end, 8), 8);
    uint8_t clusterbits = h[7];
    if (clusterbits >> 7) throw_format("EROFS : fichier stocké dans l'inode empaqueté (fragments) non pris en charge");
    z.idata_size = load_u16(&h[2]);
    z.advise = load_u16(&h[4]);
    z.algs[0] = h[6] & 15;
    z.algs[1] = h[6] >> 4;
    z.lclusterbits = blkszbits_ + (clusterbits & 15);
    if (z.lclusterbits > 20) throw_format("EROFS : taille de cluster logique invalide");
    if (z.advise & kAdviseFragment) throw_format("EROFS : fragments (inode empaqueté) non pris en charge");
    if (z.compact && z.lclusterbits > 14) throw_format("EROFS : index compacts avec des clusters trop grands");
    const uint64_t lcs = 1ull << z.lclusterbits;
    z.total_lclusters = (in.size + lcs - 1) >> z.lclusterbits;
    if (z.total_lclusters > (64u << 20)) throw_format("EROFS : trop de clusters logiques");

    struct Head {
      uint64_t lcn, la, pblk;
      uint8_t type;
      uint32_t cblks = 1;
    };
    std::vector<Head> heads;
    Lcluster last;
    for (uint64_t lcn = 0; lcn < z.total_lclusters; ++lcn) {
      Lcluster m = load_lcluster(z, lcn);
      if (m.type == kNonHead) {
        if (heads.empty()) throw_format("EROFS : premier cluster logique sans tête");
        if (m.cblks && heads.back().lcn + 1 == lcn) heads.back().cblks = m.cblks;
      } else {
        uint64_t la = (lcn << z.lclusterbits) | m.clusterofs;
        if (heads.empty() && la != 0) throw_format("EROFS : cluster logique 0 invalide");
        if (la < in.size) heads.push_back({lcn, la, m.pblk, m.type});
      }
      last = m;
    }
    if (heads.empty()) throw_format("EROFS : aucun extent compressé");
    bool tail_inline = z.advise & kAdviseInlinePcluster;
    if (tail_inline && !(features_ & kFeatZtailpacking)) throw_format("EROFS : pcluster en ligne sans ztailpacking");

    for (size_t k = 0; k < heads.size(); ++k) {
      const Head& hd = heads[k];
      uint64_t end = k + 1 < heads.size() ? heads[k + 1].la : in.size;
      if (end <= hd.la) throw_format("EROFS : extents non ordonnés");
      size_t llen = size_t(end - hd.la);
      std::vector<uint8_t> pcluster;
      if (tail_inline && k + 1 == heads.size()) {
        pcluster = src_->read(last.nextpackoff, z.idata_size);
      } else {
        bool big = (hd.type == kHead1 && (z.advise & kAdviseBigPcluster1)) ||
                   (hd.type == kHead2 && (z.advise & kAdviseBigPcluster2));
        uint64_t plen = big ? uint64_t(hd.cblks) << blkszbits_ : lcs;
        if (plen > (16u << 20)) throw_format("EROFS : pcluster trop grand");
        if (hd.pblk == kNullAddr) throw_format("EROFS : pcluster sans adresse");
        pcluster = src_->read(hd.pblk << blkszbits_, size_t(plen));
      }
      uint8_t* dst = out.data() + hd.la;
      if (hd.type == kPlain) {
        if (llen > pcluster.size()) throw_format("EROFS : pcluster non compressé trop court");
        if (z.advise & kAdviseInterlaced) {
          size_t off = size_t(hd.la & (blksz_ - 1));
          size_t n = pcluster.size();
          for (size_t i = 0; i < llen; ++i) dst[i] = pcluster[(off + i) % n];
        } else {
          std::memcpy(dst, pcluster.data(), llen);
        }
        continue;
      }
      uint8_t alg = z.algs[hd.type == kHead2 ? 1 : 0];
      if (alg != 0) {
        static const char* names[] = {"LZ4", "LZMA", "DEFLATE", "ZSTD"};
        throw_format(std::string("EROFS : compression ") + (alg < 4 ? names[alg] : "inconnue") + " non prise en charge");
      }
      if (!(algs_ & 1)) throw_format("EROFS : LZ4 absent des algorithmes déclarés");
      std::span<const uint8_t> input(pcluster);
      if (features_ & kFeatZeroPadding) {
        size_t skip = 0;
        size_t limit = std::min<size_t>(input.size(), blksz_);
        while (skip < limit && input[skip] == 0) ++skip;
        if (skip == limit && limit == input.size()) throw_format("EROFS : pcluster vide");
        input = input.subspan(skip);
      }
      std::vector<uint8_t> plain = lz4_decompress(input, llen);
      if (plain.size() != llen) {
        throw_format("EROFS : extent décompressé trop court (" + std::to_string(plain.size()) + " / " +
                     std::to_string(llen) + " octets)");
      }
      std::memcpy(dst, plain.data(), llen);
    }
  }

  std::shared_ptr<const Source> src_;
  uint32_t blkszbits_ = 12;
  uint32_t blksz_ = 4096;
  uint64_t root_nid_ = 0;
  uint64_t meta_base_ = 0;
  uint32_t features_ = 0;
  uint16_t algs_ = 1;
  uint16_t extra_devices_ = 0;
};

}  // namespace

std::unique_ptr<FileSystem> open_erofs(std::shared_ptr<const Source> src) { return std::make_unique<Erofs>(std::move(src)); }

}  // namespace aa::image
