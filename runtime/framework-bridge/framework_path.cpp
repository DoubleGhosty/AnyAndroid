// android.graphics.Path natives (G3, ADR-006 J4), each signature checked
// against the framework.jar of Android 17. A path is SkPath's model: verbs,
// points, conic weights and fill type, built with SkPath's rules (moveTo
// injected after a close, relative verbs from the last point, degenerate
// conics, the point order of rectangles, ovals and rounded rectangles, the
// radii scaling of SkRRect). What needs more of Skia's geometry (arcs,
// boolean operations, flattening, convexity, rectangle detection, matrices)
// stays unbound until the software renderer (J7). Path.approximate, which
// animations sample (PathInterpolator), is HWUI's algorithm exactly.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
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

enum Verb : uint8_t { kMove, kLine, kQuad, kConic, kCubic, kClose };

struct Pt {
  float x = 0, y = 0;
};

constexpr uint32_t kEmptyGeneration = 1;  // SkPathRef's id of empty paths
constexpr float kRoot2Over2 = 0.707106781f;
// Java Path: up to 2^24 points (16 MiB per coordinate array is plenty).
constexpr size_t kMaxPoints = size_t(1) << 24;

std::atomic<uint32_t> next_generation{kEmptyGeneration + 1};

struct PathData {
  std::vector<uint8_t> verbs;
  std::vector<Pt> points;
  std::vector<float> weights;  // one per conic
  int32_t fill_type = 0;       // WINDING
  int32_t last_move = ~0;      // SkPath::fLastMoveToIndex: negative until a moveTo follows a close
  uint32_t generation = kEmptyGeneration;  // 0: edited since last asked
};

struct Path {
  std::mutex lock;
  PathData d;
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Path>& paths() {
  static auto* t = new HandleTable<Path>(0x1109);  // never destroyed
  return *t;
}

std::shared_ptr<Path> path(Runtime& rt, int64_t h) {
  auto p = paths().get(h);
  if (!p) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Path invalide");
  return p;
}

// SkPathRef::genID: computed when asked after an edit; empty paths share one.
uint32_t generation(PathData& d) {
  if (d.generation == 0) {
    if (d.verbs.empty() && d.points.empty()) {
      d.generation = kEmptyGeneration;
    } else {
      uint32_t id;
      do {
        id = next_generation.fetch_add(1, std::memory_order_relaxed);
      } while (id == 0 || id == kEmptyGeneration);
      d.generation = id;
    }
  }
  return d.generation;
}

// Raised through `fail` (a Java exception) when the path would grow too big.
struct TooBig {};

void append(PathData& d, Verb v, std::initializer_list<Pt> pts) {
  if (d.points.size() + pts.size() > kMaxPoints || d.verbs.size() >= kMaxPoints) throw TooBig{};
  d.verbs.push_back(v);
  d.points.insert(d.points.end(), pts);
  d.generation = 0;
}

void move_to(PathData& d, Pt p) {
  d.last_move = int32_t(d.points.size());
  append(d, kMove, {p});
}

void inject_move(PathData& d) {
  if (d.last_move >= 0) return;
  move_to(d, d.verbs.empty() ? Pt{} : d.points[size_t(~d.last_move)]);
}

Pt last_point(const PathData& d) { return d.points.empty() ? Pt{} : d.points.back(); }

void line_to(PathData& d, Pt p) {
  inject_move(d);
  append(d, kLine, {p});
}

void quad_to(PathData& d, Pt p1, Pt p2) {
  inject_move(d);
  append(d, kQuad, {p1, p2});
}

void conic_to(PathData& d, Pt p1, Pt p2, float w) {
  if (!(w > 0)) {
    line_to(d, p2);
  } else if (!std::isfinite(w)) {
    line_to(d, p1);
    line_to(d, p2);
  } else if (w == 1) {
    quad_to(d, p1, p2);
  } else {
    inject_move(d);
    append(d, kConic, {p1, p2});
    d.weights.push_back(w);
  }
}

void cubic_to(PathData& d, Pt p1, Pt p2, Pt p3) {
  inject_move(d);
  append(d, kCubic, {p1, p2, p3});
}

void close(PathData& d) {
  if (!d.verbs.empty() && d.verbs.back() != kClose) append(d, kClose, {});
  if (d.last_move >= 0) d.last_move = ~d.last_move;
}

Pt add(Pt a, float dx, float dy) { return {a.x + dx, a.y + dy}; }

// SkPath_PointIterator: N points visited from `start`, clockwise or not.
template <size_t N>
struct PointIterator {
  std::array<Pt, N> pts;
  size_t current, advance;
  PointIterator(std::array<Pt, N> p, bool cw, size_t start) : pts(p), current(start % N), advance(cw ? 1 : N - 1) {}
  Pt now() const { return pts[current]; }
  Pt next() {
    current = (current + advance) % N;
    return pts[current];
  }
};

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
  float width() const { return r - l; }
  float height() const { return b - t; }
};

