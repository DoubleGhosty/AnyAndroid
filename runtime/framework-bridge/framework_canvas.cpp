// android.graphics.Canvas state natives (G3, ADR-006 J4), each signature
// checked against the framework.jar of Android 17: HWUI's SkiaCanvas over an
// SkCanvas, without the pixels. The target bitmap's size, the save stack
// (save, saveLayer, restore, restoreToCount, HWUI's partial saves), the
// current matrix (SkMatrix's formulas) and the clip follow Skia. The clip is
// kept as its device bounds: exact for rectangles under scale and translate
// (rounded as Skia's non antialiased clip), conservative after a difference,
// a path or a rotation. Drawing waits for the software renderer (J7): the
// draw natives of BaseCanvas stay unbound until then.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;
using Matrix = skmatrix::Values;

struct IRect {
  int32_t l = 0, t = 0, r = 0, b = 0;
  bool empty() const { return !(l < r && t < b); }
};

IRect intersect(const IRect& a, const IRect& b) {
  IRect out{std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
  return out.empty() ? IRect{} : out;
}

// SkScalarRoundToInt, saturated.
int32_t round_int(float v) {
  double d = std::floor(double(v) + 0.5);
  if (!(d > -2147483648.0)) return INT32_MIN;
  if (d > 2147483647.0) return INT32_MAX;
  return int32_t(d);
}

int32_t floor_int(float v) { return round_int(std::floor(v)); }
int32_t ceil_int(float v) { return round_int(std::ceil(v)); }

struct Layer {
  Matrix matrix = skmatrix::identity();
  IRect clip;           // device clip bounds
  bool clip_exact = true;  // false: the clip is smaller than its bounds
};

// HWUI's SaveRec: a save that keeps the matrix or the clip on restore.
struct PartialSave {
  int32_t save_count;
  int32_t flags;  // SaveFlags::Matrix 1, Clip 2
};

struct CanvasState {
  std::mutex lock;
  int32_t width = 0, height = 0;
  bool opaque = false;
  std::vector<Layer> stack{Layer{}};
  std::vector<PartialSave> partial;
  int64_t draw_filter = 0;

  void set_target(int32_t w, int32_t h, bool is_opaque) {
    width = w;
    height = h;
    opaque = is_opaque;
    stack.assign(1, Layer{skmatrix::identity(), IRect{0, 0, w, h}, true});
    if (stack[0].clip.empty()) stack[0].clip = IRect{};
    partial.clear();
  }
  Layer& top() { return stack.back(); }
  int32_t save_count() const { return int32_t(stack.size()); }

  int32_t save() {
    int32_t count = save_count();
    stack.push_back(stack.back());
    return count;
  }

  // SkiaCanvas::restore, with its partial saves.
  void restore() {
    if (stack.size() <= 1) return;
    bool partial_frame = !partial.empty() && partial.back().save_count == save_count();
    int32_t flags = partial_frame ? partial.back().flags : 3;
    Layer popped = stack.back();
    stack.pop_back();
    if (partial_frame) partial.pop_back();
    if (!(flags & 1)) top().matrix = popped.matrix;
    if (!(flags & 2)) {
      top().clip = popped.clip;
      top().clip_exact = popped.clip_exact;
    }
  }

  void restore_to_count(int32_t count) {
    while (save_count() > count && stack.size() > 1) restore();
  }

  // SkCanvas::clipRect without antialiasing (SkiaCanvas::clipRect).
  // Region.Op: DIFFERENCE 0, INTERSECT 1, REPLACE 5; the others are ignored.
  void clip_rect(std::array<float, 4> r, int32_t op) {
    Layer& layer = top();
    bool exact = skmatrix::map_rect(layer.matrix, r);
    if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]) || !std::isfinite(r[3])) {
      if (op != 0) layer.clip = IRect{};
      return;
    }
    IRect dev{round_int(r[0]), round_int(r[1]), round_int(r[2]), round_int(r[3])};
    if (op == 5) {  // replace (before Android P): the clip is reset to the device first
      layer.clip = intersect(IRect{0, 0, width, height}, dev);
      layer.clip_exact = exact;
    } else if (op == 1) {
      layer.clip = intersect(layer.clip, dev);
      layer.clip_exact = layer.clip_exact && exact;
      if (layer.clip.empty()) layer.clip_exact = true;
    } else if (op == 0) {
      difference(layer, dev, exact);
    }
  }

  // The bounds of clip minus `dev`: smaller only when `dev` covers whole
  // edges of an exact clip.
  void difference(Layer& layer, const IRect& dev, bool exact) {
    IRect& c = layer.clip;
    if (c.empty() || intersect(c, dev).empty()) return;
    if (!exact || !layer.clip_exact) {
      layer.clip_exact = false;
      return;
    }
    bool covers_x = dev.l <= c.l && dev.r >= c.r, covers_y = dev.t <= c.t && dev.b >= c.b;
    if (covers_x && covers_y) {
      c = IRect{};
      layer.clip_exact = true;
    } else if (covers_x && dev.t <= c.t) {
      c.t = dev.b;
    } else if (covers_x && dev.b >= c.b) {
      c.b = dev.t;
    } else if (covers_y && dev.l <= c.l) {
      c.l = dev.r;
    } else if (covers_y && dev.r >= c.r) {
      c.r = dev.l;
    } else {
      layer.clip_exact = false;  // a hole or a notch: the bounds stay
    }
  }

  // SkCanvas::clipPath with antialiasing (SkiaCanvas::clipPath), on the
  // path's bounds.
  void clip_path(std::array<float, 4> bounds, bool path_empty, int32_t op) {
    Layer& layer = top();
    if (op == 0) {  // difference: the bounds stay, the clip is no longer exact
      if (!path_empty) layer.clip_exact = false;
      return;
    }
    if (op != 1 && op != 5) return;
    if (path_empty) {
      layer.clip = IRect{};
      layer.clip_exact = true;
      return;
    }
    skmatrix::map_rect(layer.matrix, bounds);
    IRect dev{floor_int(bounds[0]), floor_int(bounds[1]), ceil_int(bounds[2]), ceil_int(bounds[3])};
    IRect base = op == 5 ? IRect{0, 0, width, height} : layer.clip;
    layer.clip = intersect(base, dev);
    layer.clip_exact = layer.clip.empty();
  }

  // SkCanvas::quickReject: against the device clip outset by 1.
  bool quick_reject(std::array<float, 4> r) {
    const Layer& layer = top();
    if (layer.clip.empty()) return true;
    skmatrix::map_rect(layer.matrix, r);
    for (float v : r) {
      if (!std::isfinite(v)) return true;
    }
    float l = float(layer.clip.l) - 1, t = float(layer.clip.t) - 1;
    float rr = float(layer.clip.r) + 1, b = float(layer.clip.b) + 1;
    return !(std::max(r[0], l) < std::min(r[2], rr) && std::max(r[1], t) < std::min(r[3], b));
  }
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<CanvasState>& canvases() {
  static auto* t = new HandleTable<CanvasState>(0x110F);  // never destroyed
  return *t;
}

