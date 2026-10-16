#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "text/coverage.h"
#include "text/font_face.h"

// minikin's Font, FontFamily and FontCollection (frameworks/minikin, with
// the typeface_redesign_readonly flag on, as in Android 17's image): which
// font of a Typeface draws each character, with which synthetic bold or
// italic and which variation coordinates.
namespace aa::text {

struct FontStyle {
  uint16_t weight = 400;
  bool italic = false;
  bool operator==(const FontStyle&) const = default;
};

// Sorted by tag, one value per tag.
using VariationSettings = std::vector<Variation>;
// Sorts by tag; of repeated tags, the last value is kept.
VariationSettings sorted_variations(std::vector<Variation> v);
struct VariationLess {
  bool operator()(const VariationSettings& a, const VariationSettings& b) const;
};

enum class FamilyVariant : uint8_t { kDefault = 0, kCompact = 1, kElegant = 2 };
enum class VariationFamilyType : uint8_t { kNone = 0, kSingleFontWghtOnly = 1, kSingleFontWghtItal = 2, kTwoFontWght = 3 };

struct FontFakery {
  bool fake_bold = false;
  bool fake_italic = false;
  VariationSettings variations;
  bool operator==(const FontFakery&) const = default;
};

class Font {
 public:
  // Font `index` of a file, at the variation coordinates `axes` (those of
  // Font.Builder). The file is only read for its tables until the font is
  // drawn or measured, then opened by FreeType.
  Font(std::shared_ptr<const FontSource> source, uint32_t index, std::vector<Variation> axes, FontStyle style,
       uint32_t locale_list_id);
  // A font over a face already opened.
  Font(std::shared_ptr<Face> face, FontStyle style, uint32_t locale_list_id);

  // The face (opened on first use); nullptr when FreeType rejects the font.
  std::shared_ptr<Face> face() const;
  const std::vector<Variation>& variations() const { return axes_; }
  std::vector<uint8_t> table(uint32_t tag) const;
  FontStyle style() const { return style_; }
  uint32_t locale_list_id() const { return locale_list_id_; }
  bool axis_supported(uint32_t tag) const { return fvar_axis(tag) != nullptr; }
  const Axis* fvar_axis(uint32_t tag) const;
  // getAdjustedTypeface: the face at `axes`, minus those at their default;
  // nullptr when the font cannot be opened.
  std::shared_ptr<Face> adjusted(const VariationSettings& axes) const;

 private:
  std::shared_ptr<const FontSource> source_;
  uint32_t index_ = 0;
  std::vector<Variation> axes_;
  FontStyle style_;
  uint32_t locale_list_id_;
  mutable std::once_flag fvar_once_;
  mutable std::vector<Axis> fvar_;
  mutable std::mutex lock_;
  mutable bool opened_ = false;
  mutable std::shared_ptr<Face> face_;
  mutable std::map<VariationSettings, std::shared_ptr<Face>, VariationLess> adjusted_;
};

struct FakedFont {
  std::shared_ptr<const Font> font;
  FontFakery fakery;
  std::shared_ptr<Face> face() const { return font->adjusted(fakery.variations); }
  bool operator==(const FakedFont& o) const { return font == o.font && fakery == o.fakery; }
};

class FontFamily {
 public:
  FontFamily(uint32_t locale_list_id, FamilyVariant variant, std::vector<std::shared_ptr<const Font>> fonts,
             bool custom_fallback, bool default_fallback, VariationFamilyType variation_type);
  size_t font_count() const { return fonts_.size(); }
  const std::shared_ptr<const Font>& font(size_t i) const { return fonts_[i]; }
  uint32_t locale_list_id() const { return locale_list_id_; }
  FamilyVariant variant() const { return variant_; }
  bool custom_fallback() const { return custom_fallback_; }
  bool default_fallback() const { return default_fallback_; }
  VariationFamilyType variation_type() const { return variation_type_; }
  bool color_emoji() const { return color_emoji_; }
  // Read from the cmap of the font closest to the default style, on first use.
  const Coverage& coverage() const;
  bool has_vs_table() const;
  bool has_glyph(char32_t c, char32_t selector) const;
  FakedFont closest_match(FontStyle style, const VariationSettings& axes = {}) const;

 private:
  std::vector<std::shared_ptr<const Font>> fonts_;
  uint32_t locale_list_id_;
  FamilyVariant variant_;
  bool custom_fallback_, default_fallback_;
  VariationFamilyType variation_type_;
  bool color_emoji_;
  mutable std::once_flag coverage_once_;
  mutable bool has_vs_table_ = false;
  mutable Coverage coverage_;
};

class FontCollection {
 public:
  explicit FontCollection(std::vector<std::shared_ptr<const FontFamily>> families);
  size_t family_count() const { return families_.size(); }
  const std::shared_ptr<const FontFamily>& family(size_t i) const { return families_[i]; }

  // A run of text drawn by one family (or, for emoji, any of several).
  struct Run {
    std::vector<uint8_t> families;  // indices, best first
    int start = 0, end = 0;
  };
  std::vector<Run> itemize(std::u16string_view text, FontStyle style, uint32_t locale_list_id,
                           FamilyVariant variant, uint32_t run_max = 0xFFFFFFFF) const;
  FakedFont best_font(std::u16string_view text, const Run& run, FontStyle style,
                      const VariationSettings& axes) const;
  FakedFont base_font_faked(FontStyle style) const { return families_[0]->closest_match(style); }
  bool has_variation_selector(char32_t base, char32_t selector) const;

 private:
  std::vector<uint8_t> family_for_char(char32_t c, char32_t vs, uint32_t locale_list_id, FamilyVariant variant) const;
  uint32_t family_score(char32_t c, char32_t vs, FamilyVariant variant, uint32_t locale_list_id,
                        const FontFamily& family) const;
  uint32_t coverage_score(char32_t c, char32_t vs, uint32_t locale_list_id, const FontFamily& family) const;
  bool primary(const FontFamily& family) const;

  std::vector<std::shared_ptr<const FontFamily>> families_;
};

}  // namespace aa::text
