// android.graphics.drawable.VectorDrawable natives (G3, ADR-006 J4), each
// signature checked against the framework.jar of Android 17, and
// com.android.internal.util.VirtualRefBasePtr, which owns the tree. HWUI's
// VectorDrawable tree as a property store: groups (rotation, pivot, scale,
// translation, children), full paths (stroke and fill, trimming, caps and
// joins, path data parsed as PathParser does) and clip paths, with HWUI's
// defaults and its property layouts (getFullPathProperties: the 48 bytes of
// FullPathProperties::PrimitiveFields; getGroupProperties: 7 floats).
// Drawing (nDraw) waits for the software renderer (J7).
#include "framework-bridge/internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "common/text.h"
#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

enum class Kind { kGroup, kFullPath, kClipPath, kTree };

// FullPathProperties::PrimitiveFields, in its order and with its defaults.
struct PathFields {
  float stroke_width = 0;
  uint32_t stroke_color = 0;  // SK_ColorTRANSPARENT
  float stroke_alpha = 1;
  uint32_t fill_color = 0;
  float fill_alpha = 1;
  float trim_start = 0;
  float trim_end = 1;
  float trim_offset = 0;
  int32_t line_cap = 0;   // kButt_Cap
  int32_t line_join = 0;  // kMiter_Join
  float miter_limit = 4;
  int32_t fill_type = 0;  // winding
};
static_assert(sizeof(PathFields) == 48, "FullPathProperties::PrimitiveFields");

// GroupProperties::PrimitiveFields: rotate, pivotX, pivotY, scaleX, scaleY, translateX, translateY.
enum { kRotate, kPivotX, kPivotY, kScaleX, kScaleY, kTranslateX, kTranslateY, kGroupCount };

struct Node {
  Kind kind;
  int64_t handle = 0;
  std::string name;
  // Groups.
  std::array<float, kGroupCount> group{0, 0, 0, 1, 1, 0, 0};
  std::vector<std::shared_ptr<Node>> children;
  // Paths.
  SvgPathData data;
  PathFields fields;
  int64_t fill_gradient = 0, stroke_gradient = 0;
  // Trees.
  std::shared_ptr<Node> root;
  float viewport_width = 0, viewport_height = 0, root_alpha = 1;
  bool allow_caching = true, anti_alias = true;

  explicit Node(Kind k) : kind(k) {}
};

std::mutex& vd_lock() {
  static auto* m = new std::mutex;
  return *m;
}

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Node>& nodes() {
  static auto* t = new HandleTable<Node>(0x1112);  // never destroyed
  return *t;
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::kGroup: return "VGroup";
    case Kind::kFullPath: return "VFullPath";
    case Kind::kClipPath: return "VClipPath";
    default: return "VectorDrawable";
  }
}

std::shared_ptr<Node> node(Runtime& rt, int64_t h, std::initializer_list<Kind> kinds) {
  auto n = nodes().get(h);
  bool ok = false;
  if (n) {
    for (Kind k : kinds) ok = ok || n->kind == k;
  }
  if (!ok) {
    rt.throw_new("Ljava/lang/IllegalStateException;",
                 std::string("[AnyAndroid] ") + kind_name(*kinds.begin()) + " invalide");
  }
  return n;
}

int64_t add(std::shared_ptr<Node> n) {
  int64_t h = nodes().add(n);
  n->handle = h;
  return h;
}

// Deleting a tree deletes its root group and, through it, every descendant.
void forget(const std::shared_ptr<Node>& n) {
  if (!n) return;
  for (const auto& child : n->children) forget(child);
  forget(n->root);
  nodes().remove(n->handle);
}

// VirtualRefBasePtr: HWUI's VirtualLightRefBase counts, starting at 0.
std::map<int64_t, int32_t>& strong_counts() {
  static auto* m = new std::map<int64_t, int32_t>;
  return *m;
}

std::string string_arg(Runtime& rt, Handle h) {
  if (h == kNull) rt.throw_npe("VectorDrawable : chaîne null");
  return text::utf16_to_utf8(rt.string_value(h));
}

// Binds a float getter and setter of a group property.
void group_property(rt::NativeRegistry& r, const char* vd, const char* get, const char* set, int index) {
  r.bind(vd, get, "(J)F", [index](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    return rt::ret_float(node(rt, a.i64(0), {Kind::kGroup})->group[size_t(index)]);
  });
  r.bind(vd, set, "(JF)V", [index](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kGroup})->group[size_t(index)] = a.f32(2);
    return 0;
  });
}

// The same for a full path property (float or color).
template <typename T>
void path_property(rt::NativeRegistry& r, const char* vd, const char* get, const char* set, T PathFields::*field) {
  constexpr bool is_float = std::is_same_v<T, float>;
  r.bind(vd, get, is_float ? "(J)F" : "(J)I", [field](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    T v = node(rt, a.i64(0), {Kind::kFullPath})->fields.*field;
    if constexpr (is_float) {
      return rt::ret_float(v);
    } else {
      return rt::ret_int(int32_t(v));
    }
  });
  r.bind(vd, set, is_float ? "(JF)V" : "(JI)V", [field](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    if constexpr (is_float) {
      node(rt, a.i64(0), {Kind::kFullPath})->fields.*field = a.f32(2);
    } else {
      node(rt, a.i64(0), {Kind::kFullPath})->fields.*field = T(a.i32(2));
    }
    return 0;
  });
}

}  // namespace

