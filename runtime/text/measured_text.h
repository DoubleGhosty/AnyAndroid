#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "text/layout.h"
#include "text/locale.h"

// minikin's MeasuredText (MeasuredText.cpp, MeasuredText.h): a paragraph
// measured once, run by run, for line breaking (line_breaker.h) and for
// Java's android.graphics.text.MeasuredText. The text itself is not kept:
// each query is given it again, as Java does.
namespace aa::text {

// LineBreakConfig.LINE_BREAK_STYLE_* and LINE_BREAK_WORD_STYLE_*.
enum class LineBreakStyle : uint8_t { kNone = 0, kLoose = 1, kNormal = 2, kStrict = 3, kNoBreak = 4, kAuto = 5 };
enum class LineBreakWordStyle : uint8_t { kNone = 0, kPhrase = 1, kAuto = 2 };

// minikin's HyphenationType: what a break inside a word does.
enum class HyphenationType : uint8_t {
  kDontBreak = 0,
  kBreakAndInsertHyphen = 1,
  kBreakAndInsertArmenianHyphen = 2,
  kBreakAndInsertMaqaf = 3,
  kBreakAndInsertUcasHyphen = 4,
  kBreakAndDontInsertHyphen = 5,
  kBreakAndReplaceWithHyphen = 6,
  kBreakAndInsertHyphenAtNextLine = 7,
  kBreakAndInsertHyphenAndZwj = 8,
};
EndHyphenEdit edit_for_this_line(HyphenationType type);
StartHyphenEdit edit_for_next_line(HyphenationType type);
// Paint's packing of the two edits (start << 3 | end).
inline int32_t pack_hyphen_edit(StartHyphenEdit start, EndHyphenEdit end) {
  return int32_t(uint8_t(start)) << 3 | int32_t(uint8_t(end));
}

struct LineMetrics {
  Extent extent;
  TextRect bounds;
  float advance = 0;
  void append(const Extent& e, const TextRect& b, float a) {
    extent.extend(e);
    bounds.join(b, advance, 0);
    advance += a;
  }
  void append(const LineMetrics& m) { append(m.extent, m.bounds, m.advance); }
};

// A run of the paragraph: styled text (minikin's StyleRun), or a span drawn
// at a fixed width with no paint (ReplacementRun: ReplacementSpan).
struct TextRun {
  uint32_t start = 0, end = 0;
  std::optional<Paint> paint;  // none for a replacement run
  float width = 0;             // of a replacement run
  uint32_t replacement_locale_list_id = kEmptyLocaleListId;
  LineBreakStyle line_break_style = LineBreakStyle::kNone;
  LineBreakWordStyle line_break_word_style = LineBreakWordStyle::kNone;
  bool hyphenation = false;
  bool rtl = false;

  static TextRun style(uint32_t start, uint32_t end, Paint paint, int32_t line_break_style,
                       int32_t line_break_word_style, bool hyphenation, bool rtl);
  static TextRun replacement(uint32_t start, uint32_t end, float width, uint32_t locale_list_id);

  bool can_break() const { return paint.has_value(); }
  bool can_hyphenate() const { return paint.has_value() && hyphenation; }
  uint32_t locale_list_id() const { return paint ? paint->locale_list_id : replacement_locale_list_id; }
  // getLetterSpacingInPx.
  float letter_spacing_px() const { return paint ? paint->letter_spacing * paint->size * paint->scale_x : 0.0f; }
  // measureHyphenPiece: the advance of text[from, to) with the hyphen
  // edits at its ends (0 for a replacement run).
  float measure_hyphen_piece(std::u16string_view text, size_t from, size_t to, StartHyphenEdit start_hyphen,
                             EndHyphenEdit end_hyphen) const;
  // measureText: the sum of the advances of `text` (a hyphen string).
  float measure_text(std::u16string_view text) const;
};

// A hyphenation point and the widths of the two pieces it makes.
struct HyphenBreak {
  uint32_t offset = 0;
  HyphenationType type = HyphenationType::kDontBreak;
  float first = 0, second = 0;
};

class MeasuredText {
 public:
  // MeasuredTextBuilder::build. `hint` (a previous measure of the same
  // paragraph) only saves work in minikin: it is not taken here, the layout
  // cache does the same.
  static std::unique_ptr<MeasuredText> build(std::u16string_view text, std::vector<TextRun> runs,
                                             bool compute_hyphenation, bool compute_bounds,
                                             bool ignore_hyphen_kerning);

  std::vector<float> widths;  // per code unit
  std::vector<HyphenBreak> hyphen_breaks;
  std::vector<TextRun> runs;
  static constexpr uint8_t kMayOverhangBit = 0b0000'0001;
  std::vector<uint8_t> flags;  // per code unit

  // Heuristics of minikin: the first and last five characters of the range.
  bool has_overhang(size_t start, size_t end) const;
  uint32_t memory_usage() const;
  TextRect bounds(std::u16string_view text, size_t start, size_t end) const;
  Extent extent(std::u16string_view text, size_t start, size_t end) const;
  LineMetrics line_metrics(std::u16string_view text, size_t start, size_t end) const;

 private:
  MeasuredText() = default;
  void measure(std::u16string_view text, bool compute_hyphenation, bool compute_bounds, bool ignore_hyphen_kerning);
};

}  // namespace aa::text
