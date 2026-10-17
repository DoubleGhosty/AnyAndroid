// android.graphics.Canvas and BaseCanvas natives (G3, J7 of ADR-006,
// ADR-008), each signature checked against the framework.jar of Android 17:
// HWUI's SkiaCanvas over an SkCanvas of Skia, drawing into the pixels of a
// Bitmap or of a window buffer. The save stack keeps HWUI's partial saves
// (SaveFlags without Matrix or Clip, replayed on restore), paints go through
// the canvas's PaintFlagsDrawFilter and the shadow of Paint.setShadowLayer
// (BlurDrawLooper), and text is laid out by the text engine (minikin's
// rules, ADR-007) then drawn as glyph runs (Canvas::drawText,
// DrawTextFunctor). Not yet: clipShader, drawGlyphs, drawMesh and hardware
// layers (RenderNode stays a property store).
#include "framework-bridge/internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/skia_bridge.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkImage.h"
#include "include/core/SkM44.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRSXform.h"
#include "include/core/SkShader.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkVertices.h"
#include "src/core/SkCanvasPriv.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

// Canvas.setCompatibilityVersion (Canvas::sApiLevel).
std::atomic<int32_t> api_level{1};

// SaveFlags: Matrix 1, Clip 2.
constexpr int32_t kSaveMatrix = 1, kSaveClip = 2, kSaveMatrixClip = 3;

// SkiaCanvas::Clip: a clip to replay after restoring a partial save.
struct Clip {
  enum class Type { kRect, kRRect, kPath } type;
  SkClipOp op;
  SkMatrix matrix;
  SkRRect rrect;
  SkPath path;

  void apply(SkCanvas* canvas) const {
    canvas->setMatrix(matrix);
    switch (type) {
      case Type::kRect:
        canvas->clipRect(rrect.rect(), op, false);  // rectangular clips are not antialiased
        break;
      case Type::kRRect:
        canvas->clipRRect(rrect, op, true);
        break;
      case Type::kPath:
        canvas->clipPath(path, op, true);
        break;
    }
  }
};

// PaintFlagsDrawFilter: Java flags cleared then set on every paint.
struct PaintFlagsFilter {
  int32_t clear = 0, set = 0;
};

struct CanvasState {
  std::mutex lock;
  PixelTarget target;  // keeps the pixels alive while the canvas draws into them
  SkBitmap bitmap;
  std::unique_ptr<SkCanvas> canvas;
  struct SaveRec {
    int save_count;
    int32_t flags;
    size_t clip_index;
  };
  std::deque<SaveRec> save_stack;
  std::vector<Clip> clip_stack;
  std::optional<PaintFlagsFilter> filter;

  CanvasState() { set_target(PixelTarget{}); }

  // SkiaCanvas::setBitmap: a new SkCanvas over the pixels.
  void set_target(const PixelTarget& t) {
    target = t;
    bitmap.reset();
    size_t pixels = size_t(std::max(t.width, 0)) * size_t(std::max(t.height, 0));
    if (t.pixels && pixels > 0 && t.pixels->data.size() >= pixels) {
      SkAlphaType at = t.opaque ? kOpaque_SkAlphaType : t.premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
      bitmap.installPixels(SkImageInfo::Make(t.width, t.height, kN32_SkColorType, at, SkColorSpace::MakeSRGB()),
                           t.pixels->data.data(), size_t(t.width) * 4);
    } else {
      target = PixelTarget{};
    }
    canvas = std::make_unique<SkCanvas>(bitmap);
    save_stack.clear();
    clip_stack.clear();
  }

  // Paint filtering (SkiaCanvas::onFilterPaint, PaintFlagsFilter::filterFullPaint).
  void filter_paint(SkiaPaint& p) const {
    if (filter) p.set_java_flags((p.java_flags() & ~filter->clear) | filter->set);
  }

  // SkiaCanvas::applyLooper: proc(paint), after the shadow when there is one.
  template <typename Proc>
  void apply_looper(const SkiaPaint* paint, Proc proc) {
    SkiaPaint p = paint ? *paint : SkiaPaint();
    filter_paint(p);
    if (p.looper) {
      SkiaPaint shadow = p;
      shadow.paint.setColor(p.looper->color);
      if (p.looper->sigma > 0) {
        shadow.paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, p.looper->sigma, true));
      }
      for (const auto& [offset, draw] : {std::pair<SkPoint, const SkiaPaint*>{p.looper->offset, &shadow},
                                         std::pair<SkPoint, const SkiaPaint*>{{0, 0}, &p}}) {
        canvas->save();
        canvas->translate(offset.fX, offset.fY);
        proc(*draw);
        canvas->restore();
      }
    } else {
      proc(p);
    }
  }

  const SaveRec* current_save_rec() const {
    if (save_stack.empty()) return nullptr;
    const SaveRec& rec = save_stack.back();
    return rec.save_count == canvas->getSaveCount() ? &rec : nullptr;
  }

  int save(int32_t flags) {
    int count = canvas->save();
    flags &= kSaveMatrixClip;
    if (flags != kSaveMatrixClip) save_stack.push_back({canvas->getSaveCount(), flags, clip_stack.size()});
    return count;
  }

  // SkiaCanvas::restore: the matrix and the clip of a partial save survive.
  void restore() {
    const SaveRec* rec = current_save_rec();
    if (!rec) {
      canvas->restore();
      return;
    }
    bool preserve_matrix = !(rec->flags & kSaveMatrix), preserve_clip = !(rec->flags & kSaveClip);
    SkMatrix saved = canvas->getTotalMatrix();
    size_t clip_index = rec->clip_index;
    canvas->restore();
    save_stack.pop_back();
    if (preserve_matrix) canvas->setMatrix(saved);
    if (preserve_clip) apply_persistent_clips(clip_index);
  }

  void restore_to_count(int count) {
    while (canvas->getSaveCount() > count) restore();
  }

  void record_clip(Clip::Type type, SkClipOp op, const SkRRect& rrect, const SkPath& path) {
    const SaveRec* rec = current_save_rec();
    if (rec && !(rec->flags & kSaveClip)) clip_stack.push_back(Clip{type, op, canvas->getTotalMatrix(), rrect, path});
  }

  void apply_persistent_clips(size_t start) {
    SkMatrix saved = canvas->getTotalMatrix();
    for (size_t i = start; i < clip_stack.size(); ++i) clip_stack[i].apply(canvas.get());
    canvas->setMatrix(saved);
    const SaveRec* rec = current_save_rec();
    if (!rec || (rec->flags & kSaveClip)) clip_stack.erase(clip_stack.begin() + std::ptrdiff_t(start), clip_stack.end());
  }

  // SkiaCanvas::getClipBounds: the device clip mapped back, not outset.
  bool clip_bounds(SkRect* out) const {
    SkIRect device;
    if (!canvas->getDeviceClipBounds(&device)) return false;
    SkMatrix inverse;
    if (!canvas->getTotalMatrix().invert(&inverse)) {
      out->setEmpty();
      return false;
    }
    *out = inverse.mapRect(SkRect::Make(device));
    return true;
  }

  // Region.Op: DIFFERENCE 0 and INTERSECT 1 clip, REPLACE 5 (before Android P)
  // resets the clip first; the others, which would grow the clip, are ignored.
  bool clip_rect(const SkRect& rect, int32_t op) {
    if (op == 0 || op == 1) {
      SkClipOp clip_op = op == 0 ? SkClipOp::kDifference : SkClipOp::kIntersect;
      record_clip(Clip::Type::kRect, clip_op, SkRRect::MakeRect(rect), SkPath());
      canvas->clipRect(rect, clip_op);
      return !canvas->isClipEmpty();
    }
    if (op == 5) {
      SkCanvasPriv::ResetClip(canvas.get());
      canvas->clipRect(rect, SkClipOp::kIntersect);
      return !canvas->isClipEmpty();
    }
    SkRect ignored;
    return clip_bounds(&ignored);
  }

  bool clip_path(const SkPath& path, int32_t op) {
    if (op == 0 || op == 1) {
      SkClipOp clip_op = op == 0 ? SkClipOp::kDifference : SkClipOp::kIntersect;
      record_clip(Clip::Type::kPath, clip_op, SkRRect(), path);
      canvas->clipPath(path, clip_op, true);
      return !canvas->isClipEmpty();
    }
    if (op == 5) {
      SkCanvasPriv::ResetClip(canvas.get());
      canvas->clipPath(path, SkClipOp::kIntersect, true);
      return !canvas->isClipEmpty();
    }
    SkRect ignored;
    return clip_bounds(&ignored);
  }
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<CanvasState>& canvases() {
  static auto* t = new HandleTable<CanvasState>(0x110F);  // never destroyed
  return *t;
}

