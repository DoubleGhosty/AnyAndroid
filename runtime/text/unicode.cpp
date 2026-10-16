#include "text/unicode.h"

#include <hb.h>

#include <atomic>

namespace aa::text {

namespace {

// Without ICU data: what can be derived from HarfBuzz's tables.
class FallbackServices final : public UnicodeServices {
 public:
  std::string language_tag(std::string_view locale) override {
    std::string out(locale);
    for (char& c : out) {
      if (c == '_') c = '-';
    }
    return out;
  }
  bool has_property(char32_t, EmojiProperty) override { return false; }
  GraphemeBreakProp grapheme_break(char32_t c) override {
    using G = GraphemeBreakProp;
    if (c == 0x0D) return G::kCR;
    if (c == 0x0A) return G::kLF;
    if (c == kZwj) return G::kZwj;
    if (c == 0x200C) return G::kExtend;
    if (is_regional_indicator(c)) return G::kRegionalIndicator;
    if ((0x1100 <= c && c <= 0x115F) || (0xA960 <= c && c <= 0xA97C)) return G::kL;
    if ((0x1160 <= c && c <= 0x11A7) || (0xD7B0 <= c && c <= 0xD7C6)) return G::kV;
    if ((0x11A8 <= c && c <= 0x11FF) || (0xD7CB <= c && c <= 0xD7FB)) return G::kT;
    if (0xAC00 <= c && c <= 0xD7A3) return (c - 0xAC00) % 28 == 0 ? G::kLV : G::kLVT;
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), c)) {
      case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
      case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
      case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
      case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
        return G::kControl;
      case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
      case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
        return G::kExtend;
      case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        return G::kSpacingMark;
      default:
        return G::kOther;
    }
  }
  bool bidi_runs(std::u16string_view text, uint8_t level, std::vector<BidiRun>* runs, bool* para_rtl) override {
    bool rtl = level == 1 || level == 0x7F;
    runs->assign(1, BidiRun{0, uint32_t(text.size()), rtl});
    *para_rtl = rtl;
    return true;
  }
  int32_t int_property(char32_t c, IntProperty p) override {
    if (p != IntProperty::kLineBreak) return kJoiningNon;
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), c)) {
      case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION: return kLineBreakOpenPunctuation;
      case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION:
      case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION: return kLineBreakQuotation;
      default: return c == '"' || c == '\'' ? kLineBreakQuotation : 2;  // AL
    }
  }
  // A break before each character that follows white space (UAX #14's LB18
  // alone), and after each line feed.
  bool line_boundaries(std::u16string_view text, std::string_view, std::vector<uint32_t>* out) override {
    out->assign(1, 0);
    auto space = [](char16_t c) { return c == ' ' || c == '\t' || c == 0x3000; };
    for (size_t i = 1; i < text.size(); ++i) {
      if (text[i - 1] == '\n' || (space(text[i - 1]) && !space(text[i]))) out->push_back(uint32_t(i));
    }
    if (!text.empty()) out->push_back(uint32_t(text.size()));
    return true;
  }
};

FallbackServices& fallback() {
  static auto* f = new FallbackServices;
  return *f;
}

std::atomic<UnicodeServices*> g_services{nullptr};

hb_unicode_funcs_t* ucd() { return hb_unicode_funcs_get_default(); }

}  // namespace

UnicodeServices& unicode_services() {
  UnicodeServices* s = g_services.load(std::memory_order_acquire);
  return s ? *s : fallback();
}

void set_unicode_services(UnicodeServices* services) { g_services.store(services, std::memory_order_release); }

char32_t next_code_point(const char16_t* s, size_t* i, size_t end) {
  char32_t c = s[(*i)++];
  if (is_lead_surrogate(char16_t(c)) && *i < end && is_trail_surrogate(s[*i])) {
    c = 0x10000 + ((c - 0xD800) << 10) + (s[(*i)++] - 0xDC00);
  }
  return c;
}

char32_t prev_code_point(const char16_t* s, size_t start, size_t* i) {
  char32_t c = s[--(*i)];
  if (is_trail_surrogate(char16_t(c)) && *i > start && is_lead_surrogate(s[*i - 1])) {
    --(*i);
    c = 0x10000 + ((char32_t(s[*i]) - 0xD800) << 10) + (c - 0xDC00);
  }
  return c;
}

uint8_t general_category(char32_t c) { return uint8_t(hb_unicode_general_category(ucd(), c)); }

uint32_t script(char32_t c) { return uint32_t(hb_unicode_script(ucd(), c)); }

uint8_t combining_class(char32_t c) { return uint8_t(hb_unicode_combining_class(ucd(), c)); }

bool is_control(char32_t c) {
  switch (hb_unicode_general_category(ucd(), c)) {
    case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
    case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
      return true;
    default:
      return false;
  }
}

bool is_combining(char32_t c) {
  switch (hb_unicode_general_category(ucd(), c)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      return true;
    default:
      return false;
  }
}

bool is_other_letter(char32_t c) {
  return hb_unicode_general_category(ucd(), c) == HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER;
}

char32_t first_of_decomposition(char32_t c) {
  hb_codepoint_t a = 0, b = 0;
  if (hb_unicode_decompose(ucd(), c, &a, &b)) return a;
  return c;
}

bool is_variation_selector(char32_t c) {
  return (0xFE00 <= c && c <= 0xFE0F) || (0xE0100 <= c && c <= 0xE01EF);
}

uint16_t vs_index(char32_t c) {
  if (0xFE00 <= c && c <= 0xFE0F) return uint16_t(c - 0xFE00);
  if (0xE0100 <= c && c <= 0xE01EF) return uint16_t(c - 0xE0100 + 16);
  return kInvalidVsIndex;
}

bool is_emoji(char32_t c) { return unicode_services().has_property(c, EmojiProperty::kEmoji); }

bool is_emoji_modifier(char32_t c) { return unicode_services().has_property(c, EmojiProperty::kEmojiModifier); }

bool is_emoji_base(char32_t c) {
  // Removed from Emoji_Modifier_Base in Emoji 4.0, kept by minikin for the
  // fonts and texts that still treat them as bases.
  if (c == 0x1F91D || c == 0x1F93C) return true;
  return unicode_services().has_property(c, EmojiProperty::kEmojiModifierBase);
}

}  // namespace aa::text
