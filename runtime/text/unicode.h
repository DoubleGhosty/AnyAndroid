#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Unicode for the text engine: what minikin takes from ICU4C. General
// category, script and combining class come from HarfBuzz's own data
// (hb-ucd, the same Unicode version as Android's ICU); what HarfBuzz does
// not have (emoji properties, grapheme cluster breaks, the bidirectional
// algorithm, likely subtags of locales) is asked of UnicodeServices, which
// the bridge implements over the ICU4J of the libcore (android.icu).
namespace aa::text {

// ICU's UGraphemeClusterBreak values.
enum class GraphemeBreakProp : int8_t {
  kOther = 0,
  kControl = 1,
  kCR = 2,
  kExtend = 3,
  kL = 4,
  kLF = 5,
  kLV = 6,
  kLVT = 7,
  kT = 8,
  kV = 9,
  kSpacingMark = 10,
  kPrepend = 11,
  kRegionalIndicator = 12,
  kEBase = 13,
  kEBaseGaz = 14,
  kEModifier = 15,
  kGlueAfterZwj = 16,
  kZwj = 17,
};

enum class EmojiProperty : uint8_t { kEmoji, kEmojiModifier, kEmojiModifierBase, kExtendedPictographic };

// u_getIntPropertyValue properties that minikin reads (UProperty values).
enum class IntProperty : uint32_t { kJoiningType = 0x1007, kLineBreak = 0x1008 };
// ULineBreak and UJoiningType values used by the engine.
constexpr int32_t kLineBreakOpenPunctuation = 20, kLineBreakQuotation = 23;
constexpr int32_t kJoiningNon = 0, kJoiningCausing = 1, kJoiningDual = 2, kJoiningLeft = 3, kJoiningRight = 4,
                  kJoiningTransparent = 5;

// One run of a paragraph in visual order, as ubidi_getVisualRun gives it.
struct BidiRun {
  uint32_t start = 0, length = 0;
  bool rtl = false;
};

class UnicodeServices {
 public:
  virtual ~UnicodeServices() = default;
  // uloc_canonicalize, uloc_addLikelySubtags then uloc_toLanguageTag of one
  // locale ("en_US" -> "en-Latn-US"); "" when no identifier can be built.
  virtual std::string language_tag(std::string_view locale) = 0;
  virtual bool has_property(char32_t c, EmojiProperty p) = 0;
  virtual GraphemeBreakProp grapheme_break(char32_t c) = 0;
  // ubidi_setPara over `text` with `level` (0, 1, 0x7E default LTR, 0x7F
  // default RTL), then its visual runs. Returns false on failure; *para_rtl
  // receives the paragraph direction.
  virtual bool bidi_runs(std::u16string_view text, uint8_t level, std::vector<BidiRun>* runs, bool* para_rtl) = 0;
  virtual int32_t int_property(char32_t c, IntProperty p) = 0;
  // ubrk_open(UBRK_LINE) for the locale of BCP 47 tag `tag` (with its -u-lb-
  // and -u-lw- options) over `text`: every boundary, 0 and text.size()
  // included, in order. Returns false on failure.
  virtual bool line_boundaries(std::u16string_view text, std::string_view tag, std::vector<uint32_t>* out) = 0;
};

// The services in use. Until the embedder installs its own, a fallback
// without ICU data: locale tags kept as written, no emoji property,
// grapheme breaks from the general category only, every paragraph left to
// right, line breaks after spaces only.
UnicodeServices& unicode_services();
void set_unicode_services(UnicodeServices* services);  // nullptr: the fallback

constexpr char32_t kReplacementChar = 0xFFFD;
constexpr char32_t kZwj = 0x200D;

inline bool is_lead_surrogate(char16_t c) { return (c & 0xFC00) == 0xD800; }
inline bool is_trail_surrogate(char16_t c) { return (c & 0xFC00) == 0xDC00; }
inline bool is_surrogate(char32_t c) { return (c & 0xFFFFF800) == 0xD800; }
inline size_t utf16_length(char32_t c) { return c > 0xFFFF ? 2 : 1; }

// U16_NEXT / U16_PREV: a lone surrogate comes out as itself.
char32_t next_code_point(const char16_t* s, size_t* i, size_t end);
char32_t prev_code_point(const char16_t* s, size_t start, size_t* i);

// hb_unicode_general_category values (HarfBuzz's enumeration).
uint8_t general_category(char32_t c);
uint32_t script(char32_t c);  // hb_script_t
uint8_t combining_class(char32_t c);
// u_iscntrl: Cc, Cf, Zl, Zp.
bool is_control(char32_t c);
// U_GC_M_MASK: Mn, Mc, Me.
bool is_combining(char32_t c);
bool is_other_letter(char32_t c);  // Lo
// The first code point of the canonical (one level) decomposition, or c.
char32_t first_of_decomposition(char32_t c);

// minikin's MinikinInternal: VS1-VS16 and VS17-VS256 (not the Mongolian
// free variation selectors); their index 0-255, kInvalidVsIndex otherwise.
constexpr uint16_t kInvalidVsIndex = 0xFFFF;
bool is_variation_selector(char32_t c);
uint16_t vs_index(char32_t c);

// minikin's Emoji.h.
bool is_emoji(char32_t c);
bool is_emoji_modifier(char32_t c);
bool is_emoji_base(char32_t c);
inline bool is_regional_indicator(char32_t c) { return 0x1F1E6 <= c && c <= 0x1F1FF; }
inline bool is_keycap(char32_t c) { return c == 0x20E3; }
inline bool is_tag_char(char32_t c) { return 0xE0020 <= c && c <= 0xE007F; }

}  // namespace aa::text
