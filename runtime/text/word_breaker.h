#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "text/layout_internal.h"
#include "text/locale.h"
#include "text/measured_text.h"

// minikin's WordBreaker (WordBreaker.cpp: ICU's line break iterator with
// minikin's corrections, emails and URLs), Hyphenator (Hyphenator.cpp,
// HyphenatorMap.cpp) and the CharProcessor of LineBreakerUtil.h, shared by
// MeasuredText and the line breakers.
namespace aa::text {

constexpr char16_t kCharTab = 0x0009, kCharNbsp = 0x00A0, kCharSoftHyphen = 0x00AD, kCharMiddleDot = 0x00B7;

// isLineEndSpace: [[:General_Category=Space_Separator:]-[:Line_Break=Glue:]]
// plus line feed (all in the BMP).
inline bool is_line_end_space(char16_t c) {
  return c == '\n' || c == ' ' || c == 0x1680 || (0x2000 <= c && c <= 0x200A && c != 0x2007) || c == 0x205F ||
         c == 0x3000;
}

class WordBreaker {
 public:
  static constexpr int64_t kDone = -1;  // UBRK_DONE

  void set_text(std::u16string_view text);
  // The first break after `from`, for the locale and the styles given
  // (LineBreakStyle::kAuto is resolved here, the word style must not be).
  int64_t following_with_locale(const Locale& locale, LineBreakStyle style, LineBreakWordStyle word_style,
                                size_t from);
  int64_t next();
  int64_t current() const { return current_; }
  // The word between the last two breaks, without leading opening
  // punctuation and quotes, nor trailing spaces, punctuation and controls.
  int64_t word_start() const;
  int64_t word_end() const;
  detail::Range word_range() const;
  // 1 inside an email address or a URL, else 0.
  int break_badness() const { return (in_email_or_url_ && current_ < scan_offset_) ? 1 : 0; }

 private:
  // ICU's line BreakIterator over the boundaries it gives (computed once
  // per text and locale), or the iterator of LineBreakStyle::kNoBreak.
  struct Iterator {
    std::shared_ptr<const std::vector<uint32_t>> bounds;
    size_t size = 0;
    bool no_break = false;
    size_t pos = 0;  // index of the current boundary; bounds->size() when done
    bool is_boundary(int64_t i);
    int64_t following(int64_t i);
    int64_t next();
  };

  int64_t iterator_next();
  void detect_email_or_url();
  int64_t find_next_break_in_email_or_url();

  std::u16string_view text_;
  bool has_text_ = false;
  Iterator icu_;
  bool no_break_style_ = false;  // the iterator's resolved style is kNoBreak
  int64_t last_ = 0, current_ = 0, scan_offset_ = 0;
  bool in_email_or_url_ = false;
};

class Hyphenator {
 public:
  // HyphenatorMap::lookup. Without hyphenation patterns (android.text.
  // Hyphenator.init is not run), every locale gets the soft-hyphen-only
  // hyphenator.
  static const Hyphenator& lookup(const Locale& locale);
  // isLineBreakingHyphen: hyphens after which a line may break.
  static bool is_line_breaking_hyphen(char32_t c);
  // One type per code unit of `word`.
  void hyphenate(std::u16string_view word, HyphenationType* out) const;

 private:
  enum class Lang : uint8_t { kOther, kCatalan, kPolish, kSlovenian, kPortuguese };
  void hyphenate_with_no_patterns(std::u16string_view word, HyphenationType* out) const;
  size_t min_prefix_ = 2, min_suffix_ = 2;
  Lang lang_ = Lang::kOther;
};

// LineBreakerUtil's hyphenate: words between no-break spaces, the longest
// ones left whole.
std::vector<HyphenationType> hyphenate(std::u16string_view str, const Hyphenator& hyphenator);

// The first locale of a list, or the empty locale.
Locale effective_locale(uint32_t locale_list_id);

// WordBreakerTransitionTracker and resolveWordStyleAuto.
class WordBreakerTransitionTracker {
 public:
  bool update(const TextRun& run);
  uint32_t locale_list_id() const { return locale_list_id_; }
  LineBreakWordStyle word_style() const { return word_style_; }

 private:
  uint32_t locale_list_id_ = kInvalidLocaleListId;
  LineBreakStyle style_ = LineBreakStyle::kNone;
  LineBreakWordStyle word_style_ = LineBreakWordStyle::kNone;
};
// The word style to break with, and whether to retry with phrases.
std::pair<LineBreakWordStyle, bool> resolve_word_style_auto(LineBreakWordStyle style, const LocaleList& locales,
                                                            bool force_phrase);

// CharProcessor: widths and spaces from the start of the paragraph, and the
// word breaks, character after character.
struct CharProcessor {
  uint32_t raw_space_count = 0;
  uint32_t effective_space_count = 0;
  float sum_of_char_widths = 0;
  float effective_width = 0;
  float sum_of_char_widths_at_prev_word_break = 0;
  uint32_t next_word_break = 0;
  uint32_t prev_word_break = 0;
  float space_width = 0;
  const Hyphenator* hyphenator = nullptr;
  bool retry_with_phrase_word_break = false;
  float max_char_width = 0;

  explicit CharProcessor(std::u16string_view text) { breaker.set_text(text); }
  detail::Range word_range() const { return breaker.word_range(); }
  detail::Range context_range() const { return {prev_word_break, next_word_break}; }
  float width_from_last_word_break() const { return effective_width - sum_of_char_widths_at_prev_word_break; }
  int word_break_penalty() const { return breaker.break_badness(); }
  void update_locale_if_necessary(const TextRun& run, bool force_word_style_auto_to_phrase);
  void feed_char(uint32_t idx, char16_t c, float w, bool can_break_here);

 private:
  WordBreakerTransitionTracker tracker;
  WordBreaker breaker;
};

}  // namespace aa::text