PointIterator<4> rect_points(const Rect& r, bool cw, size_t start) {
  return {{Pt{r.l, r.t}, Pt{r.r, r.t}, Pt{r.r, r.b}, Pt{r.l, r.b}}, cw, start};
}

void add_rect(PathData& d, const Rect& r, bool cw, size_t start) {
  auto it = rect_points(r, cw, start);
  move_to(d, it.now());
  line_to(d, it.next());
  line_to(d, it.next());
  line_to(d, it.next());
  close(d);
}

void add_oval(PathData& d, const Rect& r, bool cw, size_t start) {
  float cx = r.l + r.width() / 2, cy = r.t + r.height() / 2;
  PointIterator<4> oval({Pt{cx, r.t}, Pt{r.r, cy}, Pt{cx, r.b}, Pt{r.l, cy}}, cw, start);
  auto corners = rect_points(r, cw, start + (cw ? 0 : 1));
  move_to(d, oval.now());
  for (int i = 0; i < 4; ++i) {
    Pt corner = corners.next();
    conic_to(d, corner, oval.next(), kRoot2Over2);
  }
  close(d);
}

// SkRRect: a rectangle and the radii of its corners (upper left, upper
// right, lower right, lower left).
struct RRect {
  enum Type { kEmpty, kRect, kOval, kRounded } type = kEmpty;
  Rect rect;
  std::array<Pt, 4> radii{};
};

bool initialize_rect(RRect& rr, Rect r) {
  rr = RRect{};
  if (!std::isfinite(r.l) || !std::isfinite(r.t) || !std::isfinite(r.r) || !std::isfinite(r.b)) return false;
  rr.rect = {std::min(r.l, r.r), std::min(r.t, r.b), std::max(r.l, r.r), std::max(r.t, r.b)};
  return !(rr.rect.l >= rr.rect.r || rr.rect.t >= rr.rect.b);
}

void set_rect(RRect& rr, Rect r) {
  if (initialize_rect(rr, r)) rr.type = RRect::kRect;
}

bool clamp_to_zero(std::array<Pt, 4>& radii) {
  bool square = true;
  for (Pt& p : radii) {
    if (p.x <= 0 || p.y <= 0) {
      p = Pt{};
    } else {
      square = false;
    }
  }
  return square;
}

void compute_type(RRect& rr) {
  bool equal = true, square = rr.radii[0].x == 0 || rr.radii[0].y == 0;
  for (size_t i = 1; i < 4; ++i) {
    if (rr.radii[i].x != 0 && rr.radii[i].y != 0) square = false;
    if (rr.radii[i].x != rr.radii[i - 1].x || rr.radii[i].y != rr.radii[i - 1].y) equal = false;
  }
  if (square) {
    rr.type = RRect::kRect;
  } else if (equal && rr.radii[0].x >= rr.rect.width() / 2 && rr.radii[0].y >= rr.rect.height() / 2) {
    rr.type = RRect::kOval;
  } else {
    rr.type = RRect::kRounded;
  }
}

void set_rect_xy(RRect& rr, Rect r, float rx, float ry) {
  if (!initialize_rect(rr, r)) return;
  if (!std::isfinite(rx) || !std::isfinite(ry)) rx = ry = 0;
  if (rr.rect.width() < rx + rx || rr.rect.height() < ry + ry) {
    float scale = std::min(rr.rect.width() / (rx + rx), rr.rect.height() / (ry + ry));
    rx *= scale;
    ry *= scale;
  }
  if (rx <= 0 || ry <= 0) {
    set_rect(rr, r);
    return;
  }
  rr.radii.fill(Pt{rx, ry});
  rr.type = rr.radii[0].x >= rr.rect.width() / 2 && ry >= rr.rect.height() / 2 ? RRect::kOval : RRect::kRounded;
}

