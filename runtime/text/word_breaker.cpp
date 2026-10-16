// minikin's WordBreaker.cpp, Hyphenator.cpp (without patterns),
// HyphenatorMap.cpp and LineBreakerUtil.{h,cpp}. ICU's line break iterator
// comes from UnicodeServices: its boundaries are computed once per text and
// locale, then walked as ubrk_following/ubrk_next would.
#include "text/word_breaker.h"

#include <hb.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include "text/unicode.h"

namespace aa::text {

namespace {

constexpr char32_t kCharZwj = 0x200D, kCharHyphenMinus = 0x002D, kCharHyphen = 0x2010;

constexpr uint32_t tag4(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

bool has_script(const LocaleList& list, uint32_t script_tag) {
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].supports_script(script_tag)) return true;
  }
  return false;
}

// Locale::getStringWithLineBreakOption.
std::string line_break_tag(const Locale& locale, LineBreakStyle style, LineBreakWordStyle word_style) {
  std::string tag = locale.str();
  if (style != LineBreakStyle::kNone || word_style != LineBreakWordStyle::kNone) tag += "-u";
  switch (style) {
    case LineBreakStyle::kLoose: tag += "-lb-loose"; break;
    case LineBreakStyle::kNormal: tag += "-lb-normal"; break;
    case LineBreakStyle::kStrict: tag += "-lb-strict"; break;
    default: break;
  }
  if (word_style == LineBreakWordStyle::kPhrase) tag += "-lw-phrase";
  return tag;
}

// The boundaries of the last texts broken, by locale tag and text: the line
// breakers break the same paragraph several times (desperate breaks,
// fallbacks), and each ICU walk costs calls into the interpreter.
class BoundaryCache {
 public:
  std::shared_ptr<const std::vector<uint32_t>> get(std::u16string_view text, const std::string& tag) {
    std::string key = tag;
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(text.data()), text.size() * 2);
    {
      std::lock_guard<std::mutex> guard(lock_);
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
          entries_.splice(entries_.begin(), entries_, it);
          return entries_.front().second;
        }
      }
    }
    auto bounds = std::make_shared<std::vector<uint32_t>>();
    if (!unicode_services().line_boundaries(text, tag, bounds.get()) || bounds->empty() || bounds->front() != 0 ||
        bounds->back() != text.size()) {
      // No iterator: breaks at the ends of the text only.
      bounds->assign(1, 0);
      if (!text.empty()) bounds->push_back(uint32_t(text.size()));
    }
    std::lock_guard<std::mutex> guard(lock_);
    entries_.emplace_front(std::move(key), bounds);
    if (entries_.size() > kMaxEntries) entries_.pop_back();
    return bounds;
  }

 private:
  static constexpr size_t kMaxEntries = 8;
  std::mutex lock_;
  std::list<std::pair<std::string, std::shared_ptr<const std::vector<uint32_t>>>> entries_;
};

BoundaryCache& boundary_cache() {
  static auto* cache = new BoundaryCache;  // never destroyed
  return *cache;
}

// isValidBreak: no break after hyphens (hyphenation handles them), after a
// Myanmar virama, inside emoji ZWJ sequences or before an emoji modifier.
bool is_valid_break(std::u16string_view buf, int64_t i) {
  if (i == WordBreaker::kDone || size_t(i) == buf.size()) return true;
  if (i <= 0) return true;  // minikin would read before the text
  size_t prev_offset = size_t(i);
  char32_t code_point = prev_code_point(buf.data(), 0, &prev_offset);
  if (Hyphenator::is_line_breaking_hyphen(code_point) || code_point == kCharSoftHyphen) return false;
  if (code_point == 0x1039) return false;  // MYANMAR SIGN VIRAMA
  size_t next_offset = size_t(i);
  char32_t next = next_code_point(buf.data(), &next_offset, buf.size());
  if (code_point == kCharZwj && is_emoji(next)) return false;  // LB8
  if (is_emoji_modifier(next)) {                                // LB30b
    if (code_point == 0xFE0F && prev_offset > 0) code_point = prev_code_point(buf.data(), 0, &prev_offset);
    if (is_emoji_base(code_point)) return false;
  }
  return true;
}

