#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct hb_font_t;

// Text engine (ADR-007): fonts read by FreeType and measured as HWUI's Skia
// measures them (SkScalerContext_FreeType), so that advances, metrics and
// glyph bounds match Android's. The engine knows nothing of the Java
// runtime: the bridge gives it font bytes and the attributes of a Paint.
//
// Every call takes one engine-wide lock: a FreeType face is not reentrant,
// and Skia also serialises its FreeType calls.
namespace aa::text {

// The bytes of a font file. `owner` keeps them alive as long as a face uses
// them (the emulated mapping they come from may be released meanwhile).
struct FontData {
  std::shared_ptr<const void> owner;
  const uint8_t* bytes = nullptr;
  size_t size = 0;
};

constexpr uint32_t tag(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint8_t(d);
}

// A point on a variation axis, in design units (wght 700, wdth 75...).
struct Variation {
  uint32_t tag = 0;
  float value = 0;
  bool operator==(const Variation&) const = default;
};

// An axis of the font's fvar table.
struct Axis {
  uint32_t tag = 0;
  float min = 0, def = 0, max = 0;
};

// SkFontHinting and SkFont::Edging.
enum class Hinting : uint8_t { kNone, kSlight, kNormal, kFull };
enum class Edging : uint8_t { kAlias, kAntiAlias, kSubpixelAntiAlias };

// What of an SkFont shapes glyph metrics: size, horizontal scale and skew,
// and the flags HWUI's Paint derives from Java's (Paint::setJavaFlags).
struct FontSpec {
  float size = 12;
  float scale_x = 1;
  float skew_x = 0;
  Hinting hinting = Hinting::kNormal;
  Edging edging = Edging::kAlias;
  bool embolden = false;
  bool linear_metrics = false;
  bool subpixel = false;
  bool embedded_bitmaps = false;
  bool force_autohint = false;
  bool operator==(const FontSpec&) const = default;
};

// SkFontMetrics: y grows downwards (ascent and top are negative).
struct FontMetrics {
  enum : uint32_t {
    kUnderlineThicknessValid = 1 << 0,
    kUnderlinePositionValid = 1 << 1,
    kStrikeoutThicknessValid = 1 << 2,
    kStrikeoutPositionValid = 1 << 3,
    kBoundsInvalid = 1 << 4,
  };
  uint32_t flags = 0;
  float top = 0, ascent = 0, descent = 0, bottom = 0, leading = 0;
  float avg_char_width = 0, max_char_width = 0, x_min = 0, x_max = 0;
  float x_height = 0, cap_height = 0;
  float underline_thickness = 0, underline_position = 0;
  float strikeout_thickness = 0, strikeout_position = 0;
};

struct Rect {
  float left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return !(left < right && top < bottom); }
  bool operator==(const Rect&) const = default;
};

// One font of a file (its index in a collection) at one point of its design
// space: Skia's SkTypeface_FreeType, with minikin's HarfBuzz font.
class Face {
 public:
  // nullptr, with the reason in *error, when FreeType rejects the data.
  // `axes` sets variation coordinates (clamped to the fvar ranges, the last
  // value of a repeated tag wins); the others keep their default.
  static std::shared_ptr<Face> open(FontData data, uint32_t index, std::vector<Variation> axes,
                                    std::string* error);
  ~Face();
  Face(const Face&) = delete;
  Face& operator=(const Face&) = delete;

  // The same font at other coordinates (Skia's makeClone): `axes` apply over
  // this face's own.
  std::shared_ptr<Face> with_variations(const std::vector<Variation>& axes) const;

  const FontData& data() const { return data_; }
  uint32_t index() const { return index_; }
  const std::vector<Variation>& variations() const { return variations_; }
  const std::vector<Axis>& axes() const { return axes_; }
  uint32_t units_per_em() const { return upem_; }
  bool scalable() const { return scalable_; }
  // Whether the font draws with colour bitmaps (CBDT), as emoji fonts do.
  bool color_bitmaps() const { return color_bitmaps_; }
  // OS/2 weight class and italic bit (minikin's analyzeStyle); false when
  // the font has no OS/2 table.
  bool os2_style(int* weight, bool* italic) const;

  // The glyph of a code point (0: none), optionally followed by a variation
  // selector (cmap format 14; 0 when the sequence has no glyph of its own).
  uint32_t glyph(char32_t c) const;
  uint32_t glyph(char32_t c, char32_t selector) const;
  // A table of the font (empty when absent), valid as long as the face.
  std::span<const uint8_t> table(uint32_t tag) const;

  // SkFont::getMetrics: fills `out`, returns the line spacing.
  float metrics(const FontSpec& spec, FontMetrics* out) const;
  // SkFont::getWidths and getBounds.
  void advances(const FontSpec& spec, std::span<const uint16_t> glyphs, std::span<float> out) const;
  void bounds(const FontSpec& spec, std::span<const uint16_t> glyphs, std::span<Rect> out) const;

  // minikin's HarfBuzz font: OpenType functions, scale = units per em, this
  // face's variations. Owned by the face.
  hb_font_t* hb_font() const { return hb_font_; }

 private:
  struct Impl;
  struct Strike;
  Face() = default;
  Strike& strike(const FontSpec& spec) const;

  FontData data_;
  uint32_t index_ = 0;
  std::vector<Variation> variations_;
  std::vector<Axis> axes_;
  uint32_t upem_ = 0;
  bool scalable_ = false;
  bool color_bitmaps_ = false;
  std::unique_ptr<Impl> impl_;
  hb_font_t* hb_font_ = nullptr;
};

}  // namespace aa::text
