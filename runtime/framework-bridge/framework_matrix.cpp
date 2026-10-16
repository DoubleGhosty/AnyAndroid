// android.graphics.Matrix natives (G3, ADR-006 J4), each signature checked
// against the framework.jar of Android 17: SkMatrix's 3x3 matrix, with
// Skia's formulas and order of operations (setConcat, invert, mapPoints,
// mapRect, setRectToRect). The matrix type (identity, translate, scale,
// affine, perspective) is computed from the values, as SkMatrix does when its
// cached mask is unknown. setPolyToPoly stays unbound until asked for.
#include "framework-bridge/internal.h"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

// SkMatrix's layout, also Matrix.getValues's.
enum { kScaleX, kSkewX, kTransX, kSkewY, kScaleY, kTransY, kPersp0, kPersp1, kPersp2 };
using M = std::array<float, 9>;

constexpr M kIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};
constexpr float kNearlyZero = 1.0f / (1 << 12);  // SK_ScalarNearlyZero

enum : unsigned { kTranslateMask = 1, kScaleMask = 2, kAffineMask = 4, kPerspectiveMask = 8, kRectStaysRectMask = 16 };

// SkMatrix::computeTypeMask (-0 counts as 0).
unsigned type_of(const M& m) {
  if (m[kPersp0] != 0 || m[kPersp1] != 0 || m[kPersp2] != 1) {
    return kTranslateMask | kScaleMask | kAffineMask | kPerspectiveMask;
  }
  unsigned mask = 0;
  if (m[kTransX] != 0 || m[kTransY] != 0) mask |= kTranslateMask;
  if (m[kSkewX] != 0 || m[kSkewY] != 0) {
    mask |= kAffineMask | kScaleMask;
    if (m[kScaleX] == 0 && m[kScaleY] == 0 && m[kSkewX] != 0 && m[kSkewY] != 0) mask |= kRectStaysRectMask;
  } else {
    if (m[kScaleX] != 1 || m[kScaleY] != 1) mask |= kScaleMask;
    if (m[kScaleX] != 0 && m[kScaleY] != 0) mask |= kRectStaysRectMask;
  }
  return mask;
}

bool is_identity(const M& m) { return (type_of(m) & 0xF) == 0; }
bool has_perspective(const M& m) { return (type_of(m) & kPerspectiveMask) != 0; }
bool scale_translate_only(unsigned mask) { return (mask & ~(kScaleMask | kTranslateMask) & 0xF) == 0; }

M scale_translate(float sx, float sy, float tx, float ty) { return {sx, 0, tx, 0, sy, ty, 0, 0, 1}; }

float muladdmul(float a, float b, float c, float d) { return float(double(a) * b + double(c) * d); }
float rowcol3(const float* row, const float* col) { return row[0] * col[0] + row[1] * col[3] + row[2] * col[6]; }

// SkMatrix::setConcat: a * b.
M concat(const M& a, const M& b) {
  unsigned ta = type_of(a), tb = type_of(b);
  if ((ta & 0xF) == 0) return b;
  if ((tb & 0xF) == 0) return a;
  if (scale_translate_only(ta | tb)) {
    return scale_translate(a[kScaleX] * b[kScaleX], a[kScaleY] * b[kScaleY], a[kScaleX] * b[kTransX] + a[kTransX],
                           a[kScaleY] * b[kTransY] + a[kTransY]);
  }
  M r;
  if ((ta | tb) & kPerspectiveMask) {
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) r[size_t(row * 3 + col)] = rowcol3(&a[size_t(row * 3)], &b[size_t(col)]);
    }
    // normalize_perspective: a [0, 0, w] bottom row is divided out.
    if (r[kPersp0] == 0 && r[kPersp1] == 0 && r[kPersp2] != 0 && r[kPersp2] != 1) {
      double inv = 1.0 / r[kPersp2];
      for (size_t i = 0; i < 6; ++i) r[i] = float(r[i] * inv);
      r[kPersp2] = 1;
    }
    return r;
  }
  r[kScaleX] = muladdmul(a[kScaleX], b[kScaleX], a[kSkewX], b[kSkewY]);
  r[kSkewX] = muladdmul(a[kScaleX], b[kSkewX], a[kSkewX], b[kScaleY]);
  r[kTransX] = muladdmul(a[kScaleX], b[kTransX], a[kSkewX], b[kTransY]) + a[kTransX];
  r[kSkewY] = muladdmul(a[kSkewY], b[kScaleX], a[kScaleY], b[kSkewY]);
  r[kScaleY] = muladdmul(a[kSkewY], b[kSkewX], a[kScaleY], b[kScaleY]);
  r[kTransY] = muladdmul(a[kSkewY], b[kTransX], a[kScaleY], b[kTransY]) + a[kTransY];
  r[kPersp0] = 0;
  r[kPersp1] = 0;
  r[kPersp2] = 1;
  return r;
}