// Chicago Manual of Style: breaks after and before these in URLs and emails.
bool break_after(char16_t c) { return c == ':' || c == '=' || c == '&'; }
bool break_before(char16_t c) {
  return c == '~' || c == '.' || c == ',' || c == '-' || c == '_' || c == '?' || c == '#' || c == '%' || c == '=' ||
         c == '&';
}

enum ScanState { kStart, kSawAt, kSawColon, kSawColonSlash, kSawColonSlashSlash };

}  // namespace

// --- ICU's line iterator ---

bool WordBreaker::Iterator::is_boundary(int64_t i) {
  if (no_break || !bounds) return i == 0 || size_t(i) == size;
  if (i < 0) {
    pos = 0;
    return false;
  }
  auto it = std::lower_bound(bounds->begin(), bounds->end(), uint64_t(i));
  pos = size_t(it - bounds->begin());
  if (it != bounds->end() && *it == uint64_t(i)) return true;
  if (it == bounds->end()) pos = bounds->size() - 1;  // beyond the end: left at the end
  return false;
}

int64_t WordBreaker::Iterator::following(int64_t i) {
  if (no_break) return int64_t(size);
  if (!bounds) return kDone;  // no locale given yet
  if (i < 0) {  // first()
    pos = 0;
    return 0;
  }
  if (uint64_t(i) >= size) {
    pos = bounds->size();
    return kDone;
  }
  pos = size_t(std::upper_bound(bounds->begin(), bounds->end(), uint64_t(i)) - bounds->begin());
  return (*bounds)[pos];
}

int64_t WordBreaker::Iterator::next() {
  if (no_break) return int64_t(size);
  if (!bounds || pos + 1 >= bounds->size()) {
    pos = bounds->size();
    return kDone;
  }
  return (*bounds)[++pos];
}

// --- WordBreaker ---

void WordBreaker::set_text(std::u16string_view text) {
  text_ = text;
  has_text_ = true;
  last_ = current_ = scan_offset_ = 0;
  in_email_or_url_ = false;
}

int64_t WordBreaker::following_with_locale(const Locale& locale, LineBreakStyle style, LineBreakWordStyle word_style,
                                           size_t from) {
  if (!has_text_) return current_;
  // ICULineBreakerPool::acquire
  if (style == LineBreakStyle::kAuto) {
    style = locale.supports_script(tag4('J', 'p', 'a', 'n')) ? LineBreakStyle::kStrict : LineBreakStyle::kNone;
  }
  icu_ = Iterator{};
  icu_.size = text_.size();
  if (style == LineBreakStyle::kNoBreak) {
    icu_.no_break = true;
  } else {
    icu_.bounds = boundary_cache().get(text_, line_break_tag(locale, style, word_style));
  }
  no_break_style_ = style == LineBreakStyle::kNoBreak;
  // Inside an email or a URL, the context is kept: only the next break is
  // computed again with the new locale.
  if (!in_email_or_url_) {
    current_ = last_ = scan_offset_ = int64_t(from);
    next();
  }
  return current_;
}

int64_t WordBreaker::iterator_next() {
  int64_t result = icu_.following(current_);
  while (!is_valid_break(text_, result)) result = icu_.next();
  return result;
}

