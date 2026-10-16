#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "text/font_collection.h"
#include "text/font_face.h"

// minikin's text layout (Layout.cpp, LayoutCore.cpp, Measurement.cpp,
// GraphemeBreak.cpp): text split into bidirectional runs, words and font
// runs, each shaped by HarfBuzz with the advances of HWUI's Skia.
namespace aa::text {

// Paint.BIDI_* values.
enum class Bidi : uint8_t { kLtr = 0, kRtl = 1, kDefaultLtr = 2, kDefaultRtl = 3, kForceLtr = 4, kForceRtl = 5 };

// Hyphen edits, as packed by Paint (start << 3 | end).
enum class StartHyphenEdit : uint8_t { kNoEdit = 0, kInsertHyphen = 1, kInsertZwj = 2 };
enum class EndHyphenEdit : uint8_t {
  kNoEdit = 0,
  kReplaceWithHyphen = 1,
  kInsertHyphen = 2,
  kInsertArmenianHyphen = 3,
  kInsertMaqaf = 4,
  kInsertUcasHyphen = 5,
  kInsertZwjAndHyphen = 6,
};
inline StartHyphenEdit start_hyphen_edit(uint8_t packed) { return StartHyphenEdit(packed >> 3); }
inline EndHyphenEdit end_hyphen_edit(uint8_t packed) { return EndHyphenEdit(packed & 0x7); }

// Where a run sits on its line, for letter spacing at the edges.
enum RunFlag : uint8_t { kRunNone = 0, kRunLeftEdge = 1, kRunRightEdge = 2, kRunWholeLine = 3 };

struct FontFeature {
  uint32_t tag = 0;
  uint32_t value = 0;
  bool operator==(const FontFeature&) const = default;
};
// FontFeature::parse: "liga" 0, "kern", "'smcp' on"... (whole-text ranges only).
std::vector<FontFeature> parse_font_features(std::string_view settings);

// minikin's MinikinPaint, with the SkFont flags HWUI packs into it.
struct Paint {
  std::shared_ptr<const FontCollection> font;
  float size = 0;
  float scale_x = 1;
  float skew_x = 0;
  float letter_spacing = 0;  // in ems
  float word_spacing = 0;    // in pixels
  FontSpec flags;            // hinting, edging and booleans (size, scale and skew unused)
  uint32_t locale_list_id = 0;
  FontStyle font_style;
  FamilyVariant family_variant = FamilyVariant::kDefault;
  bool vertical_text = false;
  std::vector<FontFeature> font_features;
  VariationSettings font_variations;
};

// The SkFont of a faked font for a paint (MinikinFontSkia_SetSkiaFont).
FontSpec skia_font(const Paint& paint, const FontFakery& fakery);

// MinikinExtent: ascent negative, descent positive.
struct Extent {
  float ascent = 0, descent = 0;
  void extend(const Extent& e);
  bool operator==(const Extent&) const = default;
};

// MinikinRect: empty when a side has no length; joining never checks the
// rectangle joined.
struct TextRect {
  float left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return left == right || top == bottom; }
  void join(const TextRect& r, float dx, float dy);
  bool operator==(const TextRect&) const = default;
};

// MinikinFontSkia::GetFontExtent.
Extent font_extent(const FakedFont& font, const Paint& paint);
// FontCollection::getReferenceExtentForLocale.
Extent reference_extent_for_locale(const Paint& paint);

// Layout::measureText over text[start, start + count), with the whole of
// `text` as context: the total advance; per code unit advances, bounds and
// the number of clusters when asked.
float measure_text(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                   StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen, float* advances, TextRect* bounds,
                   uint32_t* cluster_count, RunFlag run_flag);
// minikin's getFontExtent and getBounds.
Extent text_extent(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint);
TextRect text_bounds(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                     StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen);

// A laid out glyph (Layout::doLayout), positions from the start of the text.
struct LayoutGlyph {
  FakedFont font;
  uint32_t glyph = 0;
  uint32_t cluster = 0;
  float x = 0, y = 0;
};
struct TextLayout {
  std::vector<LayoutGlyph> glyphs;
  std::vector<float> advances;
  float advance = 0;
};
TextLayout do_layout(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                     StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen, RunFlag run_flag);

// Measurement.cpp and GraphemeBreak.cpp: cursor positions from advances.
float run_advance(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset);
void distribute_advances(float* advances, const char16_t* buf, size_t start, size_t count);
size_t offset_for_advance(const float* advances, const char16_t* buf, size_t start, size_t count, float advance);
bool is_grapheme_break(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset);
// Paint.CURSOR_* values.
enum class CursorOpt { kAfter = 0, kAtOrAfter = 1, kBefore = 2, kAtOrBefore = 3, kAt = 4 };
size_t text_run_cursor(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset,
                       CursorOpt opt);

}  // namespace aa::text