M translate(float dx, float dy) { return scale_translate(1, 1, dx, dy); }

M scale(float sx, float sy, float px, float py) {
  if (sx == 1 && sy == 1) return kIdentity;
  return scale_translate(sx, sy, px - sx * px, py - sy * py);
}

M sin_cos(float s, float c, float px, float py) {
  float one_minus_c = 1 - c;
  return {c, -s, s * py + one_minus_c * px, s, c, -s * px + one_minus_c * py, 0, 0, 1};
}

float snap_to_zero(float v) { return std::fabs(v) <= kNearlyZero ? 0.0f : v; }

// SkDegreesToRadians, in float as Skia computes it.
M rotate(float degrees, float px, float py) {
  constexpr float kPi = 3.14159265f;
  float rad = degrees * (kPi / 180);
  return sin_cos(snap_to_zero(std::sin(rad)), snap_to_zero(std::cos(rad)), px, py);
}

M skew(float kx, float ky, float px, float py) { return {1, kx, -kx * py, ky, 1, -ky * px, 0, 0, 1}; }

void pre_concat(M& m, const M& other) {
  if (!is_identity(other)) m = concat(m, other);
}

// SkMatrix::preTranslate.
void pre_translate(M& m, float dx, float dy) {
  unsigned mask = type_of(m);
  if ((mask & 0xF) <= kTranslateMask) {
    m[kTransX] += dx;
    m[kTransY] += dy;
  } else if (mask & kPerspectiveMask) {
    pre_concat(m, translate(dx, dy));
  } else {
    m[kTransX] += m[kScaleX] * dx + m[kSkewX] * dy;
    m[kTransY] += m[kSkewY] * dx + m[kScaleY] * dy;
  }
}

// SkMatrix::preScale(sx, sy).
void pre_scale(M& m, float sx, float sy) {
  if (sx == 1 && sy == 1) return;
  m[kScaleX] *= sx;
  m[kSkewY] *= sx;
  m[kPersp0] *= sx;
  m[kSkewX] *= sy;
  m[kScaleY] *= sy;
  m[kPersp1] *= sy;
}
void post_concat(M& m, const M& other) {
  if (!is_identity(other)) m = concat(other, m);
}

float scross_dscale(float a, float b, float c, float d, double scale) { return float(double(a * b - c * d) * scale); }
float dcross_dscale(double a, double b, double c, double d, double scale) { return float((a * b - c * d) * scale); }