void WordBreaker::detect_email_or_url() {
  if (no_break_style_) {
    in_email_or_url_ = false;
    return;
  }
  if (last_ < scan_offset_) return;
  ScanState state = kStart;
  size_t i;
  for (i = size_t(last_); i < text_.size(); i++) {
    char16_t c = text_[i];
    if (!(' ' < c && c <= 0x007E)) break;  // ASCII only, up to a space
    if (state == kStart && c == '@') {
      state = kSawAt;
    } else if (state == kStart && c == ':') {
      state = kSawColon;
    } else if (state == kSawColon || state == kSawColonSlash) {
      state = c == '/' ? ScanState(int(state) + 1) : kStart;
    }
  }
  if (state == kSawAt || state == kSawColonSlashSlash) {
    // Combining marks and such at the end belong to the address.
    if (!icu_.is_boundary(int64_t(i))) i = size_t(icu_.following(int64_t(i)));
    in_email_or_url_ = true;
  } else {
    in_email_or_url_ = false;
  }
  scan_offset_ = int64_t(i);
}

int64_t WordBreaker::find_next_break_in_email_or_url() {
  char16_t last_char = text_[size_t(last_)];
  int64_t i;
  for (i = last_ + 1; i < scan_offset_; i++) {
    if (break_after(last_char)) break;
    if (last_char == '/' && i >= last_ + 2 && text_[size_t(i - 2)] == '/') break;  // after a double slash
    const char16_t this_char = text_[size_t(i)];
    if (last_char != '-') {  // never after a hyphen
      if (break_before(this_char)) break;
      // before a single slash
      if (this_char == '/' && last_char != '/' && !(i + 1 < scan_offset_ && text_[size_t(i + 1)] == '/')) break;
    }
    last_char = this_char;
  }
  return i;
}

int64_t WordBreaker::next() {
  last_ = current_;
  detect_email_or_url();
  current_ = in_email_or_url_ ? find_next_break_in_email_or_url() : iterator_next();
  return current_;
}

int64_t WordBreaker::word_start() const {
  if (in_email_or_url_) return last_;
  int64_t result = last_;
  while (result < current_) {
    size_t ix = size_t(result);
    char32_t c = next_code_point(text_.data(), &ix, size_t(current_));
    int32_t lb = unicode_services().int_property(c, IntProperty::kLineBreak);
    // Leading punctuation: the OP and QU classes of UAX #14.
    if (!(lb == kLineBreakOpenPunctuation || lb == kLineBreakQuotation)) break;
    result = int64_t(ix);
  }
  return result;
}

int64_t WordBreaker::word_end() const {
  if (in_email_or_url_) return last_;
  int64_t result = current_;
  while (result > last_) {
    size_t ix = size_t(result);
    char32_t c = prev_code_point(text_.data(), size_t(last_), &ix);
    switch (hb_unicode_general_category_t(general_category(c))) {
      case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR:
      case HB_UNICODE_GENERAL_CATEGORY_CONNECT_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_DASH_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_CLOSE_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_OTHER_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
        result = int64_t(ix);
        continue;
      default:
        return result;
    }
  }
  return result;
}

detail::Range WordBreaker::word_range() const {
  const uint32_t start = uint32_t(word_start());
  const uint32_t end = uint32_t(word_end());
  return start < end ? detail::Range{start, end} : detail::Range{end, end};
}

// --- Hyphenator ---

const Hyphenator& Hyphenator::lookup(const Locale&) {
  static const Hyphenator* soft_hyphen_only = new Hyphenator;  // never destroyed
  return *soft_hyphen_only;
}

bool Hyphenator::is_line_breaking_hyphen(char32_t c) {
  return c == 0x002D || c == 0x058A || c == 0x05BE || c == 0x1400 || c == 0x2010 || c == 0x2013 || c == 0x2027 ||
         c == 0x2E17 || c == 0x2E40;
}