double min_scale(double r1, double r2, double limit, double current) {
  return r1 + r2 > limit ? std::min(current, limit / (r1 + r2)) : current;
}

void flush_to_zero(float& a, float& b) {
  if (a + b == a) {
    b = 0;
  } else if (a + b == b) {
    a = 0;
  }
}

void adjust_radii(double limit, double scale, float* a, float* b) {
  *a = float(double(*a) * scale);
  *b = float(double(*b) * scale);
  if (*a + *b > limit) {
    float* lo = a;
    float* hi = b;
    if (*lo > *hi) std::swap(lo, hi);
    float new_hi = float(limit - *lo);
    while (new_hi + *lo > limit) new_hi = std::nextafter(new_hi, 0.0f);
    *hi = new_hi;
  }
}

// SkRRect::setRectRadii with the overlapping-curves rule of CSS (all radii
// scaled by the smallest side / sum-of-radii ratio below 1).
void set_rect_radii(RRect& rr, Rect r, std::array<Pt, 4> radii) {
  if (!initialize_rect(rr, r)) return;
  for (const Pt& p : radii) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      set_rect(rr, r);
      return;
    }
  }
  rr.radii = radii;
  if (clamp_to_zero(rr.radii)) {
    set_rect(rr, r);
    return;
  }
  auto& R = rr.radii;
  double width = double(rr.rect.r) - double(rr.rect.l), height = double(rr.rect.b) - double(rr.rect.t);
  double scale = 1.0;
  scale = min_scale(R[0].x, R[1].x, width, scale);
  scale = min_scale(R[1].y, R[2].y, height, scale);
  scale = min_scale(R[2].x, R[3].x, width, scale);
  scale = min_scale(R[3].y, R[0].y, height, scale);
  flush_to_zero(R[0].x, R[1].x);
  flush_to_zero(R[1].y, R[2].y);
  flush_to_zero(R[2].x, R[3].x);
  flush_to_zero(R[3].y, R[0].y);
  if (scale < 1.0) {
    adjust_radii(width, scale, &R[0].x, &R[1].x);
    adjust_radii(height, scale, &R[1].y, &R[2].y);
    adjust_radii(width, scale, &R[2].x, &R[3].x);
    adjust_radii(height, scale, &R[3].y, &R[0].y);
  }
  clamp_to_zero(R);
  compute_type(rr);
}

// SkPath::addRRect with the legacy start index (6 clockwise, 7 otherwise).
void add_rrect(PathData& d, const RRect& rr, bool cw) {
  size_t start = cw ? 6 : 7;
  const Rect& b = rr.rect;
  if (rr.type == RRect::kRect || rr.type == RRect::kEmpty) {
    add_rect(d, b, cw, (start + 1) / 2);
    return;
  }
  if (rr.type == RRect::kOval) {
    add_oval(d, b, cw, start / 2);
    return;
  }
  const auto& R = rr.radii;
  PointIterator<8> it({Pt{b.l + R[0].x, b.t}, Pt{b.r - R[1].x, b.t}, Pt{b.r, b.t + R[1].y}, Pt{b.r, b.b - R[2].y},
                       Pt{b.r - R[2].x, b.b}, Pt{b.l + R[3].x, b.b}, Pt{b.l, b.b - R[3].y}, Pt{b.l, b.t + R[0].y}},
                      cw, start);
  auto corners = rect_points(b, cw, start / 2 + (cw ? 0 : 1));
  bool starts_with_conic = (start & 1) == (cw ? 1u : 0u);
  move_to(d, it.now());
  if (starts_with_conic) {
    for (int i = 0; i < 3; ++i) {
      Pt c = corners.next();
      conic_to(d, c, it.next(), kRoot2Over2);
      line_to(d, it.next());
    }
    Pt c = corners.next();
    conic_to(d, c, it.next(), kRoot2Over2);
  } else {
    for (int i = 0; i < 4; ++i) {
      line_to(d, it.next());
      Pt c = corners.next();
      conic_to(d, c, it.next(), kRoot2Over2);
    }
  }
  close(d);
}

// SkPath::addPath (append mode) with a translation.
void add_path(PathData& d, const PathData& src, float dx, float dy) {
  if (src.verbs.empty()) return;
  if (d.points.size() + src.points.size() > kMaxPoints || d.verbs.size() + src.verbs.size() > kMaxPoints) {
    throw TooBig{};
  }
  int32_t base = int32_t(d.points.size());
  d.last_move = src.last_move >= 0 ? base + src.last_move : ~(base + ~src.last_move);
  d.verbs.insert(d.verbs.end(), src.verbs.begin(), src.verbs.end());
  for (const Pt& p : src.points) d.points.push_back(add(p, dx, dy));
  d.weights.insert(d.weights.end(), src.weights.begin(), src.weights.end());
  d.generation = 0;
}