std::shared_ptr<CanvasState> canvas(Runtime& rt, int64_t h) {
  auto c = canvases().get(h);
  if (!c) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Canvas invalide");
  return c;
}

// Runs fn(state, args) under the canvas's lock; the canvas is argument 0.
template <typename Fn>
rt::NativeFn with_canvas(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    return fn(*c, a);
  };
}

void target_bitmap(Runtime& rt, CanvasState& c, int64_t bitmap) {
  int32_t w = 0, h = 0;
  bool opaque = false;
  if (bitmap != 0) bitmap_canvas_info(rt, bitmap, w, h, opaque);
  c.set_target(w, h, opaque);
}

// The bounds of a Path (its points, control points included) and whether it
// has no verbs (SkPath::isEmpty).
void path_bounds(Runtime& rt, int64_t path, std::array<float, 4>& bounds, bool& empty) {
  std::vector<uint8_t> verbs;
  std::vector<float> coords, weights;
  path_contents(rt, path, verbs, coords, weights);
  empty = verbs.empty();
  bounds = {0, 0, 0, 0};
  for (size_t i = 0; i + 1 < coords.size(); i += 2) {
    if (i == 0) {
      bounds = {coords[0], coords[1], coords[0], coords[1]};
    } else {
      bounds = {std::min(bounds[0], coords[i]), std::min(bounds[1], coords[i + 1]), std::max(bounds[2], coords[i]),
                std::max(bounds[3], coords[i + 1])};
    }
  }
}