namespace {

HyphenationType hyphenation_type_based_on_script(char32_t c) {
  hb_script_t s = hb_script_t(script(c));
  if (s == HB_SCRIPT_KANNADA || s == HB_SCRIPT_MALAYALAM || s == HB_SCRIPT_TAMIL || s == HB_SCRIPT_TELUGU) {
    return HyphenationType::kBreakAndDontInsertHyphen;
  }
  if (s == HB_SCRIPT_ARMENIAN) return HyphenationType::kBreakAndInsertArmenianHyphen;
  if (s == HB_SCRIPT_CANADIAN_SYLLABICS) return HyphenationType::kBreakAndInsertUcasHyphen;
  return HyphenationType::kBreakAndInsertHyphen;
}

int32_t joining_type(char32_t c) { return unicode_services().int_property(c, IntProperty::kJoiningType); }

// getHyphTypeForArabic: whether the letters around a soft hyphen join.
HyphenationType hyph_type_for_arabic(std::u16string_view word, size_t location) {
  int64_t i = int64_t(location);
  int32_t type = kJoiningNon;
  while (size_t(i) < word.size() && (type = joining_type(word[size_t(i)])) == kJoiningTransparent) i++;
  if (type == kJoiningDual || type == kJoiningRight || type == kJoiningCausing) {
    i = int64_t(location) - 2;  // skip the soft hyphen
    type = kJoiningNon;
    while (i >= 0 && (type = joining_type(word[size_t(i)])) == kJoiningTransparent) i--;
    if (type == kJoiningDual || type == kJoiningLeft || type == kJoiningCausing) {
      return HyphenationType::kBreakAndInsertHyphenAndZwj;
    }
  }
  return HyphenationType::kBreakAndInsertHyphen;
}

}  // namespace

void Hyphenator::hyphenate(std::u16string_view word, HyphenationType* out) const {
  // No pattern data: words with hyphens and soft hyphens only.
  hyphenate_with_no_patterns(word, out);
}

// UAX #14's sections 5.3 (hyphens) and 5.4 (soft hyphens).
void Hyphenator::hyphenate_with_no_patterns(std::u16string_view word, HyphenationType* out) const {
  if (word.empty()) return;
  out[0] = HyphenationType::kDontBreak;
  for (size_t i = 1; i < word.size(); i++) {
    const char16_t prev = word[i - 1];
    if (i > 1 && is_line_breaking_hyphen(prev)) {
      if ((prev == kCharHyphenMinus || prev == kCharHyphen) && (lang_ == Lang::kPolish || lang_ == Lang::kSlovenian) &&
          script(word[i]) == HB_SCRIPT_LATIN) {
        out[i] = HyphenationType::kBreakAndInsertHyphenAtNextLine;
      } else {
        out[i] = HyphenationType::kBreakAndDontInsertHyphen;
      }
    } else if (i > 1 && prev == kCharSoftHyphen) {
      out[i] = script(word[i]) == HB_SCRIPT_ARABIC ? hyph_type_for_arabic(word, i)
                                                    : hyphenation_type_based_on_script(word[i]);
    } else if (prev == kCharMiddleDot && min_prefix_ < i && i <= word.size() - min_suffix_ &&
               ((word[i - 2] == 'l' && word[i] == 'l') || (word[i - 2] == 'L' && word[i] == 'L')) &&
               lang_ == Lang::kCatalan) {
      out[i] = HyphenationType::kBreakAndReplaceWithHyphen;  // l·l
    } else {
      out[i] = HyphenationType::kDontBreak;
    }
  }
}

std::vector<HyphenationType> hyphenate(std::u16string_view str, const Hyphenator& hyphenator) {
  // Very long words make hyphenation quadratic: they are not hyphenated.
  constexpr size_t kLongestHyphenatedWord = 45;
  std::vector<HyphenationType> out(str.size(), HyphenationType::kDontBreak);
  bool in_word = false;
  size_t word_start = 0;
  for (size_t i = 0; i <= str.size(); i++) {
    if (i == str.size() || str[i] == kCharNbsp) {
      if (in_word) {
        std::u16string_view word = str.substr(word_start, i - word_start);
        if (word.size() <= kLongestHyphenatedWord) hyphenator.hyphenate(word, out.data() + word_start);
        in_word = false;
      }
    } else if (!in_word) {
      in_word = true;
      word_start = i;
    }
  }
  return out;
}