void register_framework_vector_drawable(rt::NativeRegistry& r) {
  const char* ref = "Lcom/android/internal/util/VirtualRefBasePtr;";
  r.bind(ref, "nIncStrong", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kTree});  // the only VirtualLightRefBase handed out so far
    ++strong_counts()[a.i64(0)];
    return 0;
  });
  r.bind(ref, "nDecStrong", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto tree = node(rt, a.i64(0), {Kind::kTree});
    auto it = strong_counts().find(a.i64(0));
    if (it == strong_counts().end() || --it->second <= 0) {
      if (it != strong_counts().end()) strong_counts().erase(it);
      forget(tree);
    }
    return 0;
  });

  const char* vd = "Landroid/graphics/drawable/VectorDrawable;";
  // Nodes and trees. Copies take the properties, path data and name, not the
  // children (Java adds them again).
  r.bind(vd, "nCreateGroup", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    return rt::ret_long(add(std::make_shared<Node>(Kind::kGroup)));
  });
  r.bind(vd, "nCreateFullPath", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    return rt::ret_long(add(std::make_shared<Node>(Kind::kFullPath)));
  });
  r.bind(vd, "nCreateClipPath", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    return rt::ret_long(add(std::make_shared<Node>(Kind::kClipPath)));
  });
  const std::pair<const char*, Kind> copies[] = {
      {"nCreateGroup", Kind::kGroup}, {"nCreateFullPath", Kind::kFullPath}, {"nCreateClipPath", Kind::kClipPath}};
  for (const auto& [name, kind] : copies) {
    r.bind(vd, name, "(J)J", [kind = kind](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::lock_guard<std::mutex> guard(vd_lock());
      auto src = node(rt, a.i64(0), {kind});
      auto copy = std::make_shared<Node>(kind);
      copy->name = src->name;
      copy->group = src->group;
      copy->data = src->data;
      copy->fields = src->fields;
      copy->fill_gradient = src->fill_gradient;
      copy->stroke_gradient = src->stroke_gradient;
      return rt::ret_long(add(std::move(copy)));
    });
  }
  r.bind(vd, "nCreateTree", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto tree = std::make_shared<Node>(Kind::kTree);
    tree->root = node(rt, a.i64(0), {Kind::kGroup});
    return rt::ret_long(add(std::move(tree)));
  });
  r.bind(vd, "nCreateTreeFromCopy", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto src = node(rt, a.i64(0), {Kind::kTree});
    auto tree = std::make_shared<Node>(Kind::kTree);
    tree->root = node(rt, a.i64(2), {Kind::kGroup});
    tree->viewport_width = src->viewport_width;
    tree->viewport_height = src->viewport_height;
    tree->root_alpha = src->root_alpha;
    return rt::ret_long(add(std::move(tree)));
  });
  r.bind(vd, "nSetName", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string name = string_arg(rt, a.h(2));
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kGroup, Kind::kFullPath, Kind::kClipPath})->name = std::move(name);
    return 0;
  });
  r.bind(vd, "nAddChild", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto group = node(rt, a.i64(0), {Kind::kGroup});
    auto child = node(rt, a.i64(2), {Kind::kGroup, Kind::kFullPath, Kind::kClipPath});
    group->children.push_back(std::move(child));
    return 0;
  });

  // Trees.
  r.bind(vd, "nSetAllowCaching", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kTree})->allow_caching = a.i32(2) != 0;
    return 0;
  });
  r.bind(vd, "nSetAntiAlias", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kTree})->anti_alias = a.i32(2) != 0;
    return 0;
  });
  r.bind(vd, "nSetRendererViewportSize", "(JFF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto tree = node(rt, a.i64(0), {Kind::kTree});
    tree->viewport_width = a.f32(2);
    tree->viewport_height = a.f32(3);
    return 0;
  });
  r.bind(vd, "nSetRootAlpha", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto tree = node(rt, a.i64(0), {Kind::kTree});
    float alpha = a.f32(2);
    if (alpha == tree->root_alpha) return rt::ret_bool(false);
    tree->root_alpha = alpha;
    return rt::ret_bool(true);
  });
  r.bind(vd, "nGetRootAlpha", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    return rt::ret_float(node(rt, a.i64(0), {Kind::kTree})->root_alpha);
  });

  // Groups.
  r.bind(vd, "nUpdateGroupProperties", "(JFFFFFFF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    auto group = node(rt, a.i64(0), {Kind::kGroup});
    for (int i = 0; i < kGroupCount; ++i) group->group[size_t(i)] = a.f32(uint32_t(2 + i));
    return 0;
  });
  r.bind(vd, "nGetGroupProperties", "(J[FI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::array<float, kGroupCount> values;
    {
      std::lock_guard<std::mutex> guard(vd_lock());
      values = node(rt, a.i64(0), {Kind::kGroup})->group;
    }
    if (a.i32(3) != kGroupCount) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] propriétés de groupe : 7 attendues");
    }
    rt::ArrayObject& out = rt.array(a.h(2), "float[]");
    if (out.length < kGroupCount) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;");
    std::memcpy(out.data.data(), values.data(), sizeof values);
    return rt::ret_bool(true);
  });
  group_property(r, vd, "nGetRotation", "nSetRotation", kRotate);
  group_property(r, vd, "nGetPivotX", "nSetPivotX", kPivotX);
  group_property(r, vd, "nGetPivotY", "nSetPivotY", kPivotY);
  group_property(r, vd, "nGetScaleX", "nSetScaleX", kScaleX);
  group_property(r, vd, "nGetScaleY", "nSetScaleY", kScaleY);
  group_property(r, vd, "nGetTranslateX", "nSetTranslateX", kTranslateX);
  group_property(r, vd, "nGetTranslateY", "nSetTranslateY", kTranslateY);

  // Paths.
  r.bind(vd, "nSetPathString", "(JLjava/lang/String;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::string s = string_arg(rt, a.h(2));
    SvgPathData data;
    std::string error;
    bool ok = parse_svg_path(s, size_t(std::max(a.i32(3), 0)), data, error);
    if (!ok) rt.throw_new("Ljava/lang/IllegalArgumentException;", error);
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kFullPath, Kind::kClipPath})->data = std::move(data);
    return 0;
  });
  r.bind(vd, "nSetPathData", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SvgPathData data = svg_path_data(rt, a.i64(2));
    std::lock_guard<std::mutex> guard(vd_lock());
    node(rt, a.i64(0), {Kind::kFullPath, Kind::kClipPath})->data = std::move(data);
    return 0;
  });
  r.bind(vd, "nUpdateFullPathProperties", "(JFIFIFFFFFIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::lock_guard<std::mutex> guard(vd_lock());
    PathFields& f = node(rt, a.i64(0), {Kind::kFullPath})->fields;
    f.stroke_width = a.f32(2);
    f.stroke_color = uint32_t(a.i32(3));
    f.stroke_alpha = a.f32(4);
    f.fill_color = uint32_t(a.i32(5));
    f.fill_alpha = a.f32(6);
    f.trim_start = a.f32(7);
    f.trim_end = a.f32(8);
    f.trim_offset = a.f32(9);
    f.miter_limit = a.f32(10);
    f.line_cap = a.i32(11);
    f.line_join = a.i32(12);
    f.fill_type = a.i32(13);
    return 0;
  });
  r.bind(vd, "nGetFullPathProperties", "(J[BI)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PathFields fields;
    {
      std::lock_guard<std::mutex> guard(vd_lock());
      fields = node(rt, a.i64(0), {Kind::kFullPath})->fields;
    }
    if (a.i32(3) != int32_t(sizeof fields)) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "[AnyAndroid] propriétés de chemin : 48 octets attendus");
    }
    rt::ArrayObject& out = rt.array(a.h(2), "byte[]");
    if (out.length < int32_t(sizeof fields)) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;");
    std::memcpy(out.data.data(), &fields, sizeof fields);
    return rt::ret_bool(true);
  });
  for (const char* name : {"nUpdateFullPathFillGradient", "nUpdateFullPathStrokeGradient"}) {
    bool fill = std::strcmp(name, "nUpdateFullPathFillGradient") == 0;
    r.bind(vd, name, "(JJ)V", [fill](Runtime& rt, NativeArgs& a) -> uint64_t {
      std::lock_guard<std::mutex> guard(vd_lock());
      auto path = node(rt, a.i64(0), {Kind::kFullPath});
      (fill ? path->fill_gradient : path->stroke_gradient) = a.i64(2);
      return 0;
    }, S::kPartial, "dégradé retenu, appliqué au rendu (J7)");
  }
  path_property(r, vd, "nGetStrokeWidth", "nSetStrokeWidth", &PathFields::stroke_width);
  path_property(r, vd, "nGetStrokeColor", "nSetStrokeColor", &PathFields::stroke_color);
  path_property(r, vd, "nGetStrokeAlpha", "nSetStrokeAlpha", &PathFields::stroke_alpha);
  path_property(r, vd, "nGetFillColor", "nSetFillColor", &PathFields::fill_color);
  path_property(r, vd, "nGetFillAlpha", "nSetFillAlpha", &PathFields::fill_alpha);
  path_property(r, vd, "nGetTrimPathStart", "nSetTrimPathStart", &PathFields::trim_start);
  path_property(r, vd, "nGetTrimPathEnd", "nSetTrimPathEnd", &PathFields::trim_end);
  path_property(r, vd, "nGetTrimPathOffset", "nSetTrimPathOffset", &PathFields::trim_offset);
}

}  // namespace aa::bridge