void set_float(rt::Object& o, const char* name, float v) {
  rt::Field* f = find_field(o.klass, name, "F");
  if (!f || f->is_static()) return;
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  o.fields.at(f->slot) = bits;
}

// Runs fn(path data, args) under the path's lock; the path is argument 0.
template <typename Fn>
rt::NativeFn edit(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = path(rt, a.i64(0));
    try {
      std::lock_guard<std::mutex> guard(p->lock);
      fn(p->d, a);
    } catch (const TooBig&) {
      rt.throw_new("Ljava/lang/OutOfMemoryError;", "Path : trop de points");
    }
    return 0;
  };
}

// --- Path.approximate (hwui/jni/Path.cpp) ---

bool same(Pt a, Pt b) { return a.x == b.x && a.y == b.y; }

// Returns point * scale + other.
Pt scale_add(Pt point, float scale, Pt other) { return {point.x * scale + other.x, point.y * scale + other.y}; }

Pt quadratic_at(float t, const Pt* p) {
  float u = 1 - t;
  return scale_add(p[0], u * u, scale_add(p[1], 2 * u * t, Pt{p[2].x * (t * t), p[2].y * (t * t)}));
}

Pt cubic_at(float t, const Pt* p) {
  float u = 1 - t, u2 = u * u, u3 = u2 * u, t2 = t * t, t3 = t2 * t;
  return scale_add(p[0], u3, scale_add(p[1], 3 * u2 * t, scale_add(p[2], 3 * u * t2, Pt{p[3].x * t3, p[3].y * t3})));
}

using Bezier = Pt (*)(float, const Pt*);

struct Approximation {
  std::vector<Pt> points;
  std::vector<float> lengths;

  void add_move(Pt p) {
    lengths.push_back(lengths.empty() ? 0 : lengths.back());
    points.push_back(p);
  }
  void add_line(Pt to) {
    if (points.empty()) {
      points.push_back(Pt{});
      lengths.push_back(0);
    } else if (same(points.back(), to)) {
      return;  // empty line
    }
    float dx = to.x - points.back().x, dy = to.y - points.back().y;
    lengths.push_back(lengths.back() + std::sqrt(dx * dx + dy * dy));
    points.push_back(to);
  }

  // Sets the middle of [t0, t1]; whether the line p0-p1 misses it by more than
  // the error.
  static bool subdivide(const Pt* pts, Bezier f, float t0, Pt p0, float t1, Pt p1, float& mid_t, Pt& mid, float error2) {
    mid_t = (t1 + t0) / 2;
    float mx = (p1.x + p0.x) / 2, my = (p1.y + p0.y) / 2;
    mid = f(mid_t, pts);
    float ex = mid.x - mx, ey = mid.y - my;
    return ex * ex + ey * ey > error2;
  }

  // Divides the curve until linear interpolation is within the error. A cubic
  // with an inflection point can look straight at its middle: `double_check`
  // also checks the quarter, until one inflection point is found.
  void add_bezier(const Pt* pts, Bezier f, float error2, bool double_check) {
    std::map<float, Pt> at;
    at[0] = f(0, pts);
    at[1] = f(1, pts);
    auto it = at.begin();
    auto next = std::next(it);
    while (next != at.end()) {
      bool again;
      do {
        float mid_t;
        Pt mid;
        again = subdivide(pts, f, it->first, it->second, next->first, next->second, mid_t, mid, error2);
        if (!again && double_check) {
          float quarter_t;
          Pt quarter;
          again = subdivide(pts, f, it->first, it->second, mid_t, mid, quarter_t, quarter, error2);
          if (again) double_check = false;
        }
        if (again) {
          if (at.size() >= (size_t(1) << 20)) throw TooBig{};
          next = at.insert(it, {mid_t, mid});
        }
      } while (again);
      it = next;
      ++next;
    }
    for (const auto& [t, p] : at) add_line(p);
  }
};

// SkConic: a conic as 2^n quads (SkAutoConicToQuads, kMaxConicToQuadPOW2 5).
struct Conic {
  Pt p[3];
  float w;