void set_int_field(Runtime& rt, Handle obj, const char* name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, "Rect");
  rt::Field* f = find_field(o.klass, name, "I");
  if (f && !f->is_static()) o.fields.at(f->slot) = uint32_t(v);
}

}  // namespace

void register_framework_canvas(rt::NativeRegistry& r) {
  const char* cv = "Landroid/graphics/Canvas;";
  static const int64_t canvas_free = register_free_function([](int64_t h) { canvases().remove(h); });
  r.bind(cv, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(canvas_free); });
  r.bind(cv, "nInitRaster", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = std::make_shared<CanvasState>();
    target_bitmap(rt, *c, a.i64(0));
    return rt::ret_long(canvases().add(std::move(c)));
  });
  r.bind(cv, "nSetBitmap", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    target_bitmap(rt, *c, a.i64(2));
    return 0;
  });
  // Nothing is cached before the renderer exists.
  r.bind(cv, "nFreeCaches", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(cv, "nFreeTextLayoutCaches", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(cv, "nSetCompatibilityVersion", "(I)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  r.bind(cv, "nIsHighContrastText", "(J)Z", with_canvas([](CanvasState&, NativeArgs&) -> uint64_t { return 0; }));
  r.bind(cv, "nIsOpaque", "(J)Z", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t { return rt::ret_bool(c.opaque); }));
  r.bind(cv, "nGetWidth", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t { return rt::ret_int(c.width); }));
  r.bind(cv, "nGetHeight", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t { return rt::ret_int(c.height); }));
  r.bind(cv, "nSetDrawFilter", "(JJ)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.draw_filter = a.i64(2);
    return 0;
  }), S::kPartial, "filtre retenu, appliqué au rendu (J7)");

  // Save stack. Flags: SaveFlags::Matrix 1, Clip 2 (others ignored).
  r.bind(cv, "nGetSaveCount", "(J)I", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    return rt::ret_int(c.save_count());
  }));
  r.bind(cv, "nSave", "(JI)I", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    int32_t count = c.save();
    int32_t flags = a.i32(2) & 3;
    if (flags != 3) c.partial.push_back({c.save_count(), flags});
    return rt::ret_int(count);
  }));
  auto save_layer = with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t { return rt::ret_int(c.save()); });
  r.bind(cv, "nSaveLayer", "(JFFFFJ)I", save_layer, S::kPartial, "calque sans pixels propres (rendu en J7)");
  r.bind(cv, "nSaveLayerAlpha", "(JFFFFI)I", save_layer, S::kPartial, "calque sans pixels propres (rendu en J7)");
  r.bind(cv, "nSaveUnclippedLayer", "(JIIII)I", save_layer, S::kPartial, "calque sans pixels propres (rendu en J7)");
  r.bind(cv, "nRestore", "(J)Z", with_canvas([](CanvasState& c, NativeArgs&) -> uint64_t {
    if (c.save_count() <= 1) return rt::ret_bool(false);
    c.restore();
    return rt::ret_bool(true);
  }));
  r.bind(cv, "nRestoreToCount", "(JI)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.restore_to_count(a.i32(2));
    return 0;
  }));
  r.bind(cv, "nRestoreUnclippedLayer", "(JIJ)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.restore_to_count(a.i32(2));
    return 0;
  }), S::kPartial, "calque sans pixels propres (rendu en J7)");

  // Current matrix (SkCanvas: pre-concatenation).
  r.bind(cv, "nTranslate", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    float dx = a.f32(2), dy = a.f32(3);
    if (dx != 0 || dy != 0) skmatrix::pre_translate(c.top().matrix, dx, dy);
    return 0;
  }));
  r.bind(cv, "nScale", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    skmatrix::pre_scale(c.top().matrix, a.f32(2), a.f32(3));
    return 0;
  }));
  r.bind(cv, "nRotate", "(JF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    skmatrix::pre_rotate(c.top().matrix, a.f32(2));
    return 0;
  }));
  r.bind(cv, "nSkew", "(JFF)V", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    skmatrix::pre_skew(c.top().matrix, a.f32(2), a.f32(3));
    return 0;
  }));
  r.bind(cv, "nConcat", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Matrix m = skmatrix::of(rt, a.i64(2));
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    skmatrix::pre_concat(c->top().matrix, m);
    return 0;
  });
  r.bind(cv, "nSetMatrix", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Matrix m = a.i64(2) == 0 ? skmatrix::identity() : skmatrix::of(rt, a.i64(2));
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    c->top().matrix = m;
    return 0;
  });
  r.bind(cv, "nGetMatrix", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Matrix m;
    {
      auto c = canvas(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(c->lock);
      m = c->top().matrix;
    }
    skmatrix::assign(rt, a.i64(2), m);
    return 0;
  });

  // Clip.
  r.bind(cv, "nClipRect", "(JFFFFI)Z", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    c.clip_rect({a.f32(2), a.f32(3), a.f32(4), a.f32(5)}, a.i32(6));
    return rt::ret_bool(!c.top().clip.empty());
  }), S::kPartial, "découpe gardée par ses bornes (exacte pour les rectangles alignés)");
  r.bind(cv, "nClipPath", "(JJI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::array<float, 4> bounds;
    bool empty;
    path_bounds(rt, a.i64(2), bounds, empty);
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    c->clip_path(bounds, empty, a.i32(4));
    return rt::ret_bool(!c->top().clip.empty());
  }, S::kPartial, "découpe par un chemin gardée par ses bornes");
  r.bind(cv, "nGetClipBounds", "(JLandroid/graphics/Rect;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle out = a.h(2);
    if (out == kNull) rt.throw_npe("Canvas.getClipBounds : Rect null");
    std::array<float, 4> local{0, 0, 0, 0};
    bool result = false;
    {
      auto c = canvas(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(c->lock);
      const Layer& layer = c->top();
      Matrix inverse;
      // SkiaCanvas::getClipBounds: the device bounds mapped back, not outset.
      if (!layer.clip.empty() && skmatrix::invert(layer.matrix, inverse)) {
        local = {float(layer.clip.l), float(layer.clip.t), float(layer.clip.r), float(layer.clip.b)};
        skmatrix::map_rect(inverse, local);
        result = true;
      }
    }
    set_int_field(rt, out, "left", round_int(local[0]));
    set_int_field(rt, out, "top", round_int(local[1]));
    set_int_field(rt, out, "right", round_int(local[2]));
    set_int_field(rt, out, "bottom", round_int(local[3]));
    return rt::ret_bool(result);
  });
  r.bind(cv, "nQuickReject", "(JFFFF)Z", with_canvas([](CanvasState& c, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(c.quick_reject({a.f32(2), a.f32(3), a.f32(4), a.f32(5)}));
  }));
  r.bind(cv, "nQuickReject", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::array<float, 4> bounds;
    bool empty;
    path_bounds(rt, a.i64(2), bounds, empty);
    auto c = canvas(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(c->lock);
    return rt::ret_bool(empty || c->quick_reject(bounds));
  });
}

}  // namespace aa::bridge
