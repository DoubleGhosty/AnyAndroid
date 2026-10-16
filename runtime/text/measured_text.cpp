// minikin's MeasuredText.cpp and the populateHyphenationPoints of
// LineBreakerUtil.h. minikin keeps the shaped pieces in the MeasuredText
// (LayoutPieces) and looks them up before its LayoutCache; both give the
// pieces LayoutCache would shape, so the engine's layout cache serves here.
#include "text/measured_text.h"

#include <algorithm>

#include "text/layout_internal.h"
#include "text/word_breaker.h"

namespace aa::text {

namespace {

using detail::Range;

Bidi forced(bool rtl) { return rtl ? Bidi::kForceRtl : Bidi::kForceLtr; }

// BidiText(range, FORCE_*) then LayoutSplitter: fn(context, piece, rtl).
template <typename Fn>
void for_each_piece(std::u16string_view text, Range range, bool rtl, Fn&& fn) {
  for (const detail::BidiRunInfo& info : detail::bidi_runs(text, range, forced(rtl))) {
    if (!info.valid) continue;
    for (const auto& [context, piece] : detail::split_pieces(text, info.range, info.rtl)) fn(context, piece, info.rtl);
  }
}

// StyleRun::getMetrics, ReplacementRun::getMetrics.
void run_metrics(const TextRun& run, std::u16string_view text, std::vector<float>* widths,
                 std::vector<uint8_t>* flags, bool bounds_calculation) {
  if (!run.paint) {
    (*widths)[run.start] = run.width;
    return;
  }
  const Paint& paint = *run.paint;
  for_each_piece(text, Range{run.start, run.end}, run.rtl, [&](Range context, Range piece, bool rtl) {
    auto lp = detail::layout_piece(text, context, piece, rtl, paint, StartHyphenEdit::kNoEdit, EndHyphenEdit::kNoEdit);
    std::copy(lp->advances.begin(), lp->advances.end(), widths->begin() + ptrdiff_t(piece.start));
    if (!bounds_calculation) return;
    TextRect b = detail::piece_bounds(*lp, paint);
    if (b.left < 0 || b.right > lp->advance) {
      for (size_t i = piece.start; i < piece.end; ++i) (*flags)[i] |= MeasuredText::kMayOverhangBit;
    }
  });
}

// StyleRun::getBounds: the advance and the bounds of a range.
std::pair<float, TextRect> run_bounds(const TextRun& run, std::u16string_view text, Range range) {
  if (!run.paint) return {run.width, TextRect{}};  // replacement runs have no bounds
  float advance = 0;
  TextRect bounds;
  for_each_piece(text, range, run.rtl, [&](Range context, Range piece, bool rtl) {
    auto lp =
        detail::layout_piece(text, context, piece, rtl, *run.paint, StartHyphenEdit::kNoEdit, EndHyphenEdit::kNoEdit);
    TextRect b = detail::piece_bounds(*lp, *run.paint);
    if (lp->vertical) {
      bounds.join(b, 0, advance);
    } else {
      bounds.join(b, advance, 0);
    }
    advance += lp->advance;
  });
  return {advance, bounds};
}

Extent run_extent(const TextRun& run, std::u16string_view text, Range range) {
  Extent extent;
  if (!run.paint) return extent;
  for_each_piece(text, range, run.rtl, [&](Range context, Range piece, bool rtl) {
    extent.extend(
        detail::layout_piece(text, context, piece, rtl, *run.paint, StartHyphenEdit::kNoEdit, EndHyphenEdit::kNoEdit)
            ->extent);
  });
  return extent;
}

LineMetrics run_line_metrics(const TextRun& run, std::u16string_view text, Range range) {
  LineMetrics metrics;
  if (!run.paint) return metrics;  // as minikin: a replacement adds no advance here
  for_each_piece(text, range, run.rtl, [&](Range context, Range piece, bool rtl) {
    auto lp =
        detail::layout_piece(text, context, piece, rtl, *run.paint, StartHyphenEdit::kNoEdit, EndHyphenEdit::kNoEdit);
    metrics.append(lp->extent, detail::piece_bounds(*lp, *run.paint), lp->advance);
  });
  return metrics;
}

// populateHyphenationPoints: the hyphenation points of a word, with the
// widths of the pieces on each side.
void populate_hyphenation_points(std::u16string_view text, const TextRun& run, const Hyphenator& hyphenator,
                                 Range context, Range target, const std::vector<float>& widths,
                                 bool ignore_kerning, std::vector<HyphenBreak>* out) {
  if (!Range{run.start, run.end}.contains(context) || !context.contains(target)) return;
  const std::vector<HyphenationType> result = hyphenate(text.substr(target.start, target.length()), hyphenator);
  for (size_t i = target.start; i < target.end; ++i) {
    const HyphenationType hyph = result[i - target.start];
    if (hyph == HyphenationType::kDontBreak) continue;
    float first = 0, second = 0;
    if (!ignore_kerning) {
      std::u16string_view first_text = text.substr(context.start, i - context.start);
      std::u16string_view second_text = text.substr(i, context.end - i);
      first = run.measure_hyphen_piece(first_text, 0, first_text.size(), StartHyphenEdit::kNoEdit,
                                       edit_for_this_line(hyph));
      second = run.measure_hyphen_piece(second_text, 0, second_text.size(), edit_for_next_line(hyph),
                                        EndHyphenEdit::kNoEdit);
    } else {
      for (size_t j = context.start; j < i; ++j) first += widths[j];
      for (size_t j = i; j < context.end; ++j) second += widths[j];
      EndHyphenEdit end_edit = edit_for_this_line(hyph);
      StartHyphenEdit start_edit = edit_for_next_line(hyph);
      if (end_edit != EndHyphenEdit::kNoEdit) first += run.measure_text(detail::hyphen_string(end_edit));
      if (start_edit != StartHyphenEdit::kNoEdit) second += run.measure_text(detail::hyphen_string(start_edit));
    }
    out->push_back(HyphenBreak{uint32_t(i), hyph, first, second});
  }
}

}  // namespace

TextRun TextRun::style(uint32_t start, uint32_t end, Paint paint, int32_t line_break_style,
                       int32_t line_break_word_style, bool hyphenation, bool rtl) {
  TextRun r;
  r.start = start;
  r.end = end;
  r.paint = std::move(paint);
  r.line_break_style = LineBreakStyle(line_break_style);
  r.line_break_word_style = LineBreakWordStyle(line_break_word_style);
  r.hyphenation = hyphenation;
  r.rtl = rtl;
  return r;
}

TextRun TextRun::replacement(uint32_t start, uint32_t end, float width, uint32_t locale_list_id) {
  TextRun r;
  r.start = start;
  r.end = end;
  r.width = width;
  r.replacement_locale_list_id = locale_list_id;
  return r;
}

float TextRun::measure_hyphen_piece(std::u16string_view text, size_t from, size_t to, StartHyphenEdit start_hyphen,
                                    EndHyphenEdit end_hyphen) const {
  if (!paint) return 0;
  float advance = 0;
  Range range{from, to};
  for_each_piece(text, range, rtl, [&](Range context, Range piece, bool piece_rtl) {
    StartHyphenEdit se = piece.start == range.start ? start_hyphen : StartHyphenEdit::kNoEdit;
    EndHyphenEdit ee = piece.end == range.end ? end_hyphen : EndHyphenEdit::kNoEdit;
    advance += detail::layout_piece(text, context, piece, piece_rtl, *paint, se, ee)->advance;
  });
  return advance;
}

float TextRun::measure_text(std::u16string_view text) const {
  if (!paint) return 0;
  float total = 0;
  for_each_piece(text, Range{0, text.size()}, rtl, [&](Range context, Range piece, bool piece_rtl) {
    auto lp = detail::layout_piece(text, context, piece, piece_rtl, *paint, StartHyphenEdit::kNoEdit,
                                   EndHyphenEdit::kNoEdit);
    for (float w : lp->advances) total += w;
  });
  return total;
}

std::unique_ptr<MeasuredText> MeasuredText::build(std::u16string_view text, std::vector<TextRun> runs,
                                                  bool compute_hyphenation, bool compute_bounds,
                                                  bool ignore_hyphen_kerning) {
  std::unique_ptr<MeasuredText> mt(new MeasuredText);
  mt->widths.assign(text.size(), 0.0f);
  mt->flags.assign(text.size(), 0);
  mt->runs = std::move(runs);
  mt->measure(text, compute_hyphenation, compute_bounds, ignore_hyphen_kerning);
  return mt;
}

void MeasuredText::measure(std::u16string_view text, bool compute_hyphenation, bool compute_bounds,
                           bool ignore_hyphen_kerning) {
  if (text.empty()) return;
  CharProcessor proc(text);
  for (const TextRun& run : runs) {
    if (run.start > run.end || run.end > text.size()) continue;  // Java checks the ranges first
    run_metrics(run, text, &widths, &flags, compute_bounds);
    if (!compute_hyphenation || !run.can_break()) continue;
    proc.update_locale_if_necessary(run, false);
    for (uint32_t i = run.start; i < run.end; ++i) {
      // The end of a run is a break candidate even in a run that cannot break.
      const bool can_break = run.can_break() || (i + 1) == run.end;
      proc.feed_char(i, text[i], widths[i], can_break);
      if (i + 1 != proc.next_word_break) continue;  // wait for a word break
      populate_hyphenation_points(text, run, *proc.hyphenator, proc.context_range(), proc.word_range(), widths,
                                  ignore_hyphen_kerning, &hyphen_breaks);
    }
  }
}

bool MeasuredText::has_overhang(size_t start, size_t end) const {
  constexpr size_t kCharsToDumper = 5;
  auto any = [&](size_t s, size_t e) {
    for (size_t i = s; i < e; ++i) {
      if (flags[i] & kMayOverhangBit) return true;
    }
    return false;
  };
  if (end - start < kCharsToDumper * 2) return any(start, end);
  return any(start, start + kCharsToDumper) || any(end - kCharsToDumper, end);
}

uint32_t MeasuredText::memory_usage() const {
  // minikin's HyphenBreak is 16 bytes; no layout pieces are kept here.
  return uint32_t(sizeof(float) * widths.size() + 16 * hyphen_breaks.size());
}

TextRect MeasuredText::bounds(std::u16string_view text, size_t start, size_t end) const {
  TextRect rect;
  float total = 0;
  Range range{start, end};
  for (const TextRun& run : runs) {
    Range run_range{run.start, run.end};
    if (!Range::intersects(range, run_range)) continue;
    auto [advance, b] = run_bounds(run, text, Range::intersection(run_range, range));
    if (run.paint && run.paint->vertical_text) {
      rect.join(b, 0, total);
    } else {
      rect.join(b, total, 0);
    }
    total += advance;
  }
  return rect;
}

Extent MeasuredText::extent(std::u16string_view text, size_t start, size_t end) const {
  Extent extent;
  Range range{start, end};
  for (const TextRun& run : runs) {
    Range run_range{run.start, run.end};
    if (!Range::intersects(range, run_range)) continue;
    extent.extend(run_extent(run, text, Range::intersection(run_range, range)));
  }
  return extent;
}

LineMetrics MeasuredText::line_metrics(std::u16string_view text, size_t start, size_t end) const {
  LineMetrics metrics;
  Range range{start, end};
  for (const TextRun& run : runs) {
    Range run_range{run.start, run.end};
    if (!Range::intersects(range, run_range)) continue;
    metrics.append(run_line_metrics(run, text, Range::intersection(run_range, range)));
  }
  return metrics;
}

}  // namespace aa::text