// SkMatrix::invert.
bool invert(const M& m, M& out) {
  unsigned mask = type_of(m);
  if ((mask & 0xF) == 0) {
    out = kIdentity;
    return true;
  }
  if (scale_translate_only(mask)) {
    if (mask & kScaleMask) {
      float ix = 1.f / m[kScaleX], iy = 1.f / m[kScaleY];
      if (!std::isfinite(ix) || !std::isfinite(iy)) return false;
      out = scale_translate(ix, iy, -m[kTransX] * ix, -m[kTransY] * iy);
    } else {
      out = translate(-m[kTransX], -m[kTransY]);
    }
    return true;
  }
  bool persp = mask & kPerspectiveMask;
  double det;
  if (persp) {
    det = m[kScaleX] * (double(m[kScaleY]) * m[kPersp2] - double(m[kTransY]) * m[kPersp1]) +
          m[kSkewX] * (double(m[kTransY]) * m[kPersp0] - double(m[kSkewY]) * m[kPersp2]) +
          m[kTransX] * (double(m[kSkewY]) * m[kPersp1] - double(m[kScaleY]) * m[kPersp0]);
  } else {
    det = double(m[kScaleX]) * m[kScaleY] - double(m[kSkewX]) * m[kSkewY];
  }
  if (std::fabs(float(det)) <= kNearlyZero * kNearlyZero * kNearlyZero) return false;
  double inv = 1.0 / det;
  M r;
  if (persp) {
    r[kScaleX] = scross_dscale(m[kScaleY], m[kPersp2], m[kTransY], m[kPersp1], inv);
    r[kSkewX] = scross_dscale(m[kTransX], m[kPersp1], m[kSkewX], m[kPersp2], inv);
    r[kTransX] = scross_dscale(m[kSkewX], m[kTransY], m[kTransX], m[kScaleY], inv);
    r[kSkewY] = scross_dscale(m[kTransY], m[kPersp0], m[kSkewY], m[kPersp2], inv);
    r[kScaleY] = scross_dscale(m[kScaleX], m[kPersp2], m[kTransX], m[kPersp0], inv);
    r[kTransY] = scross_dscale(m[kTransX], m[kSkewY], m[kScaleX], m[kTransY], inv);
    r[kPersp0] = scross_dscale(m[kSkewY], m[kPersp1], m[kScaleY], m[kPersp0], inv);
    r[kPersp1] = scross_dscale(m[kSkewX], m[kPersp0], m[kScaleX], m[kPersp1], inv);
    r[kPersp2] = scross_dscale(m[kScaleX], m[kScaleY], m[kSkewX], m[kSkewY], inv);
  } else {
    r[kScaleX] = float(m[kScaleY] * inv);
    r[kSkewX] = float(-m[kSkewX] * inv);
    r[kTransX] = dcross_dscale(m[kSkewX], m[kTransY], m[kScaleY], m[kTransX], inv);
    r[kSkewY] = float(-m[kSkewY] * inv);
    r[kScaleY] = float(m[kScaleX] * inv);
    r[kTransY] = dcross_dscale(m[kSkewY], m[kTransX], m[kScaleX], m[kTransY], inv);
    r[kPersp0] = 0;
    r[kPersp1] = 0;
    r[kPersp2] = 1;
  }
  for (float v : r) {
    if (!std::isfinite(v)) return false;
  }
  out = r;
  return true;
}

// SkMatrix::mapPoints (the procedure of the matrix type).
void map_point(const M& m, unsigned mask, float x, float y, float& ox, float& oy) {
  if (mask & kPerspectiveMask) {
    float px = x * m[kScaleX] + y * m[kSkewX] + m[kTransX];
    float py = x * m[kSkewY] + y * m[kScaleY] + m[kTransY];
    float z = x * m[kPersp0] + y * m[kPersp1] + m[kPersp2];
    if (z != 0) z = 1 / z;
    ox = px * z;
    oy = py * z;
  } else if (mask & kAffineMask) {
    ox = x * m[kScaleX] + y * m[kSkewX] + m[kTransX];
    oy = x * m[kSkewY] + y * m[kScaleY] + m[kTransY];
  } else if (mask & kScaleMask) {
    ox = x * m[kScaleX] + m[kTransX];
    oy = y * m[kScaleY] + m[kTransY];
  } else if (mask & kTranslateMask) {
    ox = x + m[kTransX];
    oy = y + m[kTransY];
  } else {
    ox = x;
    oy = y;
  }
}

// SkMatrix::mapVectors: the translation does not apply.
void map_vector(const M& m, float x, float y, float& ox, float& oy) {
  if (has_perspective(m)) {
    float x0, y0;
    unsigned mask = type_of(m);
    map_point(m, mask, 0, 0, x0, y0);
    map_point(m, mask, x, y, ox, oy);
    ox -= x0;
    oy -= y0;
    return;
  }
  M t = m;
  t[kTransX] = t[kTransY] = 0;
  map_point(t, type_of(t), x, y, ox, oy);
}

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
};

