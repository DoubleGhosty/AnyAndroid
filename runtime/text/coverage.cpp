// minikin's CmapCoverage.cpp (frameworks/minikin). Variation sequences
// (format 14) are answered by FreeType (FT_Face_GetCharVariantIndex), whose
// rules are the same: a default UVS entry maps to the base glyph.
#include "text/coverage.h"

#include <algorithm>

namespace aa::text {

bool Coverage::contains(uint32_t c) const {
  // The range whose end is the first one above c.
  auto it = std::upper_bound(ranges_.begin(), ranges_.end(), c);
  size_t i = size_t(it - ranges_.begin());
  return i % 2 == 1;  // between a start and its end
}

uint32_t Coverage::next(uint32_t c) const {
  auto it = std::upper_bound(ranges_.begin(), ranges_.end(), c);
  size_t i = size_t(it - ranges_.begin());
  if (i % 2 == 1) return c;              // inside a range
  if (i >= ranges_.size()) return kNotFound;
  return ranges_[i];                      // start of the next range
}

namespace {

constexpr uint32_t kMaxUnicode = 0x10FFFF;

uint32_t u16(const uint8_t* d, size_t o) { return uint32_t(d[o]) << 8 | d[o + 1]; }
uint32_t u32(const uint8_t* d, size_t o) { return u16(d, o) << 16 | u16(d, o + 2); }

// Appends [start, end): the start must not precede the last end.
bool add_range(std::vector<uint32_t>& cov, uint32_t start, uint32_t end) {
  if (cov.empty() || cov.back() < start) {
    cov.push_back(start);
    cov.push_back(end);
    return true;
  }
  if (cov.back() == start) {
    cov.back() = end;
    return true;
  }
  return false;  // unordered: rejected, as minikin does
}

bool add_range_cmap4(std::vector<uint32_t>& cov, uint32_t start, uint32_t end) {
  if (!cov.empty() && cov.back() > end) return false;
  if (cov.empty() || cov.back() < start) {
    cov.push_back(start);
    cov.push_back(end);
  } else {
    cov.back() = end;
  }
  return true;
}

bool format4(std::vector<uint32_t>& cov, const uint8_t* data, size_t size) {
  constexpr size_t kSegCount = 6, kEndCount = 14, kHeader = 16, kSegment = 8;
  if (kEndCount > size) return false;
  size_t segs = u16(data, kSegCount) >> 1;
  if (kHeader + segs * kSegment > size) return false;
  for (size_t i = 0; i < segs; i++) {
    uint32_t end = u16(data, kEndCount + 2 * i);
    uint32_t start = u16(data, kHeader + 2 * (segs + i));
    if (end < start) return false;
    uint32_t range_offset = u16(data, kHeader + 2 * (3 * segs + i));
    if (range_offset == 0) {
      uint32_t delta = u16(data, kHeader + 2 * (2 * segs + i));
      if (((end + delta) & 0xffff) > end - start) {
        if (!add_range_cmap4(cov, start, end + 1)) return false;
      } else {
        for (uint32_t j = start; j < end + 1; j++) {
          if (((j + delta) & 0xffff) != 0 && !add_range_cmap4(cov, j, j + 1)) return false;
        }
      }
    } else {
      for (uint32_t j = start; j < end + 1; j++) {
        size_t at = kHeader + 6 * segs + range_offset + (i + j - start) * 2;
        if (at + 2 > size) continue;
        if (u16(data, at) != 0 && !add_range_cmap4(cov, j, j + 1)) return false;
      }
    }
  }
  return true;
}

bool format12(std::vector<uint32_t>& cov, const uint8_t* data, size_t size) {
  constexpr size_t kNGroups = 12, kFirstGroup = 16, kGroup = 12;
  constexpr size_t kMaxNGroups = 0xfffffff0 / kGroup;
  if (kFirstGroup > size) return false;
  uint32_t groups = u32(data, kNGroups);
  if (groups >= kMaxNGroups || kFirstGroup + size_t(groups) * kGroup > size) return false;
  for (uint32_t i = 0; i < groups; i++) {
    size_t at = kFirstGroup + i * kGroup;
    uint32_t start = u32(data, at), end = u32(data, at + 4);
    if (end < start) return false;
    if (start > kMaxUnicode) return true;
    if (end > kMaxUnicode) return add_range(cov, start, kMaxUnicode + 1);
    if (!add_range(cov, start, end + 1)) return false;
  }
  return true;
}

// HarfBuzz's subtable order (hb-ot-font.cc), lower first.
constexpr uint8_t kLowestPriority = 255;
uint8_t priority(uint32_t platform, uint32_t encoding) {
  if (platform == 3 && encoding == 10) return 0;
  if (platform == 0 && encoding == 6) return 1;
  if (platform == 0 && encoding == 4) return 2;
  if (platform == 3 && encoding == 1) return 3;
  if (platform == 0 && encoding == 3) return 4;
  if (platform == 0 && encoding == 2) return 5;
  if (platform == 0 && encoding == 1) return 6;
  if (platform == 0 && encoding == 0) return 7;
  return kLowestPriority;
}

}  // namespace

Coverage cmap_coverage(std::span<const uint8_t> cmap, bool* has_vs_table) {
  *has_vs_table = false;
  const uint8_t* data = cmap.data();
  size_t size = cmap.size();
  constexpr size_t kHeader = 4, kRecord = 8;
  constexpr uint32_t kNoTable = 0xFFFFFFFF;
  if (kHeader > size) return Coverage();
  uint32_t tables = u16(data, 2);
  if (kHeader + tables * kRecord > size) return Coverage();
  uint32_t best = kNoTable, vs = kNoTable;
  uint32_t best_format = 0;
  uint8_t best_priority = kLowestPriority;
  for (uint32_t i = 0; i < tables; ++i) {
    size_t head = kHeader + i * kRecord;
    uint32_t platform = u16(data, head), encoding = u16(data, head + 2), offset = u32(data, head + 4);
    if (size < 2 || offset > size - 2) continue;
    uint32_t format = u16(data, offset);
    if (platform == 0 && encoding == 5) {
      if (vs == kNoTable && format == 14) vs = offset;
    } else {
      uint32_t length, language;
      if (format == 4) {
        if (offset > size - 6) continue;
        length = u16(data, offset + 2);
        language = u16(data, offset + 4);
      } else if (format == 12) {
        if (offset > size - 12) continue;
        length = u32(data, offset + 4);
        language = u32(data, offset + 8);
      } else {
        continue;
      }
      if (length > size - offset || language != 0) continue;
      uint8_t p = priority(platform, encoding);
      if (p < best_priority) {
        best = offset;
        best_priority = p;
        best_format = format;
      }
    }
    if (vs != kNoTable && best_priority == 0) break;
  }
  *has_vs_table = vs != kNoTable;
  if (best == kNoTable) return Coverage();
  std::vector<uint32_t> ranges;
  bool ok = best_format == 4 ? format4(ranges, data + best, size - best) : format12(ranges, data + best, size - best);
  return ok ? Coverage(std::move(ranges)) : Coverage();
}

}  // namespace aa::text
