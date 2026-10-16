// android.graphics.Region and RegionIterator natives (G3, ADR-006 J4), each
// signature checked against the framework.jar of Android 17. A region is
// SkRegion's model: a set of pixels kept as horizontal bands of x spans in
// canonical form (bands sorted and disjoint, touching bands with the same
// spans merged, spans sorted, disjoint and not touching), so equal sets
// compare equal and iterate in SkRegion's order, band by band and left to
// right. Boolean operations sweep the band edges of both operands. setPath
// (scan conversion) and getBoundaryPath wait for the rasterizer of the
// software renderer (J7); parcelling waits for a caller.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

struct Span {
  int32_t l, r;
  bool operator==(const Span&) const = default;
};

struct Band {
  int32_t t, b;
  std::vector<Span> spans;
  bool operator==(const Band&) const = default;
};

using Bands = std::vector<Band>;

struct IRect {
  int32_t l = 0, t = 0, r = 0, b = 0;
  bool empty() const { return !(l < r && t < b); }
};

// SkRegion::Op: DIFFERENCE 0, INTERSECT 1, UNION 2, XOR 3, REVERSE_DIFFERENCE 4, REPLACE 5.
bool keep(int32_t op, bool in_a, bool in_b) {
  switch (op) {
    case 0: return in_a && !in_b;
    case 1: return in_a && in_b;
    case 2: return in_a || in_b;
    case 3: return in_a != in_b;
    case 4: return in_b && !in_a;
    default: return in_b;
  }
}

Bands of_rect(const IRect& r) {
  if (r.empty()) return {};
  return {Band{r.t, r.b, {Span{r.l, r.r}}}};
}

const std::vector<Span>& spans_at(const Bands& bands, int32_t y) {
  static const std::vector<Span> none;
  auto it = std::upper_bound(bands.begin(), bands.end(), y, [](int32_t v, const Band& band) { return v < band.b; });
  return it != bands.end() && it->t <= y ? it->spans : none;
}

