#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "text/measured_text.h"

// minikin's line breaking (LineBreaker.cpp, GreedyLineBreaker.cpp,
// OptimalLineBreaker.cpp, AndroidLineBreakerHelper.h): where the lines of a
// measured paragraph end, for android.graphics.text.LineBreaker.
namespace aa::text {

// LineBreaker.BREAK_STRATEGY_* and HYPHENATION_FREQUENCY_*.
enum class BreakStrategy : uint8_t { kGreedy = 0, kHighQuality = 1, kBalanced = 2 };
enum class HyphenationFrequency : uint8_t { kNone = 0, kNormal = 1, kFull = 2 };

// The tab stops of a paragraph: the given stops, then every tab_width.
struct TabStops {
  std::vector<float> stops;
  float tab_width = 0;
  float next_tab(float width_so_far) const;
};

// AndroidLineWidth: first_width for the first first_line_count lines, then
// rest_width, each less its indent (indents[line + indents_offset], the last
// one beyond the array).
struct LineWidths {
  float first_width = 0;
  int32_t first_line_count = 0;
  float rest_width = 0;
  std::vector<float> indents;
  int32_t indents_offset = 0;
  float at(size_t line) const;
  float min() const;
};

struct LineBreakResult {
  std::vector<int32_t> break_points;
  std::vector<float> widths, ascents, descents;
  std::vector<int32_t> flags;  // hyphen edits (start << 3 | end), and kTabBit
  std::vector<TextRect> bounds;
  static constexpr int32_t kTabBit = 1 << 29;  // StaticLayout.TAB_MASK
  void reverse();
};

// breakIntoLines: greedy for BREAK_STRATEGY_SIMPLE and for text with tabs,
// optimal otherwise.
LineBreakResult break_into_lines(std::u16string_view text, BreakStrategy strategy, HyphenationFrequency frequency,
                                 bool justified, const MeasuredText& measured, const LineWidths& widths,
                                 const TabStops& tab_stops, bool use_bounds_for_width);

}  // namespace aa::text
