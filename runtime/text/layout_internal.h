#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "text/layout.h"

// What layout.cpp shares with measured_text.cpp and line_breaker.cpp:
// minikin's Range, LayoutPiece and LayoutCache, BidiText and LayoutSplitter.
// Not part of the engine's interface.
namespace aa::text::detail {

// minikin's Range: [start, end) in code units.
struct Range {
  size_t start = 0, end = 0;
  size_t length() const { return end - start; }
  bool empty() const { return start == end; }
  bool contains(const Range& o) const { return start <= o.start && o.end <= end; }
  bool contains(size_t pos) const { return start <= pos && pos < end; }
  bool operator==(const Range&) const = default;
  static bool intersects(const Range& a, const Range& b) { return a.start < b.end && b.start < a.end; }
  static Range intersection(const Range& a, const Range& b) {
    return {a.start > b.start ? a.start : b.start, a.end < b.end ? a.end : b.end};
  }
};

struct Point {
  float x = 0, y = 0;
};

// A word shaped by HarfBuzz (LayoutCore.cpp).
struct LayoutPiece {
  std::vector<float> advances;  // per code unit of the piece
  std::vector<uint8_t> font_indices;
  std::vector<uint32_t> glyph_ids;
  std::vector<Point> points;
  std::vector<uint32_t> clusters;
  std::vector<FakedFont> fonts;
  float advance = 0;
  Extent extent;
  uint32_t cluster_count = 0;
  bool vertical = false;
};

bool is_word_space(char16_t c);

// LayoutSplitter: (context, piece) pairs of a run, in layout order.
std::vector<std::pair<Range, Range>> split_pieces(std::u16string_view s, Range range, bool rtl);

// BidiText: the runs of `range`, in visual order; a run emptied by the
// clipping to `range` is marked invalid and skipped by its users.
struct BidiRunInfo {
  Range range;
  bool rtl = false;
  bool valid = true;
};
std::vector<BidiRunInfo> bidi_runs(std::u16string_view text, Range range, Bidi bidi);

// LayoutCache::getOrCreate: `piece` of `text` shaped with `context` (which
// contains it) as context, from the cache when the same word was shaped
// with the same paint.
std::shared_ptr<const LayoutPiece> layout_piece(std::u16string_view text, Range context, Range piece, bool rtl,
                                                const Paint& paint, StartHyphenEdit start_hyphen,
                                                EndHyphenEdit end_hyphen);
// LayoutPiece::calculateBounds.
TextRect piece_bounds(const LayoutPiece& p, const Paint& paint);

// getHyphenString.
std::u16string_view hyphen_string(StartHyphenEdit h);
std::u16string_view hyphen_string(EndHyphenEdit h);

}  // namespace aa::text::detail