Rect sorted(float l, float t, float r, float b) { return {std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b)}; }

// SkMatrix::mapRect: true when the result is exactly the mapped rectangle.
bool map_rect(const M& m, const Rect& src, Rect& dst) {
  unsigned mask = type_of(m);
  if ((mask & 0xF) <= kTranslateMask) {
    dst = sorted(src.l + m[kTransX], src.t + m[kTransY], src.r + m[kTransX], src.b + m[kTransY]);
    return true;
  }
  if (scale_translate_only(mask)) {
    dst = sorted(src.l * m[kScaleX] + m[kTransX], src.t * m[kScaleY] + m[kTransY], src.r * m[kScaleX] + m[kTransX],
                 src.b * m[kScaleY] + m[kTransY]);
    return true;
  }
  float xs[4], ys[4];
  const float in[4][2] = {{src.l, src.t}, {src.r, src.t}, {src.r, src.b}, {src.l, src.b}};
  for (int i = 0; i < 4; ++i) map_point(m, mask, in[i][0], in[i][1], xs[i], ys[i]);
  dst = {xs[0], ys[0], xs[0], ys[0]};
  for (int i = 1; i < 4; ++i) {
    dst = {std::min(dst.l, xs[i]), std::min(dst.t, ys[i]), std::max(dst.r, xs[i]), std::max(dst.b, ys[i])};
  }
  return !(mask & kPerspectiveMask) && (mask & kRectStaysRectMask);
}

// SkMatrix::setRectToRect (ScaleToFit: FILL 0, START 1, CENTER 2, END 3).
bool rect_to_rect(const Rect& src, const Rect& dst, int32_t fit, M& out) {
  auto empty = [](const Rect& r) { return !(r.l < r.r && r.t < r.b); };
  if (empty(src)) {
    out = kIdentity;
    return false;
  }
  if (empty(dst)) {
    out = {0, 0, 0, 0, 0, 0, 0, 0, 1};
    return true;
  }
  float sx = (dst.r - dst.l) / (src.r - src.l), sy = (dst.b - dst.t) / (src.b - src.t);
  bool x_larger = false;
  if (fit != 0) {
    if (sx > sy) {
      x_larger = true;
      sx = sy;
    } else {
      sy = sx;
    }
  }
  float tx = dst.l - src.l * sx, ty = dst.t - src.t * sy;
  if (fit == 2 || fit == 3) {
    float diff = x_larger ? (dst.r - dst.l) - (src.r - src.l) * sy : (dst.b - dst.t) - (src.b - src.t) * sy;
    if (fit == 2) diff = diff / 2;
    if (x_larger) {
      tx += diff;
    } else {
      ty += diff;
    }
  }
  out = scale_translate(sx, sy, tx, ty);
  return true;
}

