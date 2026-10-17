#pragma once

// The bridge's graphics objects as Skia sees them (ADR-008), for the natives
// that draw: HWUI's Paint (an SkPaint, an SkFont and the rest drawing reads),
// paths, regions, colour spaces and the typefaces of the text engine's faces.
#include <cstdint>
#include <memory>
#include <optional>

#include "framework-bridge/internal.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkTypeface.h"
#include "text/layout.h"

namespace aa::bridge {

// GraphicsJNI::convertColorLong: a Color long (sRGB with 8-bit channels, or
// half floats and a 10-bit alpha).
SkColor4f color4f_of_long(int64_t color);
// GraphicsJNI::getNativeColorSpace: the SkColorSpace of a ColorSpace.Rgb
// native handle; nullptr (sRGB) for 0.
sk_sp<SkColorSpace> skia_color_space(rt::Runtime& rt, int64_t handle);

// HWUI's BlurDrawLooper (Paint.setShadowLayer): the shadow is drawn first,
// offset, in its colour and blurred, then the original.
struct ShadowLooper {
  SkColor4f color;
  float sigma = 0;
  SkPoint offset{0, 0};
};

// HWUI's Paint.
struct SkiaPaint {
  SkPaint paint;
  SkFont font;  // no typeface: populateSkFont sets it per font run
  std::optional<ShadowLooper> looper;
  bool filter_bitmap = false, underline = false, strike_thru = false;
  int32_t align = 0;  // Paint.Align: LEFT, CENTER, RIGHT
  // The text attributes (skia_paint_of with with_text only).
  text::Paint minikin;
  text::StartHyphenEdit start_hyphen = text::StartHyphenEdit::kNoEdit;
  text::EndHyphenEdit end_hyphen = text::EndHyphenEdit::kNoEdit;
  text::RunFlag run_flag = text::kRunNone;

  SkiaPaint() { font.setEdging(SkFont::Edging::kAlias); }  // Paint::Paint
  bool nothing_to_draw() const { return !looper && paint.nothingToDraw(); }
  SkFilterMode filter_mode() const { return filter_bitmap ? SkFilterMode::kLinear : SkFilterMode::kNearest; }
  SkSamplingOptions sampling() const { return SkSamplingOptions(filter_mode()); }
  // Paint::getJavaFlags and setJavaFlags (PaintFlagsDrawFilter).
  int32_t java_flags() const;
  void set_java_flags(int32_t flags);
};

// The paint of a Paint handle; 0 gives HWUI's default Paint.
SkiaPaint skia_paint_of(rt::Runtime& rt, int64_t paint, bool with_text = false);
// The effects a Paint refers to (framework_effects.cpp): nullptr for 0,
// IllegalStateException for an invalid handle.
sk_sp<SkShader> skia_shader(rt::Runtime& rt, int64_t shader);
sk_sp<SkColorFilter> skia_color_filter(rt::Runtime& rt, int64_t filter);
sk_sp<SkMaskFilter> skia_mask_filter(rt::Runtime& rt, int64_t filter);
sk_sp<SkPathEffect> skia_path_effect(rt::Runtime& rt, int64_t effect);
// Bitmap::makeImage: an SkImage over a Bitmap handle's pixels, shared (not
// copied) and kept alive by the image; nullptr for an empty bitmap.
sk_sp<SkImage> skia_bitmap_image(rt::Runtime& rt, int64_t bitmap);
// A Path handle's SkPath (IllegalStateException if invalid).
SkPath skia_path_of(rt::Runtime& rt, int64_t path);
// A Region handle's SkRegion (IllegalStateException if invalid).
SkRegion skia_region_of(rt::Runtime& rt, int64_t region);
// The FreeType SkTypeface of a text engine face, as HWUI's MinikinFontSkia
// holds one (SkFontMgr_New_Custom_Empty): same bytes, collection index and
// variation coordinates. Cached; nullptr when Skia rejects the font.
sk_sp<SkTypeface> skia_typeface(const std::shared_ptr<text::Face>& face);
// The SkTypeface of a Font handle (framework_fonts.cpp); nullptr if invalid or rejected.
sk_sp<SkTypeface> font_skia_typeface(rt::Runtime& rt, int64_t handle);

}  // namespace aa::bridge