// The spans of `a op b` on one row.
std::vector<Span> combine(const std::vector<Span>& a, const std::vector<Span>& b, int32_t op) {
  std::vector<int32_t> xs;
  for (const auto* spans : {&a, &b}) {
    for (const Span& s : *spans) {
      xs.push_back(s.l);
      xs.push_back(s.r);
    }
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::vector<Span> out;
  size_t ia = 0, ib = 0;
  for (size_t i = 0; i + 1 < xs.size(); ++i) {
    int32_t x = xs[i];
    while (ia < a.size() && a[ia].r <= x) ++ia;
    while (ib < b.size() && b[ib].r <= x) ++ib;
    bool in_a = ia < a.size() && a[ia].l <= x, in_b = ib < b.size() && b[ib].l <= x;
    if (!keep(op, in_a, in_b)) continue;
    if (!out.empty() && out.back().r == x) {
      out.back().r = xs[i + 1];
    } else {
      out.push_back({x, xs[i + 1]});
    }
  }
  return out;
}

// SkRegion::op(a, b, op), in canonical form.
Bands combine(const Bands& a, const Bands& b, int32_t op) {
  std::vector<int32_t> ys;
  for (const auto* bands : {&a, &b}) {
    for (const Band& band : *bands) {
      ys.push_back(band.t);
      ys.push_back(band.b);
    }
  }
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  Bands out;
  for (size_t i = 0; i + 1 < ys.size(); ++i) {
    std::vector<Span> spans = combine(spans_at(a, ys[i]), spans_at(b, ys[i]), op);
    if (spans.empty()) continue;
    if (!out.empty() && out.back().b == ys[i] && out.back().spans == spans) {
      out.back().b = ys[i + 1];
    } else {
      out.push_back({ys[i], ys[i + 1], std::move(spans)});
    }
  }
  return out;
}

IRect bounds(const Bands& bands) {
  if (bands.empty()) return {};
  IRect r{INT32_MAX, bands.front().t, INT32_MIN, bands.back().b};
  for (const Band& band : bands) {
    r.l = std::min(r.l, band.spans.front().l);
    r.r = std::max(r.r, band.spans.back().r);
  }
  return r;
}

bool is_rect(const Bands& bands) { return bands.size() == 1 && bands[0].spans.size() == 1; }

std::vector<IRect> rects(const Bands& bands) {
  std::vector<IRect> out;
  for (const Band& band : bands) {
    for (const Span& s : band.spans) out.push_back({s.l, band.t, s.r, band.b});
  }
  return out;
}

int32_t sat_add(int32_t a, int32_t d) {
  return int32_t(std::clamp<int64_t>(int64_t(a) + d, INT32_MIN, INT32_MAX));
}

struct Region {
  std::mutex lock;
  Bands bands;
};

struct RegionIter {
  std::mutex lock;
  std::vector<IRect> rects;  // of a copy of the region, as HWUI's RgnIterPair
  size_t next = 0;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Region>& regions() {
  static auto* t = new HandleTable<Region>(0x1110);  // never destroyed
  return *t;
}

HandleTable<RegionIter>& region_iters() {
  static auto* t = new HandleTable<RegionIter>(0x1111);  // never destroyed
  return *t;
}

std::shared_ptr<Region> region(Runtime& rt, int64_t h) {
  auto p = regions().get(h);
  if (!p) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Region invalide");
  return p;
}

Bands copy_of(Runtime& rt, int64_t h) {
  auto p = region(rt, h);
  std::lock_guard<std::mutex> guard(p->lock);
  return p->bands;
}

void store(Runtime& rt, int64_t h, Bands bands) {
  auto p = region(rt, h);
  std::lock_guard<std::mutex> guard(p->lock);
  p->bands = std::move(bands);
}

// Region.mNativeRegion of a Java Region.
int64_t native_of(Runtime& rt, Handle obj) {
  rt::Object& o = rt.nonnull(obj, "Region");
  rt::Field* f = find_field(o.klass, "mNativeRegion", "J");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Region sans mNativeRegion");
  return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
}

int32_t int_field(rt::Object& o, const char* name) {
  rt::Field* f = find_field(o.klass, name, "I");
  return f && !f->is_static() ? int32_t(o.fields.at(f->slot)) : 0;
}

IRect read_rect(Runtime& rt, Handle h) {
  rt::Object& o = rt.nonnull(h, "Rect");
  return {int_field(o, "left"), int_field(o, "top"), int_field(o, "right"), int_field(o, "bottom")};
}

void write_rect(Runtime& rt, Handle h, const IRect& r) {
  rt::Object& o = rt.nonnull(h, "Rect");
  const std::pair<const char*, int32_t> values[] = {{"left", r.l}, {"top", r.t}, {"right", r.r}, {"bottom", r.b}};
  for (const auto& [name, v] : values) {
    rt::Field* f = find_field(o.klass, name, "I");
    if (f && !f->is_static()) o.fields.at(f->slot) = uint32_t(v);
  }
}

void check_op(Runtime& rt, int32_t op) {
  if (op < 0 || op > 5) rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] Region.Op invalide");
}

uint64_t op_result(Runtime& rt, int64_t dst, Bands result) {
  bool nonempty = !result.empty();
  store(rt, dst, std::move(result));
  return rt::ret_bool(nonempty);
}

// HWUI's scale_rgn: the union of the rectangles scaled and rounded (roundf).
Bands scaled(const Bands& src, float scale) {
  Bands out;
  for (const IRect& r : rects(src)) {
    IRect s{int32_t(std::round(float(r.l) * scale)), int32_t(std::round(float(r.t) * scale)),
            int32_t(std::round(float(r.r) * scale)), int32_t(std::round(float(r.b) * scale))};
    out = combine(out, of_rect(s), 2);
  }
  return out;
}

Bands translated(const Bands& src, int32_t dx, int32_t dy) {
  Bands out = src;
  for (Band& band : out) {
    band.t = sat_add(band.t, dy);
    band.b = sat_add(band.b, dy);
    for (Span& s : band.spans) {
      s.l = sat_add(s.l, dx);
      s.r = sat_add(s.r, dx);
    }
  }
  return out;
}

}  // namespace