EndHyphenEdit edit_for_this_line(HyphenationType type) {
  switch (type) {
    case HyphenationType::kBreakAndInsertHyphen: return EndHyphenEdit::kInsertHyphen;
    case HyphenationType::kBreakAndInsertArmenianHyphen: return EndHyphenEdit::kInsertArmenianHyphen;
    case HyphenationType::kBreakAndInsertMaqaf: return EndHyphenEdit::kInsertMaqaf;
    case HyphenationType::kBreakAndInsertUcasHyphen: return EndHyphenEdit::kInsertUcasHyphen;
    case HyphenationType::kBreakAndReplaceWithHyphen: return EndHyphenEdit::kReplaceWithHyphen;
    case HyphenationType::kBreakAndInsertHyphenAndZwj: return EndHyphenEdit::kInsertZwjAndHyphen;
    default: return EndHyphenEdit::kNoEdit;
  }
}

StartHyphenEdit edit_for_next_line(HyphenationType type) {
  switch (type) {
    case HyphenationType::kBreakAndInsertHyphenAtNextLine: return StartHyphenEdit::kInsertHyphen;
    case HyphenationType::kBreakAndInsertHyphenAndZwj: return StartHyphenEdit::kInsertZwj;
    default: return StartHyphenEdit::kNoEdit;
  }
}

// --- LineBreakerUtil.h ---

Locale effective_locale(uint32_t locale_list_id) {
  const LocaleList& list = locale_list(locale_list_id);
  return list.empty() ? Locale() : list[0];
}

bool WordBreakerTransitionTracker::update(const TextRun& run) {
  const uint32_t id = run.locale_list_id();
  const bool changed = locale_list_id_ != id || style_ != run.line_break_style || word_style_ != run.line_break_word_style;
  locale_list_id_ = id;
  style_ = run.line_break_style;
  word_style_ = run.line_break_word_style;
  return changed;
}

std::pair<LineBreakWordStyle, bool> resolve_word_style_auto(LineBreakWordStyle style, const LocaleList& locales,
                                                            bool force_phrase) {
  if (style != LineBreakWordStyle::kAuto) return {style, false};
  if (force_phrase) return {LineBreakWordStyle::kPhrase, false};
  if (has_script(locales, tag4('K', 'o', 'r', 'e'))) return {LineBreakWordStyle::kPhrase, false};
  if (has_script(locales, tag4('J', 'p', 'a', 'n'))) return {LineBreakWordStyle::kNone, true};
  return {LineBreakWordStyle::kNone, false};
}

void CharProcessor::update_locale_if_necessary(const TextRun& run, bool force_word_style_auto_to_phrase) {
  if (!tracker.update(run)) return;
  const LocaleList& locales = locale_list(tracker.locale_list_id());
  const Locale locale = locales.empty() ? Locale() : locales[0];
  LineBreakWordStyle word_style = tracker.word_style();
  std::tie(word_style, retry_with_phrase_word_break) =
      resolve_word_style_auto(word_style, locales, force_word_style_auto_to_phrase);
  next_word_break = uint32_t(breaker.following_with_locale(locale, run.line_break_style, word_style, run.start));
  hyphenator = &Hyphenator::lookup(locale);
}

void CharProcessor::feed_char(uint32_t idx, char16_t c, float w, bool can_break_here) {
  if (idx == next_word_break) {
    if (can_break_here) {
      prev_word_break = next_word_break;
      sum_of_char_widths_at_prev_word_break = sum_of_char_widths;
    }
    next_word_break = uint32_t(breaker.next());
  }
  if (detail::is_word_space(c)) {
    raw_space_count += 1;
    space_width = w;
  }
  sum_of_char_widths += w;
  max_char_width = std::max(max_char_width, w);
  // A line-ending space vanishes at a break: nothing to adjust.
  if (!is_line_end_space(c)) {
    effective_space_count = raw_space_count;
    effective_width = sum_of_char_widths;
  }
}

}  // namespace aa::text
