// minikin's LineBreaker.cpp, GreedyLineBreaker.cpp, OptimalLineBreaker.cpp
// and AndroidLineBreakerHelper.h, with their numeric types (doubles in the
// greedy breaker, floats elsewhere) so that the same lines come out.
#include "text/line_breaker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

#include "text/layout_internal.h"
#include "text/word_breaker.h"

namespace aa::text {

namespace {

using detail::Range;

constexpr uint32_t kNowhere = 0xFFFFFFFF;
// LBW_AUTO_HEURISTICS_LINE_COUNT: phrase breaking is retried only for
// paragraphs of fewer lines.
constexpr size_t kAutoHeuristicsLineCount = 5;

// trimTrailingLineEndSpaces.
Range trim_trailing_line_end_spaces(std::u16string_view text, Range range) {
  for (size_t i = 0; i < range.length(); i++) {
    if (!is_line_end_space(text[range.end - i - 1])) return {range.start, range.end - i};
  }
  return {range.start, range.start};
}

// The line's metrics as both breakers record them.
void push_line_metrics(std::u16string_view text, const MeasuredText& measured, Range range, float line_width,
                       bool use_bounds_for_width, LineBreakResult* out) {
  if (use_bounds_for_width) {
    Range actual = trim_trailing_line_end_spaces(text, range);
    if (actual.empty()) {  // only line-end spaces
      Extent extent = measured.extent(text, range.start, range.end);
      out->ascents.push_back(extent.ascent);
      out->descents.push_back(extent.descent);
      out->bounds.push_back(TextRect{0, extent.ascent, line_width, extent.descent});
    } else {
      LineMetrics metrics = measured.line_metrics(text, actual.start, actual.end);
      out->ascents.push_back(metrics.extent.ascent);
      out->descents.push_back(metrics.extent.descent);
      out->bounds.push_back(metrics.bounds);
    }
  } else {
    // Without bounds, the line's box is its extent and width.
    Extent extent = measured.extent(text, range.start, range.end);
    out->ascents.push_back(extent.ascent);
    out->descents.push_back(extent.descent);
    out->bounds.push_back(TextRect{0, extent.ascent, line_width, extent.descent});
  }
}

const TextRun* run_containing(const MeasuredText& measured, Range range) {
  const TextRun* target = nullptr;
  for (const TextRun& run : measured.runs) {
    if (Range{run.start, run.end}.contains(range)) target = &run;
  }
  return target;
}

// --- GreedyLineBreaker ---

class GreedyLineBreaker {
 public:
  GreedyLineBreaker(std::u16string_view text, const MeasuredText& measured, const LineWidths& limits,
                    const TabStops& tab_stops, bool enable_hyphenation, bool use_bounds_for_width)
      : line_width_limit_(limits.at(0)),
        text_(text),
        measured_(measured),
        limits_(limits),
        tab_stops_(tab_stops),
        enable_hyphenation_(enable_hyphenation),
        use_bounds_for_width_(use_bounds_for_width) {}

  void process(bool force_word_style_auto_to_phrase);
  LineBreakResult result() const;
  bool retry_with_phrase_word_break = false;

 private:
  struct BreakPoint {
    uint32_t offset;
    float line_width;
    int32_t hyphen_edit;
  };

  uint32_t prev_line_break_offset() const { return break_points_.empty() ? 0 : break_points_.back().offset; }
  void break_line_at(uint32_t offset, float line_width, float remaining_next_line_width,
                     float remaining_next_sum_of_char_widths, EndHyphenEdit this_line_end_hyphen,
                     StartHyphenEdit next_line_start_hyphen);
  void update_line_width(char16_t c, float width);
  void process_line_break(uint32_t offset, WordBreaker* breaker, bool do_hyphenation);
  bool try_line_break_with_word_break();
  bool try_line_break_with_hyphenation(Range range, WordBreaker* breaker);
  bool do_line_break_with_grapheme_bounds(Range range);
  bool overhang_exceed_line_limit(Range range);
  bool do_line_break_with_fallback(Range range);
  bool is_width_exceeded() const;

  uint32_t line_num_ = 0;
  double line_width_ = 0;
  double sum_of_char_widths_ = 0;
  double line_width_limit_;
  StartHyphenEdit start_hyphen_edit_ = StartHyphenEdit::kNoEdit;