  void chop(Conic dst[2]) const {
    float scale = 1.0f / (1.0f + w);
    float new_w = std::sqrt(0.5f + w * 0.5f);
    Pt wp1{w * p[1].x, w * p[1].y};
    Pt m{(p[0].x + wp1.x * 2 + p[2].x) * scale * 0.5f, (p[0].y + wp1.y * 2 + p[2].y) * scale * 0.5f};
    if (!std::isfinite(m.x) || !std::isfinite(m.y)) {
      double w2 = double(w) * 2, half = 1 / (1 + double(w)) * 0.5;
      m = {float((p[0].x + w2 * p[1].x + p[2].x) * half), float((p[0].y + w2 * p[1].y + p[2].y) * half)};
    }
    dst[0] = {{p[0], Pt{(p[0].x + wp1.x) * scale, (p[0].y + wp1.y) * scale}, m}, new_w};
    dst[1] = {{m, Pt{(wp1.x + p[2].x) * scale, (wp1.y + p[2].y) * scale}, p[2]}, new_w};
  }

  int quad_pow2(float tol) const {
    for (const Pt& q : p) {
      if (!std::isfinite(q.x) || !std::isfinite(q.y)) return 0;
    }
    if (tol < 0 || !std::isfinite(tol)) return 0;
    float a = w - 1, k = a / (4 * (2 + a));
    float x = k * (p[0].x - 2 * p[1].x + p[2].x), y = k * (p[0].y - 2 * p[1].y + p[2].y);
    float error = std::sqrt(x * x + y * y);
    int pow2 = 0;
    for (; pow2 < 5; ++pow2) {
      if (error <= tol) break;
      error *= 0.25f;
    }
    return pow2;
  }
};

bool between(float a, float b, float c) { return (a - b) * (c - b) <= 0; }

void subdivide_conic(const Conic& src, std::vector<Pt>& out, int level) {
  if (level == 0) {
    out.push_back(src.p[1]);
    out.push_back(src.p[2]);
    return;
  }
  Conic dst[2];
  src.chop(dst);
  float start_y = src.p[0].y, end_y = src.p[2].y;
  if (between(start_y, src.p[1].y, end_y)) {
    // Keep the chopped conics in y order when the input is monotonic.
    float mid_y = dst[0].p[2].y;
    if (!between(start_y, mid_y, end_y)) {
      float closer = std::fabs(mid_y - start_y) < std::fabs(mid_y - end_y) ? start_y : end_y;
      dst[0].p[2].y = dst[1].p[0].y = closer;
    }
    if (!between(start_y, dst[0].p[1].y, dst[0].p[2].y)) dst[0].p[1].y = start_y;
    if (!between(dst[1].p[0].y, dst[1].p[1].y, end_y)) dst[1].p[1].y = end_y;
  }
  subdivide_conic(dst[0], out, level - 1);
  subdivide_conic(dst[1], out, level - 1);
}

// SkPointPriv::EqualsWithinTolerance: the difference cannot be normalized.
bool nearly_same(Pt a, Pt b) {
  float dx = a.x - b.x, dy = a.y - b.y;
  return !(std::isfinite(dx) && std::isfinite(dy) && (dx != 0 || dy != 0));
}

// The quads' points: start, then control and end of each.
std::vector<Pt> conic_quads(const Conic& c, float tol) {
  int pow2 = c.quad_pow2(tol);
  std::vector<Pt> pts{c.p[0]};
  bool lines = false;
  if (pow2 == 5) {
    Conic dst[2];
    c.chop(dst);
    if (nearly_same(dst[0].p[1], dst[0].p[2]) && nearly_same(dst[1].p[0], dst[1].p[1])) {
      pts.insert(pts.end(), {dst[0].p[1], dst[0].p[1], dst[0].p[1], dst[1].p[2]});
      lines = true;
    }
  }
  if (!lines) subdivide_conic(c, pts, pow2);
  for (const Pt& q : pts) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y)) {
      for (size_t i = 1; i + 1 < pts.size(); ++i) pts[i] = c.p[1];
      break;
    }
  }
  return pts;
}

