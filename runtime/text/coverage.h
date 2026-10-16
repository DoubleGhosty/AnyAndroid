#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace aa::text {

// The code points a font maps to a glyph: minikin's SparseBitSet as sorted,
// disjoint [start, end) ranges.
class Coverage {
 public:
  static constexpr uint32_t kNotFound = 0xFFFFFFFF;
  Coverage() = default;
  explicit Coverage(std::vector<uint32_t> ranges) : ranges_(std::move(ranges)) {}
  bool contains(uint32_t c) const;
  // The first covered code point >= c, or kNotFound.
  uint32_t next(uint32_t c) const;
  // One past the last covered code point (0 when empty).
  uint32_t length() const { return ranges_.empty() ? 0 : ranges_.back(); }
  bool empty() const { return ranges_.empty(); }
  const std::vector<uint32_t>& ranges() const { return ranges_; }

 private:
  std::vector<uint32_t> ranges_;
};

// minikin's CmapCoverage::getCoverage: the coverage of the best Unicode
// subtable (format 4 or 12) of a raw cmap table, empty when none is valid.
// *has_vs_table tells whether a format 14 subtable (variation sequences) is
// present.
Coverage cmap_coverage(std::span<const uint8_t> cmap, bool* has_vs_table);

}  // namespace aa::text