HandleTable<PaintFlagsFilter>& draw_filters() {
  static auto* t = new HandleTable<PaintFlagsFilter>(0x1121);  // never destroyed
  return *t;
}

std::shared_ptr<CanvasState> canvas(Runtime& rt, int64_t h) {
  auto c = canvases().get(h);
  if (!c) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Canvas invalide");
  return c;
}

// Runs fn(state, args) under the canvas's lock; the canvas is argument 0.
// What reads Java objects (paints, paths, arrays) is read before, by the
// callers that need it: the lock is never held while Java code runs.
template <typename Fn>
rt::NativeFn with_canvas(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    return fn(*c, a);
  };
}

// Draws with the canvas locked: fn(state).
template <typename Fn>
void draw(Runtime& rt, int64_t handle, Fn fn) {
  auto c = canvas(rt, handle);
  std::lock_guard<std::mutex> guard(c->lock);
  fn(*c);
}

SkMatrix sk_matrix(const skmatrix::Values& v) {
  SkMatrix m;
  m.set9(v.data());
  return m;
}

[[noreturn]] void throw_aioobe(Runtime& rt) { rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", ""); }

// count elements of a primitive array from `offset` (AIOOBE when out of
// bounds, NullPointerException when null).
template <typename T>
std::vector<T> array_values(Runtime& rt, Handle array, int64_t offset, int64_t count, const char* what) {
  if (array == kNull) rt.throw_npe(what);
  rt::ArrayObject& a = rt.array(array, what);
  if (a.width != sizeof(T)) rt.throw_new("Ljava/lang/IllegalArgumentException;", what);
  if (offset < 0 || count < 0 || offset + count > int64_t(a.length)) throw_aioobe(rt);
  std::vector<T> out(static_cast<size_t>(count));
  if (count > 0) std::memcpy(out.data(), a.data.data() + size_t(offset) * sizeof(T), size_t(count) * sizeof(T));
  return out;
}

// --- Bitmaps -----------------------------------------------------------------

// A bitmap to draw: its size and Bitmap::makeImage.
struct BitmapImage {
  PixelTarget pixels;
  sk_sp<SkImage> image;
  int32_t width() const { return pixels.width; }
  int32_t height() const { return pixels.height; }
};

BitmapImage bitmap_image(Runtime& rt, int64_t handle) {
  BitmapImage b;
  b.pixels = bitmap_pixels(rt, handle);
  b.image = skia_bitmap_image(rt, handle);
  return b;
}

// SkiaCanvas::drawBitmap.
void draw_bitmap(CanvasState& c, const BitmapImage& b, float left, float top, const SkiaPaint* paint) {
  if (!b.image) return;
  c.apply_looper(paint, [&](const SkiaPaint& p) { c.canvas->drawImage(b.image, left, top, p.sampling(), &p.paint); });
}

void draw_bitmap_rect(CanvasState& c, const BitmapImage& b, const SkRect& src, const SkRect& dst,
                      const SkiaPaint* paint) {
  if (!b.image) return;
  c.apply_looper(paint, [&](const SkiaPaint& p) {
    c.canvas->drawImageRect(b.image, src, dst, p.sampling(), &p.paint, SkCanvas::kFast_SrcRectConstraint);
  });
}

// SkiaCanvas::drawNinePatch with NinePatchUtils' lattice.
void draw_nine_patch(CanvasState& c, const BitmapImage& b, const NinePatchDivs& chunk, const SkRect& dst,
                     const SkiaPaint* paint) {
  if (!b.image) return;
  SkCanvas::Lattice lattice;
  lattice.fXDivs = chunk.x_divs.data();
  lattice.fYDivs = chunk.y_divs.data();
  lattice.fXCount = int(chunk.x_divs.size());
  lattice.fYCount = int(chunk.y_divs.size());
  // A last division at the width or height says nothing, and Skia refuses it.
  if (lattice.fXCount > 0 && b.width() == lattice.fXDivs[lattice.fXCount - 1]) lattice.fXCount--;
  if (lattice.fYCount > 0 && b.height() == lattice.fYDivs[lattice.fYCount - 1]) lattice.fYCount--;
  lattice.fRectTypes = nullptr;
  lattice.fColors = nullptr;
  lattice.fBounds = nullptr;

  int x_rects = lattice.fXCount > 0 ? (lattice.fXDivs[0] == 0 ? lattice.fXCount : lattice.fXCount + 1) : 1;
  int y_rects = lattice.fYCount > 0 ? (lattice.fYDivs[0] == 0 ? lattice.fYCount : lattice.fYCount + 1) : 1;
  int num_flags = 0;
  if (!chunk.colors.empty() && int(chunk.colors.size()) == x_rects * y_rects) {
    num_flags = (lattice.fXCount + 1) * (lattice.fYCount + 1);
  }
  std::vector<SkCanvas::Lattice::RectType> flags(size_t(num_flags), SkCanvas::Lattice::kDefault);
  std::vector<SkColor> colors(size_t(num_flags), 0);
  if (num_flags > 0) {
    // NinePatchUtils::SetLatticeFlags (Res_png_9patch: TRANSPARENT_COLOR 0, NO_COLOR 1).
    bool pad_row = lattice.fYCount > 0 && lattice.fYDivs[0] == 0;
    bool pad_col = lattice.fXCount > 0 && lattice.fXDivs[0] == 0;
    size_t f = pad_row ? size_t(lattice.fXCount + 1) : 0;
    int y_count = lattice.fYCount - (pad_row ? 1 : 0);
    size_t i = 0;
    bool set_flags = false;
    for (int y = 0; y < y_count + 1; y++) {
      for (int x = 0; x < lattice.fXCount + 1; x++, f++) {
        if (x == 0 && pad_col) continue;
        uint32_t color = chunk.colors[i++];
        if (color == 0) {
          flags[f] = SkCanvas::Lattice::kTransparent;
          set_flags = true;
        } else if (color != 1) {
          flags[f] = SkCanvas::Lattice::kFixedColor;
          colors[f] = color;
          set_flags = true;
        }
      }
    }
    if (set_flags) {
      lattice.fRectTypes = flags.data();
      lattice.fColors = colors.data();
    }
  }
  c.apply_looper(paint, [&](const SkiaPaint& p) {
    c.canvas->drawImageLattice(b.image.get(), lattice, dst, p.filter_mode(), &p.paint);
  });
}

// SkiaCanvas::drawBitmapMesh.
void draw_bitmap_mesh(CanvasState& c, const BitmapImage& b, int mesh_width, int mesh_height, const float* vertices,
                      const int32_t* vertex_colors, const SkiaPaint* paint) {
  if (!b.image) return;
  const int pt_count = (mesh_width + 1) * (mesh_height + 1);
  const int index_count = mesh_width * mesh_height * 6;
  uint32_t builder_flags = SkVertices::kHasTexCoords_BuilderFlag;
  if (vertex_colors) builder_flags |= SkVertices::kHasColors_BuilderFlag;
  SkVertices::Builder builder(SkVertices::kTriangles_VertexMode, pt_count, index_count, builder_flags);
  std::memcpy(builder.positions(), vertices, size_t(pt_count) * sizeof(SkPoint));
  if (vertex_colors) std::memcpy(builder.colors(), vertex_colors, size_t(pt_count) * sizeof(SkColor));
  SkPoint* texs = builder.texCoords();
  uint16_t* indices = builder.indices();
  const float w = float(b.width()), h = float(b.height());
  const float dx = w / float(mesh_width), dy = h / float(mesh_height);
  float y = 0;
  for (int i = 0; i <= mesh_height; i++) {
    if (i == mesh_height) y = h;  // to hit h exactly
    float x = 0;
    for (int j = 0; j < mesh_width; j++) {
      (texs++)->set(x, y);
      x += dx;
    }
    (texs++)->set(w, y);
    y += dy;
  }
  int index = 0;
  for (int i = 0; i < mesh_height; i++) {
    for (int j = 0; j < mesh_width; j++) {
      *indices++ = uint16_t(index);
      *indices++ = uint16_t(index + mesh_width + 1);
      *indices++ = uint16_t(index + mesh_width + 2);
      *indices++ = uint16_t(index);
      *indices++ = uint16_t(index + mesh_width + 2);
      *indices++ = uint16_t(index + 1);
      index += 1;
    }
    index += 1;
  }
  SkiaPaint pnt = paint ? *paint : SkiaPaint();
  SkSamplingOptions sampling = pnt.sampling();
  pnt.paint.setShader(b.image->makeShader(sampling));
  sk_sp<SkVertices> v = builder.detach();
  c.apply_looper(&pnt, [&](const SkiaPaint& p) {
    SkPaint copy(p.paint);
    if (p.sampling() != sampling) copy.setShader(b.image->makeShader(p.sampling()));
    c.canvas->drawVertices(v, SkBlendMode::kModulate, copy);
  });
}

// --- Text --------------------------------------------------------------------

// A run of glyphs of one font (minikin's font runs): its typeface and fakery.
struct FontRun {
  size_t start = 0, end = 0;
  sk_sp<SkTypeface> typeface;
  bool fake_bold = false, fake_italic = false;
};

// A laid out text, ready to draw (computed before locking the canvas).
struct LaidOutText {
  text::TextLayout layout;
  std::vector<FontRun> runs;
};

LaidOutText lay_out(std::u16string_view context, size_t start, size_t count, text::Bidi bidi, const SkiaPaint& p,
                    text::RunFlag run_flag) {
  LaidOutText out;
  out.layout = text::do_layout(context, start, count, bidi, p.minikin, p.start_hyphen, p.end_hyphen, run_flag);
  const auto& glyphs = out.layout.glyphs;
  for (size_t i = 0; i < glyphs.size();) {
    size_t j = i + 1;
    while (j < glyphs.size() && glyphs[j].font == glyphs[i].font) j++;
    const text::FakedFont& font = glyphs[i].font;
    out.runs.push_back(FontRun{i, j, skia_typeface(font.face()), font.fakery.fake_bold, font.fakery.fake_italic});
    i = j;
  }
  return out;
}

// SkiaCanvas::drawGlyphs: one text blob per font run.
void draw_glyphs(CanvasState& c, const SkiaPaint& paint, const text::TextLayout& layout, size_t start, size_t end,
                 float x, float y) {
  int count = int(end - start);
  if (count <= 0 || paint.nothing_to_draw()) return;
  SkiaPaint copy = paint;
  c.filter_paint(copy);
  SkTextBlobBuilder builder;
  const SkTextBlobBuilder::RunBuffer& buffer = builder.allocRunPos(copy.font, count);
  for (size_t i = start, k = 0; i < end; i++, k++) {
    buffer.glyphs[k] = SkGlyphID(layout.glyphs[i].glyph);
    buffer.pos[2 * k] = x + layout.glyphs[i].x;
    buffer.pos[2 * k + 1] = y + layout.glyphs[i].y;
  }
  sk_sp<SkTextBlob> blob = builder.make();
  c.apply_looper(&copy, [&](const SkiaPaint& p) { c.canvas->drawTextBlob(blob, 0, 0, p.paint); });
}

// DrawTextFunctor's drawStroke: a rectangle at least one pixel high.
void draw_stroke(CanvasState& c, float left, float right, float top, float thickness, const SkiaPaint& paint) {
  float bottom = top + std::fmax(thickness, 1.0f);
  if (paint.nothing_to_draw()) return;
  c.apply_looper(&paint, [&](const SkiaPaint& p) { c.canvas->drawRect({left, top, right, bottom}, p.paint); });
}

// MinikinUtils::xOffsetForTextAlign.
float x_offset_for_align(const SkiaPaint& p, float advance) {
  if (p.align == 1) return advance * -0.5f;
  if (p.align == 2) return -advance;
  return 0;
}

// Canvas::drawText, from the font runs on: populateSkFont per run, then the
// decorations at the lowest underline position of the runs.
void draw_laid_out_text(CanvasState& c, SkiaPaint paint, const LaidOutText& t, float x, float y) {
  x += x_offset_for_align(paint, t.layout.advance);
  paint.align = 0;
  float underline_position = 0, underline_thickness = 0;
  const float skew = paint.font.getSkewX();
  const bool embolden = paint.font.isEmbolden();
  for (const FontRun& run : t.runs) {
    paint.font.setTypeface(run.typeface);
    paint.font.setEmbolden(embolden || run.fake_bold);
    if (run.fake_italic) paint.font.setSkewX(skew - 0.25f);
    draw_glyphs(c, paint, t.layout, run.start, run.end, x, y);
    if (paint.underline) {
      SkFontMetrics metrics;
      paint.font.getMetrics(&metrics);
      const float size = paint.font.getSize();
      SkScalar position, thickness;
      if (!metrics.hasUnderlinePosition(&position)) position = size * (1.0f / 9.0f - 0.5f / 18.0f);
      if (!metrics.hasUnderlineThickness(&thickness)) thickness = size * (1.0f / 18.0f);
      underline_position = std::max(underline_position, position);
      underline_thickness = std::max(underline_thickness, thickness);
    }
    paint.font.setSkewX(skew);
    paint.font.setEmbolden(embolden);
  }
  SkiaPaint copied = paint;
  c.filter_paint(copied);
  if (copied.underline || copied.strike_thru) {
    const float left = x, right = x + t.layout.advance;
    if (copied.underline) draw_stroke(c, left, right, y + underline_position, underline_thickness, copied);
    if (copied.strike_thru) {
      // Paint::kStdStrikeThru_Top and _Thickness.
      const float size = paint.font.getSize();
      const float thickness = size * (1.0f / 18.0f);
      draw_stroke(c, left, right, y + size * (-6.0f / 21.0f - 0.5f * (1.0f / 18.0f)), thickness, copied);
    }
  }
}

// Canvas::drawText for the natives: the paint and the layout are computed
// first, the canvas is locked to draw.
void draw_text(Runtime& rt, int64_t handle, std::u16string_view context, size_t start, size_t count, float x,
               float y, text::Bidi bidi, int64_t paint_handle, std::optional<text::RunFlag> run_flag) {
  SkiaPaint paint = skia_paint_of(rt, paint_handle, true);
  // 'linear metrics' means linear: no hinting when drawing.
  if (paint.font.isLinearMetrics()) paint.font.setHinting(SkFontHinting::kNone);
  LaidOutText t = lay_out(context, start, count, bidi, paint, run_flag.value_or(paint.run_flag));
  draw(rt, handle, [&](CanvasState& c) { draw_laid_out_text(c, std::move(paint), t, x, y); });
}

// Canvas::drawTextOnPath and SkiaCanvas::drawLayoutOnPath.
void draw_text_on_path(Runtime& rt, int64_t handle, std::u16string_view text, int64_t path_handle, float h_offset,
                       float v_offset, text::Bidi bidi, int64_t paint_handle) {
  SkiaPaint paint = skia_paint_of(rt, paint_handle, true);
  if (paint.font.isLinearMetrics()) paint.font.setHinting(SkFontHinting::kNone);
  SkPath path = skia_path_of(rt, path_handle);
  LaidOutText t = lay_out(text, 0, text.size(), bidi, paint, text::kRunWholeLine);
  float align = paint.align == 1 ? -0.5f : paint.align == 2 ? -1.0f : 0.0f;
  if (align != 0) h_offset += align * (t.layout.advance - SkPathMeasure(path, false).getLength());
  paint.align = 0;
  draw(rt, handle, [&](CanvasState& c) {
    const float skew = paint.font.getSkewX();
    const bool embolden = paint.font.isEmbolden();
    for (const FontRun& run : t.runs) {
      paint.font.setTypeface(run.typeface);
      paint.font.setEmbolden(embolden || run.fake_bold);
      if (run.fake_italic) paint.font.setSkewX(skew - 0.25f);
      SkiaPaint copy = paint;
      c.filter_paint(copy);
      int n = int(run.end - run.start);
      SkTextBlobBuilder builder;
      const SkTextBlobBuilder::RunBuffer& rec = builder.allocRunRSXform(copy.font, n);
      auto* xform = reinterpret_cast<SkRSXform*>(rec.pos);
      SkPathMeasure measure(path, false);
      for (size_t i = run.start; i < run.end; i++) {
        const text::LayoutGlyph& g = t.layout.glyphs[i];
        rec.glyphs[i - run.start] = SkGlyphID(g.glyph);
        // HWUI reads the advance of the character at the glyph's index.
        float half_width = (i < t.layout.advances.size() ? t.layout.advances[i] : 0.0f) * 0.5f;
        float gx = h_offset + g.x + half_width, gy = v_offset + g.y;
        SkPoint pos;
        SkVector tan;
        if (!measure.getPosTan(gx, &pos, &tan)) {
          pos.set(gx, gy);
          tan.set(1, 0);
        }
        SkRSXform& x = xform[i - run.start];
        x.fSCos = tan.x();
        x.fSSin = tan.y();
        x.fTx = pos.x() - tan.y() * gy - half_width * tan.x();
        x.fTy = pos.y() + tan.x() * gy - half_width * tan.y();
      }
      sk_sp<SkTextBlob> blob = builder.make();
      c.apply_looper(&copy, [&](const SkiaPaint& p) { c.canvas->drawTextBlob(blob, 0, 0, p.paint); });
      paint.font.setSkewX(skew);
      paint.font.setEmbolden(embolden);
    }
  });
}

std::u16string chars_of(Runtime& rt, Handle array, int32_t index, int32_t count) {
  std::vector<char16_t> v = array_values<char16_t>(rt, array, index, count, "char[]");
  return std::u16string(v.begin(), v.end());
}

std::u16string string_range(Runtime& rt, Handle str, int32_t start, int32_t end) {
  if (str == kNull) rt.throw_npe("text");
  std::u16string s = rt.string_value(str);
  if (start < 0 || end < start || size_t(end) > s.size()) {
    rt.throw_new("Ljava/lang/StringIndexOutOfBoundsException;", "");
  }
  return s.substr(size_t(start), size_t(end - start));
}

text::Bidi bidi_of(int32_t flags) { return text::Bidi(std::clamp(flags, 0, 5)); }

void set_int_field(Runtime& rt, Handle obj, const char* name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, "Rect");
  rt::Field* f = find_field(o.klass, name, "I");
  if (f && !f->is_static()) o.fields.at(f->slot) = uint32_t(v);
}