std::vector<float> approximate(const PathData& d, float acceptable_error) {
  Approximation out;
  float error2 = acceptable_error * acceptable_error, error_conic = acceptable_error / 2;
  // SkPath::Iter without forced close: a trailing moveTo is dropped, a close
  // first draws the line back to the contour's start when it is elsewhere.
  Pt move{}, last{};
  size_t pi = 0, wi = 0;
  for (size_t v = 0; v < d.verbs.size(); ++v) {
    switch (d.verbs[v]) {
      case kMove:
        if (v + 1 == d.verbs.size()) break;
        move = last = d.points[pi++];
        out.add_move(move);
        break;
      case kLine:
        last = d.points[pi++];
        out.add_line(last);
        break;
      case kQuad:
      case kCubic: {
        bool cubic = d.verbs[v] == kCubic;
        Pt pts[4] = {last, d.points[pi], d.points[pi + 1], cubic ? d.points[pi + 2] : Pt{}};
        pi += cubic ? 3 : 2;
        last = pts[cubic ? 3 : 2];
        if (cubic) {
          out.add_bezier(pts, cubic_at, error2, true);
        } else {
          out.add_bezier(pts, quadratic_at, error2, false);
        }
        break;
      }
      case kConic: {
        Conic c{{last, d.points[pi], d.points[pi + 1]}, d.weights[wi++]};
        pi += 2;
        last = c.p[2];
        std::vector<Pt> quads = conic_quads(c, error_conic);
        // HWUI passes the conic tolerance, not its square, as the error.
        for (size_t q = 0; q + 2 < quads.size(); q += 2) out.add_bezier(&quads[q], quadratic_at, error_conic, false);
        break;
      }
      case kClose: {
        bool nan = std::isnan(last.x) || std::isnan(last.y) || std::isnan(move.x) || std::isnan(move.y);
        if (!same(last, move) && !nan) out.add_line(move);
        out.add_line(move);
        last = move;
        break;
      }
    }
  }
  if (out.points.empty()) out.add_move(d.verbs.size() == 1 ? d.points[0] : Pt{});
  float total = out.lengths.back();
  if (total == 0) {
    // A lone moveTo still animates, at the same value.
    out.points.push_back(out.points.back());
    out.lengths.push_back(1);
    total = 1;
  }
  std::vector<float> result;
  result.reserve(out.points.size() * 3);
  for (size_t i = 0; i < out.points.size(); ++i) {
    result.push_back(out.lengths[i] / total);
    result.push_back(out.points[i].x);
    result.push_back(out.points[i].y);
  }
  return result;
}

Rect rect_args(NativeArgs& a, uint32_t first) {
  return {a.f32(first), a.f32(first + 1), a.f32(first + 2), a.f32(first + 3)};
}

}  // namespace