struct Matrix {
  std::mutex lock;
  M m = kIdentity;
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Matrix>& matrices() {
  static auto* t = new HandleTable<Matrix>(0x110A);  // never destroyed
  return *t;
}

std::shared_ptr<Matrix> matrix(Runtime& rt, int64_t h) {
  auto m = matrices().get(h);
  if (!m) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Matrix invalide");
  return m;
}

M snapshot(Runtime& rt, int64_t h) {
  auto m = matrix(rt, h);
  std::lock_guard<std::mutex> guard(m->lock);
  return m->m;
}

// Runs fn(values, args) under the matrix's lock; the matrix is argument 0.
template <typename Fn>
rt::NativeFn edit(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = matrix(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(m->lock);
    fn(m->m, a);
    return 0;
  };
}

// The same, with a second matrix's values read first (it may be the same one).
template <typename Fn>
rt::NativeFn edit_with(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    M other = snapshot(rt, a.i64(2));
    auto m = matrix(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(m->lock);
    fn(m->m, other);
    return 0;
  };
}

float float_field(rt::Object& o, const char* name) {
  rt::Field* f = find_field(o.klass, name, "F");
  if (!f || f->is_static()) return 0;
  float v;
  uint32_t bits = o.fields.at(f->slot);
  std::memcpy(&v, &bits, 4);
  return v;
}

void set_float_field(rt::Object& o, const char* name, float v) {
  rt::Field* f = find_field(o.klass, name, "F");
  if (!f || f->is_static()) return;
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  o.fields.at(f->slot) = bits;
}

Rect read_rect(Runtime& rt, Handle h, const char* what) {
  rt::Object& o = rt.nonnull(h, what);
  return {float_field(o, "left"), float_field(o, "top"), float_field(o, "right"), float_field(o, "bottom")};
}

void write_rect(Runtime& rt, Handle h, const Rect& r) {
  rt::Object& o = rt.nonnull(h, "dst");
  set_float_field(o, "left", r.l);
  set_float_field(o, "top", r.t);
  set_float_field(o, "right", r.r);
  set_float_field(o, "bottom", r.b);
}

// AutoJavaFloatArray: at least `min` floats, else ArrayIndexOutOfBoundsException.
float* floats(Runtime& rt, Handle h, int64_t min) {
  if (h == kNull) rt.throw_npe("array");
  rt::ArrayObject& a = rt.array(h, "float[]");
  if (a.width != 4 || min < 0 || int64_t(a.length) < min) {
    rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "tableau de float trop court");
  }
  return reinterpret_cast<float*>(a.data.data());
}

}  // namespace

namespace skmatrix {

Values identity() { return kIdentity; }
void pre_translate(Values& m, float dx, float dy) { bridge::pre_translate(m, dx, dy); }
void pre_scale(Values& m, float sx, float sy) { bridge::pre_scale(m, sx, sy); }
void pre_rotate(Values& m, float degrees) { bridge::pre_concat(m, rotate(degrees, 0, 0)); }
void pre_skew(Values& m, float kx, float ky) { bridge::pre_concat(m, skew(kx, ky, 0, 0)); }
void pre_concat(Values& m, const Values& other) { bridge::pre_concat(m, other); }
bool invert(const Values& m, Values& out) { return bridge::invert(m, out); }
bool is_scale_translate(const Values& m) { return scale_translate_only(type_of(m)); }
bool map_rect(const Values& m, std::array<float, 4>& rect) {
  Rect out;
  bool exact = bridge::map_rect(m, {rect[0], rect[1], rect[2], rect[3]}, out);
  rect = {out.l, out.t, out.r, out.b};
  return exact;
}
Values of(rt::Runtime& rt, int64_t handle) { return snapshot(rt, handle); }
void assign(rt::Runtime& rt, int64_t handle, const Values& values) {
  auto m = matrix(rt, handle);
  std::lock_guard<std::mutex> guard(m->lock);
  m->m = values;
}

}  // namespace skmatrix