  uint32_t prev_word_bounds_offset_ = kNowhere;
  double line_width_at_prev_word_boundary_ = 0;
  double sum_of_char_widths_at_prev_word_boundary_ = 0;
  bool is_prev_word_break_in_email_or_url_ = false;
  float line_start_letter_spacing_ = 0;
  float current_letter_spacing_ = 0;

  const Hyphenator* hyphenator_ = nullptr;

  std::u16string_view text_;
  const MeasuredText& measured_;
  const LineWidths& limits_;
  const TabStops& tab_stops_;
  bool enable_hyphenation_;
  bool use_bounds_for_width_;

  std::vector<BreakPoint> break_points_;
};

void GreedyLineBreaker::break_line_at(uint32_t offset, float line_width, float remaining_next_line_width,
                                      float remaining_next_sum_of_char_widths, EndHyphenEdit this_line_end_hyphen,
                                      StartHyphenEdit next_line_start_hyphen) {
  float edge_letter_spacing = (line_start_letter_spacing_ + current_letter_spacing_) / 2.0f;
  break_points_.push_back(
      BreakPoint{offset, line_width - edge_letter_spacing, pack_hyphen_edit(start_hyphen_edit_, this_line_end_hyphen)});
  line_width_limit_ = limits_.at(++line_num_);
  line_width_ = remaining_next_line_width;
  sum_of_char_widths_ = remaining_next_sum_of_char_widths;
  start_hyphen_edit_ = next_line_start_hyphen;
  prev_word_bounds_offset_ = kNowhere;
  line_width_at_prev_word_boundary_ = 0;
  sum_of_char_widths_at_prev_word_boundary_ = 0;
  is_prev_word_break_in_email_or_url_ = false;
  line_start_letter_spacing_ = current_letter_spacing_;
}

bool GreedyLineBreaker::try_line_break_with_word_break() {
  if (prev_word_bounds_offset_ == kNowhere) return false;  // no word break before
  break_line_at(prev_word_bounds_offset_, float(line_width_at_prev_word_boundary_),
                float(line_width_ - sum_of_char_widths_at_prev_word_boundary_),
                float(sum_of_char_widths_ - sum_of_char_widths_at_prev_word_boundary_), EndHyphenEdit::kNoEdit,
                StartHyphenEdit::kNoEdit);
  return true;
}

bool GreedyLineBreaker::try_line_break_with_hyphenation(Range range, WordBreaker* breaker) {
  if (!enable_hyphenation_ || hyphenator_ == nullptr) return false;
  const TextRun* target_run = run_containing(measured_, range);
  if (target_run == nullptr) return false;  // the range spans several runs
  const Range target_range = breaker->word_range();
  if (!range.contains(target_range)) return false;
  if (!target_run->can_hyphenate()) return false;

  const std::vector<HyphenationType> hyphen_result =
      hyphenate(text_.substr(target_range.start, target_range.length()), *hyphenator_);
  Range context_range = range;
  uint32_t prev_offset = kNowhere;
  float prev_width = 0;

  auto break_at_prev = [&] {
    const HyphenationType hyph = hyphen_result[prev_offset - target_range.start];
    const StartHyphenEdit next_line_start_hyphen_edit = edit_for_next_line(hyph);
    const float remaining_char_widths = target_run->measure_hyphen_piece(
        text_, prev_offset, context_range.end, next_line_start_hyphen_edit, EndHyphenEdit::kNoEdit);
    break_line_at(prev_offset, prev_width, float(remaining_char_widths - (sum_of_char_widths_ - line_width_)),
                  remaining_char_widths, edit_for_this_line(hyph), next_line_start_hyphen_edit);
  };

  // The hyphenation points from the beginning.
  for (uint32_t i = uint32_t(target_range.start); i < target_range.end; ++i) {
    const HyphenationType hyph = hyphen_result[i - target_range.start];
    if (hyph == HyphenationType::kDontBreak) continue;
    const float width = target_run->measure_hyphen_piece(text_, context_range.start, i, start_hyphen_edit_,
                                                         edit_for_this_line(hyph));
    if (width <= line_width_limit_) {
      // Still room: remember this point and look at the next one.
      prev_offset = i;
      prev_width = width;
      continue;
    }
    if (prev_offset == kNowhere) {
      // Too long even hyphenated: break between characters.
      do_line_break_with_grapheme_bounds(context_range);
    } else {
      break_at_prev();  // the longest piece that fits
    }
    if (line_width_ <= line_width_limit_) return true;  // the rest fits
    // Still too long after the break: hyphenate the rest.
    i = prev_line_break_offset();
    context_range.start = i;
    prev_offset = kNowhere;
  }
  // The same at the end of the word.
  if (prev_offset == kNowhere) {
    do_line_break_with_grapheme_bounds(context_range);
  } else {
    break_at_prev();
  }
  return true;
}

bool GreedyLineBreaker::do_line_break_with_grapheme_bounds(Range range) {
  float width = measured_.widths[range.start];
  const float estimated_letter_spacing = float((line_start_letter_spacing_ + current_letter_spacing_) * 0.5);
  // At least one character on each line.
  for (uint32_t i = uint32_t(range.start) + 1; i < range.end; ++i) {
    const float w = measured_.widths[i];
    if (w == 0) continue;  // not a grapheme boundary
    if (width + w - estimated_letter_spacing > line_width_limit_ ||
        overhang_exceed_line_limit(Range{range.start, size_t(i) + 1})) {
      // The longest piece that fits; the rest of the word may be hyphenated.
      break_line_at(i, width, float(line_width_ - width), float(sum_of_char_widths_ - width), EndHyphenEdit::kNoEdit,
                    StartHyphenEdit::kNoEdit);
      return false;
    }
    width += w;
  }
  // Not even one cluster fits: break at the end of the range.
  break_line_at(uint32_t(range.end), float(line_width_), 0, 0, EndHyphenEdit::kNoEdit, StartHyphenEdit::kNoEdit);
  return true;
}

bool GreedyLineBreaker::do_line_break_with_fallback(Range range) {
  const TextRun* target_run = run_containing(measured_, range);
  if (target_run == nullptr) return false;
  if (target_run->line_break_word_style == LineBreakWordStyle::kNone) return false;  // nothing to fall back to

  WordBreaker wb;
  wb.set_text(text_);
  int64_t next = wb.following_with_locale(effective_locale(target_run->locale_list_id()),
                                          target_run->line_break_style, LineBreakWordStyle::kNone, range.start);
  if (!range.contains(size_t(uint32_t(next)))) return false;  // no fallback break

  int32_t prev_break = -1;
  float word_width = 0;
  float pre_break_width = 0;
  for (uint32_t i = uint32_t(range.start); i < range.end; ++i) {
    const float w = measured_.widths[i];
    if (w == 0) continue;
    if (i == uint32_t(next)) {
      if (pre_break_width + word_width > line_width_limit_) {
        if (prev_break == -1) return false;
        break_line_at(uint32_t(prev_break), pre_break_width, float(line_width_ - pre_break_width),
                      float(sum_of_char_widths_ - pre_break_width), EndHyphenEdit::kNoEdit, StartHyphenEdit::kNoEdit);
        return true;
      }
      prev_break = int32_t(i);
      next = wb.next();
      pre_break_width += word_width;
      word_width = w;
    } else {
      word_width += w;
    }
  }
  if (pre_break_width <= line_width_limit_ && prev_break != -1) {  // minikin would break at offset -1
    break_line_at(uint32_t(prev_break), pre_break_width, float(line_width_ - pre_break_width),
                  float(sum_of_char_widths_ - pre_break_width), EndHyphenEdit::kNoEdit, StartHyphenEdit::kNoEdit);
    return true;
  }
  return false;
}

void GreedyLineBreaker::update_line_width(char16_t c, float width) {
  if (c == kCharTab) {
    sum_of_char_widths_ = tab_stops_.next_tab(float(sum_of_char_widths_));
    line_width_ = sum_of_char_widths_;
  } else {
    sum_of_char_widths_ += width;
    if (!is_line_end_space(c)) line_width_ = sum_of_char_widths_;
  }
}

bool GreedyLineBreaker::overhang_exceed_line_limit(Range range) {
  if (!use_bounds_for_width_) return false;
  if (!measured_.has_overhang(range.start, range.end)) return false;
  size_t i;
  for (i = 0; i < range.length(); ++i) {
    if (!is_line_end_space(text_[range.end - i - 1])) break;
  }
  if (i == range.length()) return false;
  TextRect b = measured_.bounds(text_, range.start, range.end - i);
  return b.right - b.left > line_width_limit_;
}

bool GreedyLineBreaker::is_width_exceeded() const {
  // The letter spacing of the line's edges, estimated from the previous
  // break and the current one (an exact one would cost a bidi resolution
  // per break opportunity).
  const float estimated_letter_spacing = float((line_start_letter_spacing_ + current_letter_spacing_) * 0.5);
  return (line_width_ - estimated_letter_spacing) > line_width_limit_;
}

void GreedyLineBreaker::process_line_break(uint32_t offset, WordBreaker* breaker, bool do_hyphenation) {
  while (is_width_exceeded() || overhang_exceed_line_limit(Range{prev_line_break_offset(), offset})) {
    if (try_line_break_with_word_break()) continue;  // the new line's word may still be too long
    if (do_hyphenation && try_line_break_with_hyphenation(Range{prev_line_break_offset(), offset}, breaker)) continue;
    if (do_line_break_with_fallback(Range{prev_line_break_offset(), offset})) continue;
    if (do_line_break_with_grapheme_bounds(Range{prev_line_break_offset(), offset})) return;
  }
  // Room left: this word break is the candidate until the next word.
  const bool is_in_email_or_url = breaker->break_badness() != 0;
  if (prev_word_bounds_offset_ == kNowhere || is_prev_word_break_in_email_or_url_ | !is_in_email_or_url) {
    prev_word_bounds_offset_ = offset;
    line_width_at_prev_word_boundary_ = line_width_;
    sum_of_char_widths_at_prev_word_boundary_ = sum_of_char_widths_;
    is_prev_word_break_in_email_or_url_ = is_in_email_or_url;
  }
}

void GreedyLineBreaker::process(bool force_word_style_auto_to_phrase) {
  WordBreaker word_breaker;
  word_breaker.set_text(text_);
  WordBreakerTransitionTracker tracker;
  uint32_t next_word_boundary_offset = 0;
  for (size_t run_index = 0; run_index < measured_.runs.size(); ++run_index) {
    const TextRun& run = measured_.runs[run_index];
    current_letter_spacing_ = run.letter_spacing_px();
    if (run_index == 0) line_start_letter_spacing_ = current_letter_spacing_;
    if (tracker.update(run)) {
      const LocaleList& locales = locale_list(tracker.locale_list_id());
      const Locale locale = locales.empty() ? Locale() : locales[0];
      LineBreakWordStyle word_style = tracker.word_style();
      std::tie(word_style, retry_with_phrase_word_break) =
          resolve_word_style_auto(word_style, locales, force_word_style_auto_to_phrase);
      next_word_boundary_offset =
          uint32_t(word_breaker.following_with_locale(locale, run.line_break_style, word_style, run.start));
      hyphenator_ = &Hyphenator::lookup(locale);
    }
    for (uint32_t i = run.start; i < run.end; ++i) {
      update_line_width(text_[i], measured_.widths[i]);
      if (i + 1 == next_word_boundary_offset) {
        // Breaks at word boundaries only, where the run can break.
        if (run.can_break() || next_word_boundary_offset == run.end) {
          process_line_break(i + 1, &word_breaker, run.can_break());
        }
        next_word_boundary_offset = uint32_t(word_breaker.next());
      }
    }
  }
  if (prev_line_break_offset() != text_.size() && prev_word_bounds_offset_ != kNowhere) {
    // The words left make the last line.
    break_line_at(prev_word_bounds_offset_, float(line_width_), 0, 0, EndHyphenEdit::kNoEdit,
                  StartHyphenEdit::kNoEdit);
  }
}

LineBreakResult GreedyLineBreaker::result() const {
  LineBreakResult out;
  uint32_t prev_break_offset = 0;
  for (const BreakPoint& bp : break_points_) {
    bool has_tab_char = false;
    for (uint32_t i = prev_break_offset; i < bp.offset; ++i) has_tab_char |= text_[i] == kCharTab;
    push_line_metrics(text_, measured_, Range{prev_break_offset, bp.offset}, bp.line_width, use_bounds_for_width_,
                      &out);
    out.break_points.push_back(int32_t(bp.offset));
    out.widths.push_back(bp.line_width);
    out.flags.push_back((has_tab_char ? LineBreakResult::kTabBit : 0) | bp.hyphen_edit);
    prev_break_offset = bp.offset;
  }
  return out;
}

LineBreakResult break_line_greedy(std::u16string_view text, const MeasuredText& measured, const LineWidths& limits,
                                  const TabStops& tab_stops, bool enable_hyphenation, bool use_bounds_for_width) {
  if (text.empty()) return {};
  GreedyLineBreaker breaker(text, measured, limits, tab_stops, enable_hyphenation, use_bounds_for_width);
  breaker.process(false);
  LineBreakResult res = breaker.result();
  if (!breaker.retry_with_phrase_word_break) return res;
  if (res.break_points.size() >= kAutoHeuristicsLineCount) return res;
  GreedyLineBreaker phrase(text, measured, limits, tab_stops, enable_hyphenation, use_bounds_for_width);
  phrase.process(true);
  LineBreakResult res2 = phrase.result();
  return res2.break_points.size() < kAutoHeuristicsLineCount ? std::move(res2) : std::move(res);
}

// --- OptimalLineBreaker ---

// Scores in a hierarchy: desperate breaks before an overfull line.
constexpr float kScoreInfty = std::numeric_limits<float>::max();
constexpr float kScoreOverfull = 1e12f;
constexpr float kScoreDesperate = 1e10f;
constexpr float kScoreFallback = 1e6f;
constexpr float kLastLinePenaltyMultiplier = 4.0f;
constexpr float kLinePenaltyMultiplier = 2.0f;
constexpr float kShrinkPenaltyMultiplier = 4.0f;
constexpr float kShrinkability = 1.0 / 3.0;

struct Candidate {
  uint32_t offset;
  float pre_break;   // width up to here if the line does not break here
  float post_break;  // width up to here if it does
  float penalty;
  uint32_t pre_space_count;
  uint32_t post_space_count;
  HyphenationType hyphen_type;
  bool rtl;
};

struct OptimizeContext {
  std::vector<Candidate> candidates;
  float line_penalty = 0.0f;
  float space_width = 0.0f;
  bool retry_with_phrase_word_break = false;
  float max_char_width = 0.0f;