// --- Natives -----------------------------------------------------------------

void register_canvas_state(rt::NativeRegistry& r) {
  const char* cv = "Landroid/graphics/Canvas;";
  static const int64_t canvas_free = register_free_function([](int64_t h) { canvases().remove(h); });
  r.bind(cv, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(canvas_free); });
  r.bind(cv, "nInitRaster", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = std::make_shared<CanvasState>();
    if (a.i64(0) != 0) c->set_target(bitmap_pixels(rt, a.i64(0)));
    return rt::ret_long(canvases().add(std::move(c)));
  });
  r.bind(cv, "nSetBitmap", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PixelTarget t = a.i64(2) != 0 ? bitmap_pixels(rt, a.i64(2)) : PixelTarget{};
    draw(rt, a.i64(0), [&](CanvasState& c) { c.set_target(t); });
    return 0;
  });
  r.bind(cv, "nFreeCaches", "()V", [](Runtime&, NativeArgs&) -> uint64_t {
    SkGraphics::PurgeFontCache();
    return 0;
  });
  r.bind(cv, "nFreeTextLayoutCaches", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(cv, "nSetCompatibilityVersion", "(I)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    api_level = a.i32(0);
    return 0;
  });
  r.bind(cv, "nIsHighContrastText", "(J)Z", with_canvas([](CanvasState&, NativeArgs&) -> uint64_t { return 0; }));
  r.bind(cv, "nIsOpaque", "(J)Z", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    return rt::ret_bool(c.canvas->imageInfo().isOpaque());
  }));
  r.bind(cv, "nGetWidth", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    return rt::ret_int(c.canvas->imageInfo().width());
  }));
  r.bind(cv, "nGetHeight", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    return rt::ret_int(c.canvas->imageInfo().height());
  }));
  r.bind(cv, "nSetDrawFilter", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<PaintFlagsFilter> filter;
    if (a.i64(2) != 0) {
      auto f = draw_filters().get(a.i64(2));
      if (!f) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] DrawFilter invalide");
      filter = *f;
    }
    draw(rt, a.i64(0), [&](CanvasState& c) { c.filter = filter; });
    return 0;
  });

  // Save stack.
  r.bind(cv, "nGetSaveCount", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    return rt::ret_int(c.canvas->getSaveCount());
  }));
  r.bind(cv, "nSave", "(JI)I", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    return rt::ret_int(c.save(a.i32(2)));
  }));
  r.bind(cv, "nSaveLayer", "(JFFFFJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkiaPaint> paint;
    if (a.i64(6) != 0) paint = skia_paint_of(rt, a.i64(6));
    int count = 0;
    draw(rt, a.i64(0), [&](CanvasState& c) {
      SkRect bounds = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
      count = c.canvas->saveLayer(SkCanvas::SaveLayerRec(&bounds, paint ? &paint->paint : nullptr));
    });
    return rt::ret_int(count);
  });
  r.bind(cv, "nSaveLayerAlpha", "(JFFFFI)I", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    SkRect bounds = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
    uint32_t alpha = uint32_t(a.i32(6));
    SkPaint alpha_paint;
    if (alpha < 0xFF) alpha_paint.setAlpha(alpha);
    return rt::ret_int(c.canvas->saveLayer(SkCanvas::SaveLayerRec(&bounds, alpha < 0xFF ? &alpha_paint : nullptr)));
  }));
  r.bind(cv, "nSaveUnclippedLayer", "(JIIII)I", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    SkRect bounds = SkRect::MakeLTRB(float(a.i32(2)), float(a.i32(3)), float(a.i32(4)), float(a.i32(5)));
    return rt::ret_int(SkCanvasPriv::SaveBehind(c.canvas.get(), &bounds));
  }));
  r.bind(cv, "nRestoreUnclippedLayer", "(JIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint paint = skia_paint_of(rt, a.i64(3));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      int count = a.i32(2);
      while (c.canvas->getSaveCount() > count + 1) c.restore();
      if (c.canvas->getSaveCount() == count + 1) {
        SkiaPaint filtered = paint;
        c.filter_paint(filtered);
        SkCanvasPriv::DrawBehind(c.canvas.get(), filtered.paint);
        c.restore();
      }
    });
    return 0;
  });
  r.bind(cv, "nRestore", "(J)Z", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    if (c.canvas->getSaveCount() <= 1) return rt::ret_bool(false);
    c.restore();
    return rt::ret_bool(true);
  }));
  r.bind(cv, "nRestoreToCount", "(JI)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.restore_to_count(a.i32(2));
    return 0;
  }));

  // Current matrix.
  r.bind(cv, "nTranslate", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.canvas->translate(a.f32(2), a.f32(3));
    return 0;
  }));
  r.bind(cv, "nScale", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.canvas->scale(a.f32(2), a.f32(3));
    return 0;
  }));
  r.bind(cv, "nRotate", "(JF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.canvas->rotate(a.f32(2));
    return 0;
  }));
  r.bind(cv, "nSkew", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.canvas->skew(a.f32(2), a.f32(3));
    return 0;
  }));
  r.bind(cv, "nConcat", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkMatrix m = sk_matrix(skmatrix::of(rt, a.i64(2)));
    draw(rt, a.i64(0), [&](CanvasState& c) { c.canvas->concat(m); });
    return 0;
  });
  r.bind(cv, "nConcat", "(J[F)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<float> v = array_values<float>(rt, a.h(2), 0, 16, "float[]");
    SkM44 m = SkM44::RowMajor(v.data());
    draw(rt, a.i64(0), [&](CanvasState& c) { c.canvas->concat(m); });
    return 0;
  });
  r.bind(cv, "nSetMatrix", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkMatrix m = a.i64(2) == 0 ? SkMatrix::I() : sk_matrix(skmatrix::of(rt, a.i64(2)));
    draw(rt, a.i64(0), [&](CanvasState& c) { c.canvas->setMatrix(m); });
    return 0;
  });
  r.bind(cv, "nGetMatrix", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    skmatrix::Values v;
    draw(rt, a.i64(0), [&](CanvasState& c) { c.canvas->getTotalMatrix().get9(v.data()); });
    skmatrix::assign(rt, a.i64(2), v);
    return 0;
  });

  // Clip.
  r.bind(cv, "nClipRect", "(JFFFFI)Z", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(c.clip_rect(SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5)), a.i32(6)));
  }));
  r.bind(cv, "nClipPath", "(JJI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkPath path = skia_path_of(rt, a.i64(2));
    bool result = false;
    draw(rt, a.i64(0), [&](CanvasState& c) { result = c.clip_path(path, a.i32(4)); });
    return rt::ret_bool(result);
  });
  r.bind(cv, "nGetClipBounds", "(JLandroid/graphics/Rect;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle out = a.h(2);
    if (out == kNull) rt.throw_npe("Canvas.getClipBounds : Rect null");
    SkRect bounds = SkRect::MakeEmpty();
    bool result = false;
    draw(rt, a.i64(0), [&](CanvasState& c) { result = c.clip_bounds(&bounds); });
    if (!result) bounds.setEmpty();
    SkIRect ir = bounds.round();
    set_int_field(rt, out, "left", ir.fLeft);
    set_int_field(rt, out, "top", ir.fTop);
    set_int_field(rt, out, "right", ir.fRight);
    set_int_field(rt, out, "bottom", ir.fBottom);
    return rt::ret_bool(result);
  });
  r.bind(cv, "nQuickReject", "(JFFFF)Z", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(c.canvas->quickReject(SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5))));
  }));
  r.bind(cv, "nQuickReject", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkPath path = skia_path_of(rt, a.i64(2));
    bool result = false;
    draw(rt, a.i64(0), [&](CanvasState& c) { result = c.canvas->quickReject(path); });
    return rt::ret_bool(result);
  });

  // PaintFlagsDrawFilter (jni/PaintFilter.cpp): nothing to filter, no filter.
  const char* pf = "Landroid/graphics/PaintFlagsDrawFilter;";
  r.bind(pf, "nativeConstructor", "(II)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    if ((a.i32(0) | a.i32(1)) == 0) return rt::ret_long(0);
    // PaintFlagsFilter keeps the low 16 bits.
    auto f = std::make_shared<PaintFlagsFilter>(PaintFlagsFilter{a.i32(0) & 0xFFFF, a.i32(1) & 0xFFFF});
    return rt::ret_long(draw_filters().add(std::move(f)));
  });
  r.bind("Landroid/graphics/DrawFilter;", "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    draw_filters().remove(a.i64(0));
    return 0;
  });
}