void register_framework_matrix(rt::NativeRegistry& r) {
  const char* ex = "Landroid/graphics/Matrix$ExtraNatives;";
  static const int64_t matrix_free = register_free_function([](int64_t h) { matrices().remove(h); });
  r.bind(ex, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(matrix_free); });
  // A copy of the source, or the identity for 0.
  r.bind(ex, "nCreate", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto m = std::make_shared<Matrix>();
    if (a.i64(0) != 0) m->m = snapshot(rt, a.i64(0));
    return uint64_t(matrices().add(std::move(m)));
  });

  const char* mx = "Landroid/graphics/Matrix;";
  r.bind(mx, "nIsIdentity", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(is_identity(snapshot(rt, a.i64(0))));
  });
  r.bind(mx, "nIsAffine", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(!has_perspective(snapshot(rt, a.i64(0))));
  });
  r.bind(mx, "nRectStaysRect", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(type_of(snapshot(rt, a.i64(0))) & kRectStaysRectMask);
  });
  r.bind(mx, "nEquals", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M x = snapshot(rt, a.i64(0)), y = snapshot(rt, a.i64(2));
    for (size_t i = 0; i < 9; ++i) {
      if (!(x[i] == y[i])) return rt::ret_bool(false);
    }
    return rt::ret_bool(true);
  });
  r.bind(mx, "nReset", "(J)V", edit([](M& m, NativeArgs&) { m = kIdentity; }));
  r.bind(mx, "nSet", "(JJ)V", edit_with([](M& m, const M& other) { m = other; }));
  r.bind(mx, "nSetConcat", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M x = snapshot(rt, a.i64(2)), y = snapshot(rt, a.i64(4));
    auto m = matrix(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(m->lock);
    m->m = concat(x, y);
    return 0;
  });
  r.bind(mx, "nPreConcat", "(JJ)V", edit_with([](M& m, const M& other) { pre_concat(m, other); }));
  r.bind(mx, "nPostConcat", "(JJ)V", edit_with([](M& m, const M& other) { post_concat(m, other); }));

  // set*, pre* and post* of each elementary transformation.
  r.bind(mx, "nSetTranslate", "(JFF)V", edit([](M& m, NativeArgs& a) { m = translate(a.f32(2), a.f32(3)); }));
  r.bind(mx, "nPreTranslate", "(JFF)V", edit([](M& m, NativeArgs& a) { pre_translate(m, a.f32(2), a.f32(3)); }));
  r.bind(mx, "nPostTranslate", "(JFF)V", edit([](M& m, NativeArgs& a) {
    if (has_perspective(m)) {
      post_concat(m, translate(a.f32(2), a.f32(3)));
    } else {
      m[kTransX] += a.f32(2);
      m[kTransY] += a.f32(3);
    }
  }));
  r.bind(mx, "nSetScale", "(JFF)V", edit([](M& m, NativeArgs& a) { m = scale_translate(a.f32(2), a.f32(3), 0, 0); }));
  r.bind(mx, "nSetScale", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    m = scale(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
  }));
  r.bind(mx, "nPreScale", "(JFF)V", edit([](M& m, NativeArgs& a) { pre_scale(m, a.f32(2), a.f32(3)); }));
  r.bind(mx, "nPreScale", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    pre_concat(m, scale(a.f32(2), a.f32(3), a.f32(4), a.f32(5)));
  }));
  r.bind(mx, "nPostScale", "(JFF)V", edit([](M& m, NativeArgs& a) {
    post_concat(m, scale_translate(a.f32(2), a.f32(3), 0, 0));
  }));
  r.bind(mx, "nPostScale", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    post_concat(m, scale(a.f32(2), a.f32(3), a.f32(4), a.f32(5)));
  }));
  r.bind(mx, "nSetRotate", "(JF)V", edit([](M& m, NativeArgs& a) { m = rotate(a.f32(2), 0, 0); }));
  r.bind(mx, "nSetRotate", "(JFFF)V", edit([](M& m, NativeArgs& a) { m = rotate(a.f32(2), a.f32(3), a.f32(4)); }));
  r.bind(mx, "nPreRotate", "(JF)V", edit([](M& m, NativeArgs& a) { pre_concat(m, rotate(a.f32(2), 0, 0)); }));
  r.bind(mx, "nPreRotate", "(JFFF)V", edit([](M& m, NativeArgs& a) {
    pre_concat(m, rotate(a.f32(2), a.f32(3), a.f32(4)));
  }));
  r.bind(mx, "nPostRotate", "(JF)V", edit([](M& m, NativeArgs& a) { post_concat(m, rotate(a.f32(2), 0, 0)); }));
  r.bind(mx, "nPostRotate", "(JFFF)V", edit([](M& m, NativeArgs& a) {
    post_concat(m, rotate(a.f32(2), a.f32(3), a.f32(4)));
  }));
  r.bind(mx, "nSetSinCos", "(JFF)V", edit([](M& m, NativeArgs& a) { m = sin_cos(a.f32(2), a.f32(3), 0, 0); }));
  r.bind(mx, "nSetSinCos", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    m = sin_cos(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
  }));
  r.bind(mx, "nSetSkew", "(JFF)V", edit([](M& m, NativeArgs& a) { m = skew(a.f32(2), a.f32(3), 0, 0); }));
  r.bind(mx, "nSetSkew", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    m = skew(a.f32(2), a.f32(3), a.f32(4), a.f32(5));
  }));
  r.bind(mx, "nPreSkew", "(JFF)V", edit([](M& m, NativeArgs& a) { pre_concat(m, skew(a.f32(2), a.f32(3), 0, 0)); }));
  r.bind(mx, "nPreSkew", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    pre_concat(m, skew(a.f32(2), a.f32(3), a.f32(4), a.f32(5)));
  }));
  r.bind(mx, "nPostSkew", "(JFF)V", edit([](M& m, NativeArgs& a) { post_concat(m, skew(a.f32(2), a.f32(3), 0, 0)); }));
  r.bind(mx, "nPostSkew", "(JFFFF)V", edit([](M& m, NativeArgs& a) {
    post_concat(m, skew(a.f32(2), a.f32(3), a.f32(4), a.f32(5)));
  }));

  r.bind(mx, "nInvert", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M src = snapshot(rt, a.i64(0)), inv;
    auto dst = matrix(rt, a.i64(2));
    if (!invert(src, inv)) return rt::ret_bool(false);
    std::lock_guard<std::mutex> guard(dst->lock);
    dst->m = inv;
    return rt::ret_bool(true);
  });
  r.bind(mx, "nGetValues", "(J[F)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M m = snapshot(rt, a.i64(0));
    std::memcpy(floats(rt, a.h(2), 9), m.data(), sizeof(m));
    return 0;
  });
  r.bind(mx, "nSetValues", "(J[F)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M v;
    std::memcpy(v.data(), floats(rt, a.h(2), 9), sizeof(v));
    auto m = matrix(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(m->lock);
    m->m = v;
    return 0;
  });
  // mapPoints/mapVectors(dst, dstIndex, src, srcIndex, pointCount, isPoints);
  // the arrays may be the same one.
  r.bind(mx, "nMapPoints", "(J[FI[FIIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M m = snapshot(rt, a.i64(0));
    int32_t dst_index = a.i32(3), src_index = a.i32(5), count = a.i32(6);
    if (count < 0 || dst_index < 0 || src_index < 0) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "mapPoints");
    }
    float* src = floats(rt, a.h(4), int64_t(src_index) + 2 * int64_t(count)) + src_index;
    float* dst = floats(rt, a.h(2), int64_t(dst_index) + 2 * int64_t(count)) + dst_index;
    unsigned mask = type_of(m);
    for (int32_t i = 0; i < count; ++i) {
      float x = src[2 * i], y = src[2 * i + 1], ox, oy;
      if (a.z(7)) {
        map_point(m, mask, x, y, ox, oy);
      } else {
        map_vector(m, x, y, ox, oy);
      }
      dst[2 * i] = ox;
      dst[2 * i + 1] = oy;
    }
    return 0;
  });
  r.bind(mx, "nMapRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M m = snapshot(rt, a.i64(0));
    Rect src = read_rect(rt, a.h(3), "src"), dst;
    bool stays = map_rect(m, src, dst);
    write_rect(rt, a.h(2), dst);
    return rt::ret_bool(stays);
  });
  // The geometric mean of the lengths of the mapped (radius, 0) and (0, radius).
  r.bind(mx, "nMapRadius", "(JF)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    M m = snapshot(rt, a.i64(0));
    float radius = a.f32(2), x0, y0, x1, y1;
    map_vector(m, radius, 0, x0, y0);
    map_vector(m, 0, radius, x1, y1);
    float d0 = std::sqrt(x0 * x0 + y0 * y0), d1 = std::sqrt(x1 * x1 + y1 * y1);
    return rt::ret_float(std::sqrt(d0 * d1));
  });
  r.bind(mx, "nSetRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           Rect src = read_rect(rt, a.h(2), "src"), dst = read_rect(rt, a.h(3), "dst");
           M out;
           bool ok = rect_to_rect(src, dst, a.i32(4), out);
           auto m = matrix(rt, a.i64(0));
           std::lock_guard<std::mutex> guard(m->lock);
           m->m = out;
           return rt::ret_bool(ok);
         });
}

}  // namespace aa::bridge
