// Skia's CPU raster subset (runtime/third_party/skia), configured as in
// Android: drawing into a bitmap, and the FreeType typefaces HWUI makes
// through SkFontMgr_New_Custom_Empty, measured as the text engine measures.
#include <memory>
#include <span>
#include <vector>

#include "font_builder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"
#include "test.h"
#include "text/font_face.h"

using namespace aa;

namespace {

// Glyphs: .notdef, 'A' (advance 550), 'x' (500, 480 high), as rectangles.
test::TestFont box_font() {
  test::TestFont f;
  f.glyphs = {
      {0, 500, 50, 0, 450, 700},
      {U'A', 550, 10, 0, 540, 700},
      {U'x', 500, 20, 0, 480, 480},
  };
  return f;
}

}  // namespace

TEST(skia_raster_fills_rects_and_antialiased_circles) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(32, 32);
  SkCanvas canvas(bitmap);
  canvas.clear(SK_ColorWHITE);
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  canvas.drawRect(SkRect::MakeLTRB(4, 4, 12, 12), paint);
  CHECK_EQ(bitmap.getColor(8, 8), SK_ColorRED);
  CHECK_EQ(bitmap.getColor(2, 2), SK_ColorWHITE);
  CHECK_EQ(bitmap.getColor(12, 8), SK_ColorWHITE);  // right edge excluded
  paint.setAntiAlias(true);
  paint.setColor(SK_ColorBLUE);
  canvas.drawCircle(24, 24, 5, paint);
  CHECK_EQ(bitmap.getColor(24, 24), SK_ColorBLUE);
  SkColor edge = bitmap.getColor(20, 20);  // partly covered: a mix of white and blue
  CHECK(SkColorGetB(edge) == 0xFF && SkColorGetR(edge) > 0 && SkColorGetR(edge) < 0xFF);
}

TEST(skia_freetype_typefaces_draw_and_measure_like_the_text_engine) {
  test::TestFont tf = box_font();
  std::vector<uint8_t> bytes = tf.build();
  sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Empty();
  sk_sp<SkTypeface> typeface = manager->makeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
  CHECK(typeface != nullptr);
  SkFont font(typeface, 10);
  font.setEdging(SkFont::Edging::kAntiAlias);
  SkGlyphID glyph = font.unicharToGlyph('A');
  CHECK_EQ(glyph, SkGlyphID(1));

  // The engine's own FreeType path (font_face.cpp) gives Skia's advance.
  auto data = std::make_shared<std::vector<uint8_t>>(bytes);
  std::string error;
  auto face = text::Face::open({data, data->data(), data->size()}, 0, {}, &error);
  CHECK(face != nullptr);
  text::FontSpec spec;
  spec.size = 10;
  spec.edging = text::Edging::kAntiAlias;
  for (float size : {10.0f, 13.0f, 20.0f}) {
    font.setSize(size);
    spec.size = size;
    float skia = 0, engine = 0;
    font.getWidths(SkSpan<const SkGlyphID>(&glyph, 1), SkSpan<SkScalar>(&skia, 1));
    uint16_t g = glyph;
    face->advances(spec, std::span<const uint16_t>(&g, 1), std::span<float>(&engine, 1));
    CHECK_EQ(skia, engine);
  }

  SkBitmap bitmap;
  bitmap.allocN32Pixels(32, 32);
  SkCanvas canvas(bitmap);
  canvas.clear(SK_ColorWHITE);
  SkPaint paint;
  font.setSize(20);
  canvas.drawSimpleText("A", 1, SkTextEncoding::kUTF8, 2, 24, font, paint);
  CHECK_EQ(bitmap.getColor(7, 16), SK_ColorBLACK);  // inside the 'A' box
  CHECK_EQ(bitmap.getColor(20, 16), SK_ColorWHITE);
}