  explicit OptimizeContext(float first_letter_spacing) {
    push_word_break(0, 0, 0, 0, 0, 0, false, first_letter_spacing);
  }
  void push_desperate(uint32_t offset, float sum_of_char_widths, float score, uint32_t space_count, bool rtl,
                      float letter_spacing) {
    push(offset, sum_of_char_widths, sum_of_char_widths, score, space_count, space_count,
         HyphenationType::kBreakAndDontInsertHyphen, rtl, letter_spacing);
  }
  void push_hyphenation(uint32_t offset, float pre_break, float post_break, float penalty, uint32_t space_count,
                        HyphenationType type, bool rtl, float letter_spacing) {
    push(offset, pre_break, post_break, penalty, space_count, space_count, type, rtl, letter_spacing);
  }
  void push_word_break(uint32_t offset, float pre_break, float post_break, float penalty, uint32_t pre_space_count,
                       uint32_t post_space_count, bool rtl, float letter_spacing) {
    push(offset, pre_break, post_break, penalty, pre_space_count, post_space_count, HyphenationType::kDontBreak, rtl,
         letter_spacing);
  }

 private:
  // Half the letter spacing on each side: the line loses the spacing of
  // its two edges (estimated with the run at each end).
  void push(uint32_t offset, float pre_break, float post_break, float penalty, uint32_t pre_space_count,
            uint32_t post_space_count, HyphenationType type, bool rtl, float letter_spacing) {
    const float half = float(letter_spacing * 0.5);
    candidates.push_back(Candidate{offset, pre_break + half, post_break - half, penalty, pre_space_count,
                                   post_space_count, type, rtl});
  }
};

// computePenalties: (hyphen penalty, line penalty).
std::pair<float, float> compute_penalties(const TextRun& run, const LineWidths& widths,
                                          HyphenationFrequency frequency, bool justified) {
  float line_penalty = 0.0;
  float hyphen_penalty = float(0.5 * run.paint->size * run.paint->scale_x * widths.at(0));
  if (frequency == HyphenationFrequency::kNormal) hyphen_penalty *= 4.0;
  if (justified) {
    hyphen_penalty *= 0.25;  // "normal" justified is "full" ragged-right
  } else {
    line_penalty = hyphen_penalty * kLinePenaltyMultiplier;
  }
  return {hyphen_penalty, line_penalty};
}

struct DesperateBreak {
  uint32_t offset;
  float sum_of_chars;  // from the start of the word
  float score;
};

std::vector<DesperateBreak> populate_desperate_points(std::u16string_view text, const MeasuredText& measured,
                                                      Range range, const TextRun& run) {
  std::vector<DesperateBreak> out;
  WordBreaker wb;
  wb.set_text(text);
  int64_t next = wb.following_with_locale(effective_locale(run.locale_list_id()), run.line_break_style,
                                          LineBreakWordStyle::kNone, range.start);
  const bool calculate_fallback = range.contains(size_t(uint32_t(next)));
  float width = measured.widths[range.start];
  for (uint32_t i = uint32_t(range.start) + 1; i < range.end; ++i) {
    const float w = measured.widths[i];
    if (w == 0) continue;  // not a grapheme boundary
    if (calculate_fallback && i == uint32_t(next)) {
      out.push_back(DesperateBreak{i, width, kScoreFallback});
      next = wb.next();
      if (!range.contains(size_t(uint32_t(next)))) break;
    } else {
      out.push_back(DesperateBreak{i, width, kScoreDesperate});
    }
    width += w;
  }
  return out;
}

// appendWithMerging: desperate breaks first at a shared offset (they make
// the shorter line, and the DP expects widths growing with offsets).
void append_with_merging(std::vector<HyphenBreak>::const_iterator hy, std::vector<HyphenBreak>::const_iterator hy_end,
                         const std::vector<DesperateBreak>& desperates, const CharProcessor& proc,
                         float hyphen_penalty, bool rtl, float letter_spacing, OptimizeContext* out) {
  auto d = desperates.begin();
  while (hy != hy_end || d != desperates.end()) {
    if (d != desperates.end() && (hy == hy_end || d->offset <= hy->offset)) {
      out->push_desperate(d->offset, proc.sum_of_char_widths_at_prev_word_break + d->sum_of_chars, d->score,
                          proc.effective_space_count, rtl, letter_spacing);
      d++;
    } else {
      out->push_hyphenation(hy->offset, proc.sum_of_char_widths - hy->second,
                            proc.sum_of_char_widths_at_prev_word_break + hy->first, hyphen_penalty,
                            proc.effective_space_count, hy->type, rtl, letter_spacing);
      hy++;
    }
  }
}

OptimizeContext populate_candidates(std::u16string_view text, const MeasuredText& measured, const LineWidths& widths,
                                    HyphenationFrequency frequency, bool justified,
                                    bool force_word_style_auto_to_phrase) {
  const float min_line_width = widths.min();
  CharProcessor proc(text);
  OptimizeContext result(measured.runs.empty() ? 0.0f : measured.runs[0].letter_spacing_px());
  const bool do_hyphenation = frequency != HyphenationFrequency::kNone;
  auto hy = measured.hyphen_breaks.cbegin();

  for (const TextRun& run : measured.runs) {
    const float letter_spacing = run.letter_spacing_px();
    float hyphen_penalty = 0.0f;
    if (run.can_break()) {
      auto penalties = compute_penalties(run, widths, frequency, justified);
      hyphen_penalty = penalties.first;
      result.line_penalty = std::max(penalties.second, result.line_penalty);
    }
    proc.update_locale_if_necessary(run, force_word_style_auto_to_phrase);
    for (uint32_t i = run.start; i < run.end; ++i) {
      // The end of a run is a candidate even in a run that cannot break.
      const bool can_break = run.can_break() || (i + 1) == run.end;
      proc.feed_char(i, text[i], measured.widths[i], can_break);
      const uint32_t next_char_offset = i + 1;
      if (next_char_offset != proc.next_word_break) continue;  // wait for a word break

      std::vector<DesperateBreak> desperate_breaks;
      const Range context_range = proc.context_range();
      auto begin_hy = hy;
      while (hy != measured.hyphen_breaks.cend() && hy->offset < context_range.end) hy++;
      if (proc.width_from_last_word_break() > min_line_width) {
        desperate_breaks = populate_desperate_points(text, measured, context_range, run);
      }
      const bool do_hyphenation_run = do_hyphenation && run.can_hyphenate();
      append_with_merging(begin_hy, do_hyphenation_run ? hy : begin_hy, desperate_breaks, proc, hyphen_penalty,
                          run.rtl, letter_spacing, &result);
      // No breaks for the zero-width characters of replacement spans.
      if (run.paint || next_char_offset == run.end || measured.widths[next_char_offset] > 0) {
        const float penalty = hyphen_penalty * float(proc.word_break_penalty());
        result.push_word_break(next_char_offset, proc.sum_of_char_widths, proc.effective_width, penalty,
                               proc.raw_space_count, proc.effective_space_count, run.rtl, letter_spacing);
      }
    }
  }
  result.space_width = proc.space_width;
  result.retry_with_phrase_word_break = proc.retry_with_phrase_word_break;
  result.max_char_width = proc.max_char_width;
  return result;
}

struct OptimalBreaksData {
  float score;           // best score for this break
  uint32_t prev;         // previous break
  uint32_t line_number;  // of the candidate
};

LineBreakResult finish_breaks_optimal(std::u16string_view text, const MeasuredText& measured,
                                      const std::vector<OptimalBreaksData>& breaks,
                                      const std::vector<Candidate>& candidates, bool use_bounds_for_width) {
  LineBreakResult result;
  const uint32_t n = uint32_t(candidates.size());
  uint32_t prev_index;
  for (uint32_t i = n - 1; i > 0; i = prev_index) {
    prev_index = breaks[i].prev;
    const Candidate& cand = candidates[i];
    const Candidate& prev = candidates[prev_index];
    result.break_points.push_back(int32_t(cand.offset));
    result.widths.push_back(cand.post_break - prev.pre_break);
    push_line_metrics(text, measured, Range{prev.offset, cand.offset}, cand.post_break - prev.pre_break,
                      use_bounds_for_width, &result);
    result.flags.push_back(pack_hyphen_edit(edit_for_next_line(prev.hyphen_type), edit_for_this_line(cand.hyphen_type)));
  }
  result.reverse();
  return result;
}

LineBreakResult compute_breaks(const OptimizeContext& context, std::u16string_view text, const MeasuredText& measured,
                               const LineWidths& widths, BreakStrategy strategy, bool justified,
                               bool use_bounds_for_width) {
  const std::vector<Candidate>& candidates = context.candidates;
  uint32_t active = 0;
  const uint32_t n = uint32_t(candidates.size());
  const float max_shrink = justified ? kShrinkability * context.space_width : 0.0f;

  std::vector<OptimalBreaksData> breaks;
  breaks.reserve(n);
  breaks.push_back({0.0, 0, 0});  // the first candidate is on the first line

  const float delta_max = context.max_char_width * 2;
  for (uint32_t i = 1; i < n; i++) {  // i: the end of the line
    const bool at_end = i == n - 1;
    float best = kScoreInfty;
    uint32_t best_prev = 0;
    uint32_t line_number_last = breaks[active].line_number;
    float width = widths.at(line_number_last);
    float left_edge = candidates[i].post_break - width;
    float best_hope = 0;

    for (uint32_t j = active; j < i; j++) {  // j: the start of the line
      const uint32_t line_number = breaks[j].line_number;
      if (line_number != line_number_last) {
        const float width_new = widths.at(line_number);
        if (width_new != width) {
          left_edge = candidates[i].post_break - width;
          best_hope = 0;
          width = width_new;
        }
        line_number_last = line_number;
      }
      const float j_score = breaks[j].score;
      if (j_score + best_hope >= best) continue;
      float delta = candidates[j].pre_break - left_edge;

      // Bounds keep glyphs from being clipped; only when the advance does
      // not overshoot and the margin is under two widest characters.
      if (use_bounds_for_width && 0 <= delta && delta < delta_max) {
        if (candidates[i].hyphen_type == HyphenationType::kDontBreak &&
            candidates[j].hyphen_type == HyphenationType::kDontBreak) {
          Range range{candidates[j].offset, candidates[i].offset};
          Range actual = trim_trailing_line_end_spaces(text, range);
          if (!actual.empty() && measured.has_overhang(range.start, range.end)) {
            TextRect b = measured.bounds(text, actual.start, actual.end);
            float bounds_delta = width - (b.right - b.left);
            if (bounds_delta < 0) delta = bounds_delta;
          }
        }
      }

      float width_score = 0.0f;
      float additional_penalty = 0.0f;
      if ((at_end || !justified) && delta < 0) {
        width_score = kScoreOverfull;
      } else if (at_end && strategy != BreakStrategy::kBalanced) {
        additional_penalty = kLastLinePenaltyMultiplier * candidates[j].penalty;  // hyphen on the last line
      } else {
        width_score = delta * delta;
        if (delta < 0) {
          if (-delta < max_shrink * float(candidates[i].post_space_count - candidates[j].pre_space_count)) {
            width_score *= kShrinkPenaltyMultiplier;
          } else {
            width_score = kScoreOverfull;
          }
        }
      }
      if (delta < 0) {
        active = j + 1;
      } else {
        best_hope = width_score;
      }
      const float score = j_score + width_score + additional_penalty;
      if (score <= best) {
        best = score;
        best_prev = j;
      }
    }
    breaks.push_back({best + candidates[i].penalty + context.line_penalty, best_prev,
                      breaks[best_prev].line_number + 1});
  }
  return finish_breaks_optimal(text, measured, breaks, candidates, use_bounds_for_width);
}

LineBreakResult break_line_optimal(std::u16string_view text, const MeasuredText& measured, const LineWidths& widths,
                                   BreakStrategy strategy, HyphenationFrequency frequency, bool justified,
                                   bool use_bounds_for_width) {
  if (text.empty()) return {};
  const OptimizeContext context = populate_candidates(text, measured, widths, frequency, justified, false);
  LineBreakResult res = compute_breaks(context, text, measured, widths, strategy, justified, use_bounds_for_width);
  if (!context.retry_with_phrase_word_break) return res;
  if (res.break_points.size() >= kAutoHeuristicsLineCount) return res;
  const OptimizeContext phrase = populate_candidates(text, measured, widths, frequency, justified, true);
  LineBreakResult res2 = compute_breaks(phrase, text, measured, widths, strategy, justified, use_bounds_for_width);
  return res2.break_points.size() < kAutoHeuristicsLineCount ? std::move(res2) : std::move(res);
}

}  // namespace

float TabStops::next_tab(float width_so_far) const {
  for (float stop : stops) {
    if (stop > width_so_far) return stop;
  }
  if (tab_width == 0) return 0;
  return std::floor(width_so_far / tab_width + 1) * tab_width;
}

float LineWidths::at(size_t line) const {
  const float width = int64_t(line) < int64_t(first_line_count) ? first_width : rest_width;
  float indent = 0;
  if (!indents.empty()) {
    const size_t index = line + size_t(int64_t(indents_offset));
    indent = index < indents.size() ? indents[index] : indents.back();
  }
  return std::max(0.0f, width - indent);
}

float LineWidths::min() const {
  float m = std::min(at(0), at(size_t(int64_t(first_line_count))));
  for (size_t line = 1; line + size_t(int64_t(indents_offset)) < indents.size(); line++) m = std::min(m, at(line));
  return m;
}

void LineBreakResult::reverse() {
  std::reverse(break_points.begin(), break_points.end());
  std::reverse(widths.begin(), widths.end());
  std::reverse(ascents.begin(), ascents.end());
  std::reverse(descents.begin(), descents.end());
  std::reverse(flags.begin(), flags.end());
  std::reverse(bounds.begin(), bounds.end());
}

LineBreakResult break_into_lines(std::u16string_view text, BreakStrategy strategy, HyphenationFrequency frequency,
                                 bool justified, const MeasuredText& measured, const LineWidths& widths,
                                 const TabStops& tab_stops, bool use_bounds_for_width) {
  if (strategy == BreakStrategy::kGreedy || text.find(kCharTab) != std::u16string_view::npos) {
    return break_line_greedy(text, measured, widths, tab_stops, frequency != HyphenationFrequency::kNone,
                             use_bounds_for_width);
  }
  return break_line_optimal(text, measured, widths, strategy, frequency, justified, use_bounds_for_width);
}

}  // namespace aa::text