void register_canvas_draw(rt::NativeRegistry& r) {
  const char* bc = "Landroid/graphics/BaseCanvas;";
  r.bind(bc, "nDrawColor", "(JII)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.canvas->drawColor(SkColor(uint32_t(a.i32(2))), SkBlendMode(std::clamp(a.i32(3), 0, int(SkBlendMode::kLastMode))));
    return 0;
  }));
  r.bind(bc, "nDrawColor", "(JJJI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p;
    p.paint.setColor(color4f_of_long(a.i64(4)), skia_color_space(rt, a.i64(2)).get());
    p.paint.setBlendMode(SkBlendMode(std::clamp(a.i32(6), 0, int(SkBlendMode::kLastMode))));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.filter_paint(p);
      c.canvas->drawPaint(p.paint);
    });
    return 0;
  });
  r.bind(bc, "nDrawPaint", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(2));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.filter_paint(p);
      c.canvas->drawPaint(p.paint);
    });
    return 0;
  });
  r.bind(bc, "nDrawPoint", "(JFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(4));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawPoint(a.f32(2), a.f32(3), q.paint); });
    });
    return 0;
  });
  // SkiaCanvas::drawPoints: count floats, two per point.
  auto points = [](SkCanvas::PointMode mode, int min_count) {
    return [mode, min_count](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::vector<float> v = array_values<float>(rt, a.h(2), a.i32(3), a.i32(4), "float[]");
      SkiaPaint p = skia_paint_of(rt, a.i64(5));
      if (int(v.size()) < min_count || p.nothing_to_draw()) return 0;
      std::vector<SkPoint> pts(v.size() / 2);
      for (size_t i = 0; i < pts.size(); i++) pts[i].set(v[2 * i], v[2 * i + 1]);
      draw(rt, a.i64(0), [&](CanvasState& c) {
        c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawPoints(mode, pts, q.paint); });
      });
      return 0;
    };
  };
  r.bind(bc, "nDrawPoints", "(J[FIIJ)V", points(SkCanvas::kPoints_PointMode, 2));
  r.bind(bc, "nDrawLines", "(J[FIIJ)V", points(SkCanvas::kLines_PointMode, 4));
  r.bind(bc, "nDrawLine", "(JFFFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(6));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) {
        c.canvas->drawLine(a.f32(2), a.f32(3), a.f32(4), a.f32(5), q.paint);
      });
    });
    return 0;
  });
  r.bind(bc, "nDrawRect", "(JFFFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(6));
    if (p.nothing_to_draw()) return 0;
    SkRect rect = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawRect(rect, q.paint); });
    });
    return 0;
  });
  r.bind(bc, "nDrawRegion", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkRegion region = skia_region_of(rt, a.i64(2));
    SkiaPaint p = skia_paint_of(rt, a.i64(4));
    if (p.nothing_to_draw()) return 0;
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawRegion(region, q.paint); });
    });
    return 0;
  });
  r.bind(bc, "nDrawRoundRect", "(JFFFFFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(8));
    if (p.nothing_to_draw()) return 0;
    SkRect rect = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawRoundRect(rect, a.f32(6), a.f32(7), q.paint); });
    });
    return 0;
  });
  auto double_round_rect = [](Runtime& rt, int64_t handle, const SkRRect& outer, const SkRRect& inner,
                              int64_t paint) {
    SkiaPaint p = skia_paint_of(rt, paint);
    if (p.nothing_to_draw()) return;
    draw(rt, handle, [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawDRRect(outer, inner, q.paint); });
    });
  };
  r.bind(bc, "nDrawDoubleRoundRect", "(JFFFFFFFFFFFFJ)V", [double_round_rect](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkRRect outer, inner;
    outer.setRectXY(SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5)), a.f32(6), a.f32(7));
    inner.setRectXY(SkRect::MakeLTRB(a.f32(8), a.f32(9), a.f32(10), a.f32(11)), a.f32(12), a.f32(13));
    double_round_rect(rt, a.i64(0), outer, inner, a.i64(14));
    return 0;
  });
  r.bind(bc, "nDrawDoubleRoundRect", "(JFFFF[FFFFF[FJ)V", [double_round_rect](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<float> outer_radii = array_values<float>(rt, a.h(6), 0, 8, "float[]");
    std::vector<float> inner_radii = array_values<float>(rt, a.h(11), 0, 8, "float[]");
    SkRRect outer, inner;
    outer.setRectRadii(SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5)),
                       reinterpret_cast<const SkVector*>(outer_radii.data()));
    inner.setRectRadii(SkRect::MakeLTRB(a.f32(7), a.f32(8), a.f32(9), a.f32(10)),
                       reinterpret_cast<const SkVector*>(inner_radii.data()));
    double_round_rect(rt, a.i64(0), outer, inner, a.i64(12));
    return 0;
  });
  r.bind(bc, "nDrawCircle", "(JFFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(5));
    float radius = a.f32(4);
    if (!(radius > 0) || p.nothing_to_draw()) return 0;
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawCircle(a.f32(2), a.f32(3), radius, q.paint); });
    });
    return 0;
  });
  r.bind(bc, "nDrawOval", "(JFFFFJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(6));
    if (p.nothing_to_draw()) return 0;
    SkRect oval = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawOval(oval, q.paint); });
    });
    return 0;
  });
  r.bind(bc, "nDrawArc", "(JFFFFFFZJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkiaPaint p = skia_paint_of(rt, a.i64(9));
    if (p.nothing_to_draw()) return 0;
    SkRect arc = SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
    float start = a.f32(6), sweep = a.f32(7);
    bool use_center = a.z(8);
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) {
        if (std::fabs(sweep) >= 360.0f) {
          c.canvas->drawOval(arc, q.paint);
        } else {
          c.canvas->drawArc(arc, start, sweep, use_center, q.paint);
        }
      });
    });
    return 0;
  });
  r.bind(bc, "nDrawPath", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkPath path = skia_path_of(rt, a.i64(2));
    SkiaPaint p = skia_paint_of(rt, a.i64(4));
    if (p.nothing_to_draw() || (path.isEmpty() && !path.isInverseFillType())) return 0;
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawPath(path, q.paint); });
    });
    return 0;
  });
  r.bind(bc, "nDrawVertices", "(JII[FI[FI[II[SIIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t mode = a.i32(2), float_count = a.i32(3), vert_index = a.i32(5), color_index = a.i32(9);
    int32_t index_index = a.i32(11), index_count = a.i32(12);
    if (float_count < 0 || index_count < 0) throw_aioobe(rt);
    const int32_t vertex_count = float_count >> 1;
    std::vector<float> verts = array_values<float>(rt, a.h(4), vert_index, float_count, "float[]");
    std::vector<float> texs;
    if (a.h(6) != kNull) {
      // HWUI checks the length against texIndex but reads from vertIndex.
      rt::ArrayObject& t = rt.array(a.h(6), "float[]");
      if (int64_t(a.i32(7)) + float_count > int64_t(t.length)) throw_aioobe(rt);
      texs = array_values<float>(rt, a.h(6), vert_index, float_count, "float[]");
    }
    std::vector<int32_t> colors;
    if (a.h(8) != kNull) colors = array_values<int32_t>(rt, a.h(8), color_index, vertex_count, "int[]");
    std::vector<int16_t> indices;
    if (a.h(10) != kNull) indices = array_values<int16_t>(rt, a.h(10), index_index, index_count, "short[]");
    SkiaPaint p = skia_paint_of(rt, a.i64(13));
    if (a.h(6) == kNull) p.paint.setShader(nullptr);  // no texture coordinates: no shader
    // Skia blends the paint and the vertex colours without a shader; kDst
    // keeps the vertex colours, as before.
    SkBlendMode blend = p.paint.getShader() ? SkBlendMode::kModulate : SkBlendMode::kDst;
    sk_sp<SkVertices> v = SkVertices::MakeCopy(
        SkVertices::VertexMode(std::clamp(mode, 0, 2)), vertex_count, reinterpret_cast<const SkPoint*>(verts.data()),
        texs.empty() ? nullptr : reinterpret_cast<const SkPoint*>(texs.data()),
        colors.empty() ? nullptr : reinterpret_cast<const SkColor*>(colors.data()), indices.empty() ? 0 : index_count,
        indices.empty() ? nullptr : reinterpret_cast<const uint16_t*>(indices.data()));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&p, [&](const SkiaPaint& q) { c.canvas->drawVertices(v, blend, q.paint); });
    });
    return 0;
  });

  // Bitmaps, with the density scaling of the JNI layer.
  r.bind(bc, "nDrawNinePatch", "(JJJFFFFJII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    BitmapImage b = bitmap_image(rt, a.i64(2));
    NinePatchDivs chunk = nine_patch_divs(rt, a.i64(4));
    std::optional<SkiaPaint> paint;
    if (a.i64(10) != 0) paint = skia_paint_of(rt, a.i64(10));
    float left = a.f32(6), top = a.f32(7), right = a.f32(8), bottom = a.f32(9);
    int32_t dst_density = a.i32(12), src_density = a.i32(13);
    draw(rt, a.i64(0), [&](CanvasState& c) {
      if (dst_density == src_density || dst_density == 0 || src_density == 0) {
        draw_nine_patch(c, b, chunk, SkRect::MakeLTRB(left, top, right, bottom), paint ? &*paint : nullptr);
        return;
      }
      c.save(kSaveMatrixClip);
      float scale = float(dst_density) / float(src_density);
      c.canvas->translate(left, top);
      c.canvas->scale(scale, scale);
      SkiaPaint filtered = paint ? *paint : SkiaPaint();
      filtered.filter_bitmap = true;
      draw_nine_patch(c, b, chunk, SkRect::MakeLTRB(0, 0, (right - left) / scale, (bottom - top) / scale), &filtered);
      c.restore();
    });
    return 0;
  });
  r.bind(bc, "nDrawBitmapMatrix", "(JJJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    BitmapImage b = bitmap_image(rt, a.i64(2));
    SkMatrix m = sk_matrix(skmatrix::of(rt, a.i64(4)));
    std::optional<SkiaPaint> paint;
    if (a.i64(6) != 0) paint = skia_paint_of(rt, a.i64(6));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      SkAutoCanvasRestore restore(c.canvas.get(), true);
      c.canvas->concat(m);
      draw_bitmap(c, b, 0, 0, paint ? &*paint : nullptr);
    });
    return 0;
  });
  r.bind(bc, "nDrawBitmap", "(JJFFJIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    BitmapImage b = bitmap_image(rt, a.i64(2));
    std::optional<SkiaPaint> paint;
    if (a.i64(6) != 0) paint = skia_paint_of(rt, a.i64(6));
    float left = a.f32(4), top = a.f32(5);
    int32_t canvas_density = a.i32(8), screen_density = a.i32(9), bitmap_density = a.i32(10);
    draw(rt, a.i64(0), [&](CanvasState& c) {
      if (canvas_density == bitmap_density || canvas_density == 0 || bitmap_density == 0) {
        if (screen_density != 0 && screen_density != bitmap_density) {
          SkiaPaint filtered = paint ? *paint : SkiaPaint();
          filtered.filter_bitmap = true;
          draw_bitmap(c, b, left, top, &filtered);
        } else {
          draw_bitmap(c, b, left, top, paint ? &*paint : nullptr);
        }
        return;
      }
      c.save(kSaveMatrixClip);
      float scale = float(canvas_density) / float(bitmap_density);
      c.canvas->translate(left, top);
      c.canvas->scale(scale, scale);
      SkiaPaint filtered = paint ? *paint : SkiaPaint();
      filtered.filter_bitmap = true;
      draw_bitmap(c, b, 0, 0, &filtered);
      c.restore();
    });
    return 0;
  });
  r.bind(bc, "nDrawBitmap", "(JJFFFFFFFFJII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    BitmapImage b = bitmap_image(rt, a.i64(2));
    std::optional<SkiaPaint> paint;
    if (a.i64(12) != 0) paint = skia_paint_of(rt, a.i64(12));
    SkRect src = SkRect::MakeLTRB(a.f32(4), a.f32(5), a.f32(6), a.f32(7));
    SkRect dst = SkRect::MakeLTRB(a.f32(8), a.f32(9), a.f32(10), a.f32(11));
    int32_t screen_density = a.i32(14), bitmap_density = a.i32(15);
    if (screen_density != 0 && screen_density != bitmap_density) {
      if (!paint) paint = SkiaPaint();
      paint->filter_bitmap = true;
    }
    draw(rt, a.i64(0), [&](CanvasState& c) { draw_bitmap_rect(c, b, src, dst, paint ? &*paint : nullptr); });
    return 0;
  });
  r.bind(bc, "nDrawBitmap", "(J[IIIFFIIZJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t offset = a.i32(3), stride = a.i32(4), width = a.i32(7), height = a.i32(8);
    bool has_alpha = a.z(9);
    if (width <= 0 || height <= 0) return 0;
    if (a.h(2) == kNull) rt.throw_npe("colors");
    rt::ArrayObject& arr = rt.array(a.h(2), "int[]");
    // Each row's colours, rows `stride` apart (Java checked the bounds).
    int64_t last = int64_t(offset) + int64_t(height - 1) * stride;
    if (offset < 0 || last < 0 || int64_t(offset) + width > int64_t(arr.length) || last + width > int64_t(arr.length)) {
      throw_aioobe(rt);
    }
    std::vector<uint32_t> colors(size_t(width) * size_t(height));
    for (int32_t y = 0; y < height; y++) {
      std::memcpy(colors.data() + size_t(y) * size_t(width),
                  arr.data.data() + size_t(int64_t(offset) + int64_t(y) * stride) * 4, size_t(width) * 4);
    }
    // GraphicsJNI::SetPixels into an N32 bitmap, or RGB 565 without alpha.
    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::Make(width, height, has_alpha ? kN32_SkColorType : kRGB_565_SkColorType,
                                         kPremul_SkAlphaType));
    SkPixmap src(SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB()),
                 colors.data(), size_t(width) * 4);
    bitmap.writePixels(src, 0, 0);
    bitmap.setImmutable();
    BitmapImage b;
    b.pixels.width = width;
    b.pixels.height = height;
    b.image = bitmap.asImage();
    std::optional<SkiaPaint> paint;
    if (a.i64(10) != 0) paint = skia_paint_of(rt, a.i64(10));
    draw(rt, a.i64(0), [&](CanvasState& c) { draw_bitmap(c, b, a.f32(5), a.f32(6), paint ? &*paint : nullptr); });
    return 0;
  });
  r.bind(bc, "nDrawBitmapMesh", "(JJII[FI[IIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t mesh_width = a.i32(4), mesh_height = a.i32(5), vert_index = a.i32(7), color_index = a.i32(9);
    if (api_level < 28) vert_index = color_index = 0;  // before P, the offsets were ignored
    if (mesh_width <= 0 || mesh_height <= 0) return 0;
    int64_t pt_count = int64_t(mesh_width + 1) * int64_t(mesh_height + 1);
    if (pt_count > 65536) rt.throw_new("Ljava/lang/IllegalArgumentException;", "maillage trop grand");
    std::vector<float> verts = array_values<float>(rt, a.h(6), int64_t(vert_index) * 2, pt_count * 2, "float[]");
    std::vector<int32_t> colors;
    if (a.h(8) != kNull) colors = array_values<int32_t>(rt, a.h(8), color_index, pt_count, "int[]");
    BitmapImage b = bitmap_image(rt, a.i64(2));
    std::optional<SkiaPaint> paint;
    if (a.i64(10) != 0) paint = skia_paint_of(rt, a.i64(10));
    draw(rt, a.i64(0), [&](CanvasState& c) {
      draw_bitmap_mesh(c, b, mesh_width, mesh_height, verts.data(), colors.empty() ? nullptr : colors.data(),
                       paint ? &*paint : nullptr);
    });
    return 0;
  });

  // Text. drawText draws a whole line, whatever the paint's run flag.
  r.bind(bc, "nDrawText", "(J[CIIFFIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string s = chars_of(rt, a.h(2), a.i32(3), a.i32(4));
    draw_text(rt, a.i64(0), s, 0, s.size(), a.f32(5), a.f32(6), bidi_of(a.i32(7)), a.i64(8), text::kRunWholeLine);
    return 0;
  });
  r.bind(bc, "nDrawText", "(JLjava/lang/String;IIFFIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string s = string_range(rt, a.h(2), a.i32(3), a.i32(4));
    draw_text(rt, a.i64(0), s, 0, s.size(), a.f32(5), a.f32(6), bidi_of(a.i32(7)), a.i64(8), text::kRunWholeLine);
    return 0;
  });
  // drawTextRun: laid out within its context, in the direction given (the
  // MeasuredText, when given, is not needed to lay out the same glyphs).
  r.bind(bc, "nDrawTextRun", "(J[CIIIIFFZJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t index = a.i32(3), count = a.i32(4), context_index = a.i32(5), context_count = a.i32(6);
    if ((index | count | context_index | context_count | (index - context_index) |
         ((context_index + context_count) - (index + count))) < 0) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "");
    }
    std::u16string context = chars_of(rt, a.h(2), context_index, context_count);
    text::Bidi bidi = a.z(9) ? text::Bidi::kForceRtl : text::Bidi::kForceLtr;
    draw_text(rt, a.i64(0), context, size_t(index - context_index), size_t(count), a.f32(7), a.f32(8), bidi,
              a.i64(10), std::nullopt);
    return 0;
  });
  r.bind(bc, "nDrawTextRun", "(JLjava/lang/String;IIIIFFZJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t start = a.i32(3), end = a.i32(4), context_start = a.i32(5), context_end = a.i32(6);
    if ((start | end | context_start | context_end | (start - context_start) | (end - start) |
         (context_end - end)) < 0) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "");
    }
    std::u16string context = string_range(rt, a.h(2), context_start, context_end);
    text::Bidi bidi = a.z(9) ? text::Bidi::kForceRtl : text::Bidi::kForceLtr;
    draw_text(rt, a.i64(0), context, size_t(start - context_start), size_t(end - start), a.f32(7), a.f32(8), bidi,
              a.i64(10), std::nullopt);
    return 0;
  });
  r.bind(bc, "nDrawTextOnPath", "(J[CIIJFFIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::u16string s = chars_of(rt, a.h(2), a.i32(3), a.i32(4));
    draw_text_on_path(rt, a.i64(0), s, a.i64(5), a.f32(7), a.f32(8), bidi_of(a.i32(9)), a.i64(10));
    return 0;
  });
  r.bind(bc, "nDrawTextOnPath", "(JLjava/lang/String;JFFIJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(2) == kNull) rt.throw_npe("text");
    std::u16string s = rt.string_value(a.h(2));
    draw_text_on_path(rt, a.i64(0), s, a.i64(3), a.f32(5), a.f32(6), bidi_of(a.i32(7)), a.i64(8));
    return 0;
  });
  // SkiaCanvas::drawGlyphs: raw glyph IDs and positions with an explicit font.
  r.bind(bc, "nDrawGlyphs", "(J[I[FIIIJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t glyph_offset = a.i32(4), position_offset = a.i32(5), glyph_count = a.i32(6);
    if (glyph_count <= 0) return 0;
    if (a.h(2) == kNull) rt.throw_npe("glyphIds");
    if (a.h(3) == kNull) rt.throw_npe("positions");
    rt::ArrayObject& glyphs_arr = rt.array(a.h(2), "int[]");
    rt::ArrayObject& pos_arr = rt.array(a.h(3), "float[]");
    if (glyph_offset < 0 || size_t(glyph_offset + glyph_count) > glyphs_arr.length) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "glyphIdOffset hors limites");
    }
    if (position_offset < 0 || size_t(position_offset + glyph_count * 2) > pos_arr.length) {
      rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "positionOffset hors limites");
    }
    sk_sp<SkTypeface> tf = font_skia_typeface(rt, a.i64(7));
    if (!tf) return 0;

    SkiaPaint paint = skia_paint_of(rt, a.i64(9));
    paint.font.setTypeface(tf);

    const int32_t* glyph_ids = reinterpret_cast<const int32_t*>(glyphs_arr.data.data()) + glyph_offset;
    const float* positions = reinterpret_cast<const float*>(pos_arr.data.data()) + position_offset;

    SkTextBlobBuilder builder;
    const SkTextBlobBuilder::RunBuffer& buffer = builder.allocRunPos(paint.font, glyph_count);
    for (int32_t i = 0; i < glyph_count; ++i) {
      buffer.glyphs[i] = SkGlyphID(glyph_ids[i]);
      buffer.pos[2 * i] = positions[2 * i];
      buffer.pos[2 * i + 1] = positions[2 * i + 1];
    }
    sk_sp<SkTextBlob> blob = builder.make();
    if (!blob) return 0;

    draw(rt, a.i64(0), [&](CanvasState& c) {
      c.apply_looper(&paint, [&](const SkiaPaint& p) {
        c.canvas->drawTextBlob(blob, 0, 0, p.paint);
      });
    });
    return 0;
  });
  // SkiaCanvas::punchHole: the rounded rectangle cleared to `alpha`.
  r.bind(bc, "nPunchHole", "(JFFFFFFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    SkPaint paint;
    paint.setColor(SkColors::kBlack);
    paint.setAlphaf(a.f32(8));
    paint.setBlendMode(SkBlendMode::kDstOut);
    c.canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(a.f32(2), a.f32(3), a.f32(4), a.f32(5)), a.f32(6),
                                            a.f32(7)),
                        paint);
    return 0;
  }));
}

}  // namespace

void canvas_target_surface(Runtime& rt, Handle canvas_object, const PixelTarget& target, const int32_t* dirty) {
  rt::Object& o = rt.nonnull(canvas_object, "Canvas");
  rt::Field* f = find_field(o.klass, "mNativeCanvasWrapper", "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Canvas sans natif");
  int64_t handle = int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
  draw(rt, handle, [&](CanvasState& c) {
    c.set_target(target);
    if (dirty) c.clip_rect(SkRect::MakeLTRB(float(dirty[0]), float(dirty[1]), float(dirty[2]), float(dirty[3])), 1);
  });
}

void register_framework_canvas(rt::NativeRegistry& r) {
  register_canvas_state(r);
  register_canvas_draw(r);
}

}  // namespace aa::bridge