void register_framework_path(rt::NativeRegistry& r) {
  const char* p = "Landroid/graphics/Path;";
  static const int64_t path_free = register_free_function([](int64_t h) { paths().remove(h); });
  r.bind(p, "nGetFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(path_free); });
  r.bind(p, "nInit", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return uint64_t(paths().add(std::make_shared<Path>()));
  });
  // A copy shares the source's generation until either is edited (SkPathRef).
  r.bind(p, "nInit", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = path(rt, a.i64(0));
    auto copy = std::make_shared<Path>();
    {
      std::lock_guard<std::mutex> guard(src->lock);
      generation(src->d);
      copy->d = src->d;
    }
    return uint64_t(paths().add(std::move(copy)));
  });
  r.bind(p, "nSet", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dst = path(rt, a.i64(0));
    auto src = path(rt, a.i64(2));
    if (dst == src) return 0;
    std::scoped_lock guard(dst->lock, src->lock);
    generation(src->d);
    dst->d = src->d;
    return 0;
  });
  // Both forget the fill type too (Java's reset restores it).
  for (const char* name : {"nReset", "nRewind"}) {
    r.bind(p, name, "(J)V", edit([](PathData& d, NativeArgs&) { d = PathData{}; }));
  }
  r.bind(p, "nIncReserve", "(JI)V", edit([](PathData&, NativeArgs&) {}));
  r.bind(p, "nIsEmpty", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = path(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(pt->lock);
    return rt::ret_bool(pt->d.verbs.empty());
  });
  r.bind(p, "nGetGenerationID", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = path(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(pt->lock);
    return rt::ret_int(int32_t(generation(pt->d)));
  });
  r.bind(p, "nGetFillType", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = path(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(pt->lock);
    return rt::ret_int(pt->d.fill_type);
  });
  r.bind(p, "nSetFillType", "(JI)V", edit([](PathData& d, NativeArgs& a) { d.fill_type = a.i32(2) & 3; }));
  // Bounds of all the points, control points included; empty when a point is
  // not finite (SkRect::setBoundsCheck).
  r.bind(p, "nComputeBounds", "(JLandroid/graphics/RectF;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(2) == kNull) rt.throw_npe("bounds");
    auto pt = path(rt, a.i64(0));
    Rect b;
    {
      std::lock_guard<std::mutex> guard(pt->lock);
      const auto& pts = pt->d.points;
      bool finite = true;
      for (size_t i = 0; i < pts.size(); ++i) {
        if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y)) finite = false;
        if (i == 0) {
          b = {pts[i].x, pts[i].y, pts[i].x, pts[i].y};
        } else {
          b = {std::min(b.l, pts[i].x), std::min(b.t, pts[i].y), std::max(b.r, pts[i].x), std::max(b.b, pts[i].y)};
        }
      }
      if (!finite) b = Rect{};
    }
    rt::Object& rect = rt.nonnull(a.h(2), "bounds");
    set_float(rect, "left", b.l);
    set_float(rect, "top", b.t);
    set_float(rect, "right", b.r);
    set_float(rect, "bottom", b.b);
    return 0;
  });

  // Contours.
  r.bind(p, "nMoveTo", "(JFF)V", edit([](PathData& d, NativeArgs& a) { move_to(d, {a.f32(2), a.f32(3)}); }));
  r.bind(p, "nRMoveTo", "(JFF)V", edit([](PathData& d, NativeArgs& a) {
    Pt from;
    if (!d.points.empty()) from = d.last_move >= 0 ? d.points.back() : d.points[size_t(~d.last_move)];
    move_to(d, add(from, a.f32(2), a.f32(3)));
  }));
  r.bind(p, "nLineTo", "(JFF)V", edit([](PathData& d, NativeArgs& a) { line_to(d, {a.f32(2), a.f32(3)}); }));
  r.bind(p, "nRLineTo", "(JFF)V", edit([](PathData& d, NativeArgs& a) {
    inject_move(d);
    line_to(d, add(last_point(d), a.f32(2), a.f32(3)));
  }));
  r.bind(p, "nQuadTo", "(JFFFF)V", edit([](PathData& d, NativeArgs& a) {
    quad_to(d, {a.f32(2), a.f32(3)}, {a.f32(4), a.f32(5)});
  }));
  r.bind(p, "nRQuadTo", "(JFFFF)V", edit([](PathData& d, NativeArgs& a) {
    inject_move(d);
    Pt o = last_point(d);
    quad_to(d, add(o, a.f32(2), a.f32(3)), add(o, a.f32(4), a.f32(5)));
  }));
  r.bind(p, "nConicTo", "(JFFFFF)V", edit([](PathData& d, NativeArgs& a) {
    conic_to(d, {a.f32(2), a.f32(3)}, {a.f32(4), a.f32(5)}, a.f32(6));
  }));
  r.bind(p, "nRConicTo", "(JFFFFF)V", edit([](PathData& d, NativeArgs& a) {
    inject_move(d);
    Pt o = last_point(d);
    conic_to(d, add(o, a.f32(2), a.f32(3)), add(o, a.f32(4), a.f32(5)), a.f32(6));
  }));
  r.bind(p, "nCubicTo", "(JFFFFFF)V", edit([](PathData& d, NativeArgs& a) {
    cubic_to(d, {a.f32(2), a.f32(3)}, {a.f32(4), a.f32(5)}, {a.f32(6), a.f32(7)});
  }));
  r.bind(p, "nRCubicTo", "(JFFFFFF)V", edit([](PathData& d, NativeArgs& a) {
    inject_move(d);
    Pt o = last_point(d);
    cubic_to(d, add(o, a.f32(2), a.f32(3)), add(o, a.f32(4), a.f32(5)), add(o, a.f32(6), a.f32(7)));
  }));
  r.bind(p, "nClose", "(J)V", edit([](PathData& d, NativeArgs&) { close(d); }));
  r.bind(p, "nSetLastPoint", "(JFF)V", edit([](PathData& d, NativeArgs& a) {
    if (d.points.empty()) {
      move_to(d, {a.f32(2), a.f32(3)});
    } else {
      d.points.back() = {a.f32(2), a.f32(3)};
      d.generation = 0;
    }
  }));
  r.bind(p, "nOffset", "(JFF)V", edit([](PathData& d, NativeArgs& a) {
    for (Pt& pt : d.points) pt = add(pt, a.f32(2), a.f32(3));
    d.generation = 0;
  }));

  // Shapes (Path.Direction: CW 0, CCW 1).
  r.bind(p, "nAddRect", "(JFFFFI)V", edit([](PathData& d, NativeArgs& a) {
    add_rect(d, rect_args(a, 2), a.i32(6) == 0, 0);
  }));
  r.bind(p, "nAddOval", "(JFFFFI)V", edit([](PathData& d, NativeArgs& a) {
    add_oval(d, rect_args(a, 2), a.i32(6) == 0, 1);
  }));
  r.bind(p, "nAddCircle", "(JFFFI)V", edit([](PathData& d, NativeArgs& a) {
    float x = a.f32(2), y = a.f32(3), radius = a.f32(4);
    if (radius > 0) add_oval(d, {x - radius, y - radius, x + radius, y + radius}, a.i32(5) == 0, 1);
  }));
  r.bind(p, "nAddRoundRect", "(JFFFFFFI)V", edit([](PathData& d, NativeArgs& a) {
    float rx = a.f32(6), ry = a.f32(7);
    if (rx < 0 || ry < 0) return;
    RRect rr;
    set_rect_xy(rr, rect_args(a, 2), rx, ry);
    add_rrect(d, rr, a.i32(8) == 0);
  }));
  r.bind(p, "nAddRoundRect", "(JFFFF[FI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(6) == kNull) rt.throw_npe("radii");
    rt::ArrayObject& arr = rt.array(a.h(6), "float[]");
    if (arr.length < 8) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "8 rayons attendus");
    std::array<float, 8> v;
    std::memcpy(v.data(), arr.data.data(), sizeof(v));
    std::array<Pt, 4> radii{Pt{v[0], v[1]}, Pt{v[2], v[3]}, Pt{v[4], v[5]}, Pt{v[6], v[7]}};
    return edit([radii](PathData& d, NativeArgs& args) {
      RRect rr;
      set_rect_radii(rr, rect_args(args, 2), radii);
      add_rrect(d, rr, args.i32(7) == 0);
    })(rt, a);
  });
  // Points along the path with the fraction of its length at each:
  // [fraction, x, y] triplets.
  r.bind(p, "nApproximate", "(JF)[F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = path(rt, a.i64(0));
    std::vector<float> values;
    try {
      std::lock_guard<std::mutex> guard(pt->lock);
      values = approximate(pt->d, a.f32(2));
    } catch (const TooBig&) {
      rt.throw_new("Ljava/lang/OutOfMemoryError;", "Path.approximate : trop de points");
    }
    if (values.size() > size_t(INT32_MAX)) rt.throw_new("Ljava/lang/OutOfMemoryError;", "Path.approximate");
    Handle array = rt.new_array("[F", int32_t(values.size()));
    std::memcpy(rt.array(array, "float[]").data.data(), values.data(), values.size() * sizeof(float));
    return rt::ret_obj(array);
  });
  for (bool translated : {false, true}) {
    r.bind(p, "nAddPath", translated ? "(JJFF)V" : "(JJ)V", [translated](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto dst = path(rt, a.i64(0));
      auto src = path(rt, a.i64(2));
      float dx = translated ? a.f32(4) : 0, dy = translated ? a.f32(5) : 0;
      try {
        if (dst == src) {  // adding a path to itself: from a copy
          std::lock_guard<std::mutex> guard(dst->lock);
          PathData copy = dst->d;
          add_path(dst->d, copy, dx, dy);
        } else {
          std::scoped_lock guard(dst->lock, src->lock);
          add_path(dst->d, src->d, dx, dy);
        }
      } catch (const TooBig&) {
        rt.throw_new("Ljava/lang/OutOfMemoryError;", "Path : trop de points");
      }
      return 0;
    });
  }
}

// For the unit tests: the verbs, points and conic weights of a path.
void path_contents(Runtime& rt, int64_t handle, std::vector<uint8_t>& verbs, std::vector<float>& coords,
                   std::vector<float>& weights) {
  auto p = path(rt, handle);
  std::lock_guard<std::mutex> guard(p->lock);
  verbs = p->d.verbs;
  coords.clear();
  for (const Pt& pt : p->d.points) {
    coords.push_back(pt.x);
    coords.push_back(pt.y);
  }
  weights = p->d.weights;
}

}  // namespace aa::bridge