void register_framework_region(rt::NativeRegistry& r) {
  const char* rg = "Landroid/graphics/Region;";
  r.bind(rg, "nativeConstructor", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(regions().add(std::make_shared<Region>()));
  });
  r.bind(rg, "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    regions().remove(a.i64(0));
    return 0;
  });
  r.bind(rg, "nativeSetRegion", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    store(rt, a.i64(0), copy_of(rt, a.i64(2)));
    return 0;
  });
  r.bind(rg, "nativeSetRect", "(JIIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return op_result(rt, a.i64(0), of_rect({a.i32(2), a.i32(3), a.i32(4), a.i32(5)}));
  });
  r.bind(rg, "nativeGetBounds", "(JLandroid/graphics/Rect;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands bands = copy_of(rt, a.i64(0));
    write_rect(rt, a.h(2), bounds(bands));
    return rt::ret_bool(!bands.empty());
  });
  r.bind(rg, "nativeOp", "(JIIIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t op = a.i32(6);
    check_op(rt, op);
    return op_result(rt, a.i64(0), combine(copy_of(rt, a.i64(0)), of_rect({a.i32(2), a.i32(3), a.i32(4), a.i32(5)}), op));
  });
  r.bind(rg, "nativeOp", "(JLandroid/graphics/Rect;JI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t op = a.i32(5);
    check_op(rt, op);
    return op_result(rt, a.i64(0), combine(of_rect(read_rect(rt, a.h(2))), copy_of(rt, a.i64(3)), op));
  });
  r.bind(rg, "nativeOp", "(JJJI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t op = a.i32(6);
    check_op(rt, op);
    return op_result(rt, a.i64(0), combine(copy_of(rt, a.i64(2)), copy_of(rt, a.i64(4)), op));
  });
  r.bind(rg, "nativeEquals", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(copy_of(rt, a.i64(0)) == copy_of(rt, a.i64(2)));
  });
  // SkRegion::toString.
  r.bind(rg, "nativeToString", "(J)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string s = "SkRegion(";
    for (const IRect& rc : rects(copy_of(rt, a.i64(0)))) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "(%d,%d,%d,%d)", int(rc.l), int(rc.t), int(rc.r), int(rc.b));
      s += buf;
    }
    s += ")";
    return rt::ret_obj(rt.new_string_utf8(s));
  });

  // Instance methods: argument 0 is the Java Region.
  r.bind(rg, "isEmpty", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(copy_of(rt, native_of(rt, a.h(0))).empty());
  });
  r.bind(rg, "isRect", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_bool(is_rect(copy_of(rt, native_of(rt, a.h(0)))));
  });
  r.bind(rg, "isComplex", "()Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands bands = copy_of(rt, native_of(rt, a.h(0)));
    return rt::ret_bool(!bands.empty() && !is_rect(bands));
  });
  r.bind(rg, "contains", "(II)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands bands = copy_of(rt, native_of(rt, a.h(0)));
    int32_t x = a.i32(1);
    for (const Span& s : spans_at(bands, a.i32(2))) {
      if (s.l <= x && x < s.r) return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });
  r.bind(rg, "quickContains", "(IIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands bands = copy_of(rt, native_of(rt, a.h(0)));
    IRect q{a.i32(1), a.i32(2), a.i32(3), a.i32(4)};
    IRect b = bounds(bands);
    return rt::ret_bool(!q.empty() && is_rect(bands) && b.l <= q.l && b.t <= q.t && b.r >= q.r && b.b >= q.b);
  });
  auto disjoint = [](const IRect& x, const IRect& y) {
    return !(std::max(x.l, y.l) < std::min(x.r, y.r) && std::max(x.t, y.t) < std::min(x.b, y.b));
  };
  r.bind(rg, "quickReject", "(IIII)Z", [disjoint](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands bands = copy_of(rt, native_of(rt, a.h(0)));
    IRect q{a.i32(1), a.i32(2), a.i32(3), a.i32(4)};
    return rt::ret_bool(bands.empty() || q.empty() || disjoint(bounds(bands), q));
  });
  r.bind(rg, "quickReject", "(Landroid/graphics/Region;)Z", [disjoint](Runtime& rt, NativeArgs& a) -> uint64_t {
    Bands mine = copy_of(rt, native_of(rt, a.h(0)));
    Bands other = copy_of(rt, native_of(rt, a.h(1)));
    return rt::ret_bool(mine.empty() || other.empty() || disjoint(bounds(mine), bounds(other)));
  });
  r.bind(rg, "scale", "(FLandroid/graphics/Region;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t self = native_of(rt, a.h(0));
    int64_t dst = a.h(2) == kNull ? self : native_of(rt, a.h(2));
    store(rt, dst, scaled(copy_of(rt, self), a.f32(1)));
    return 0;
  });
  r.bind(rg, "translate", "(IILandroid/graphics/Region;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t self = native_of(rt, a.h(0));
    int64_t dst = a.h(3) == kNull ? self : native_of(rt, a.h(3));
    store(rt, dst, translated(copy_of(rt, self), a.i32(1), a.i32(2)));
    return 0;
  });

  const char* it = "Landroid/graphics/RegionIterator;";
  r.bind(it, "nativeConstructor", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto iter = std::make_shared<RegionIter>();
    iter->rects = rects(copy_of(rt, a.i64(0)));
    return rt::ret_long(region_iters().add(std::move(iter)));
  });
  r.bind(it, "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    region_iters().remove(a.i64(0));
    return 0;
  });
  r.bind(it, "nativeNext", "(JLandroid/graphics/Rect;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto iter = region_iters().get(a.i64(0));
    if (!iter) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RegionIterator invalide");
    IRect next;
    {
      std::lock_guard<std::mutex> guard(iter->lock);
      if (iter->next >= iter->rects.size()) return rt::ret_bool(false);
      next = iter->rects[iter->next++];
    }
    write_rect(rt, a.h(2), next);
    return rt::ret_bool(true);
  });
}

}  // namespace aa::bridge
