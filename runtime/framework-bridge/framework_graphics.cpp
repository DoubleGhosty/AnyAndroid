// Graphics natives of framework.jar (G3, ADR-006 J4), written from the natives
// the real start-up path asks for, each signature checked against the
// framework.jar of Android 17. Until the software renderer (J7) they are
// faithful property stores: what Java sets, Java reads back.
//
// Native objects live in bounded handle tables (handles.h), never behind
// pointers. Their free functions are registered for NativeAllocationRegistry;
// AnyAndroid does not run Cleaners yet, so objects stay until the process
// ends (small, fixed-size records).
#include "framework-bridge/internal.h"

#include <array>
#include <atomic>
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
using S = rt::ApiStatus;

// android.graphics.ColorSpace.Rgb: transfer parameters (a, b, c, d, e, f, g)
// and the RGB -> XYZ (D50) matrix, as skcms keeps them.
struct ColorSpaceRgb {
  std::array<float, 7> transfer{};
  std::array<float, 9> to_xyz{};
};

// android.graphics.RenderNode: HWUI's RenderProperties, with its defaults.
// Setters report whether the value changed (HWUI's RP_SET), which View uses to
// decide on invalidation.
struct RenderNodeState {
  std::mutex lock;
  int64_t unique_id = 0;
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  float alpha = 1, elevation = 0, translation_x = 0, translation_y = 0, translation_z = 0;
  float rotation = 0, rotation_x = 0, rotation_y = 0, scale_x = 1, scale_y = 1;
  float pivot_x = 0, pivot_y = 0, camera_distance = -8;  // Sk3DView's default (8 inches)
  bool pivot_explicit = false, clip_to_bounds = true, clip_to_outline = false, overlapping = true;
  bool allow_force_dark = true, project_backwards = false, projection_receiver = false;
  bool has_clip_bounds = false, has_outline = false, outline_empty = false;
  int32_t layer_type = 0, ambient_shadow = int32_t(0xFF000000), spot_shadow = int32_t(0xFF000000);
  int32_t clip_left = 0, clip_top = 0, clip_right = 0, clip_bottom = 0;
  bool has_static_matrix = false, has_animation_matrix = false;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<ColorSpaceRgb> color_spaces{0x1101};
  HandleTable<RenderNodeState> render_nodes{0x1102};
  std::atomic<int64_t> next_node_id{1};
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: handles may be freed at exit
  return *t;
}

void register_color_space(rt::NativeRegistry& r) {
  const char* cs = "Landroid/graphics/ColorSpace$Rgb$Native;";
  static const int64_t cs_free = register_free_function([](int64_t h) { tables().color_spaces.remove(h); });
  r.bind(cs, "nativeGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(cs_free); });
  r.bind(cs, "nativeCreate", "(FFFFFFF[F)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto c = std::make_shared<ColorSpaceRgb>();
    for (uint32_t i = 0; i < 7; ++i) c->transfer[i] = a.f32(i);
    if (a.h(7) == kNull) rt.throw_npe("transform");
    rt::ArrayObject& m = rt.array(a.h(7), "float[]");
    if (m.length < 9) rt.throw_new("Ljava/lang/IllegalArgumentException;", "matrice XYZ de 9 valeurs attendue");
    std::memcpy(c->to_xyz.data(), m.data.data(), 9 * sizeof(float));
    return uint64_t(tables().color_spaces.add(std::move(c)));
  });
}

std::shared_ptr<RenderNodeState> node(Runtime& rt, int64_t h) {
  auto n = tables().render_nodes.get(h);
  if (!n) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RenderNode invalide");
  return n;
}

template <typename T>
bool set_changed(T& field, T value) {
  if (field == value) return false;
  field = value;
  return true;
}

void register_render_node(rt::NativeRegistry& r) {
  const char* rn = "Landroid/graphics/RenderNode;";
  static const int64_t rn_free = register_free_function([](int64_t h) { tables().render_nodes.remove(h); });
  r.bind(rn, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(rn_free); });
  r.bind(rn, "nCreate", "(Ljava/lang/String;)J", [](Runtime&, NativeArgs&) -> uint64_t {
    auto n = std::make_shared<RenderNodeState>();
    n->unique_id = tables().next_node_id++;
    return uint64_t(tables().render_nodes.add(std::move(n)));
  }, S::kPartial, "magasin de propriétés de RenderProperties (pas de liste d'affichage avant J7)");

  // Float properties: nSetX(JF)Z / nGetX(J)F.
  using FloatField = float RenderNodeState::*;
  for (const auto& [name, field] : std::initializer_list<std::pair<const char*, FloatField>>{
           {"Alpha", &RenderNodeState::alpha}, {"Elevation", &RenderNodeState::elevation},
           {"TranslationX", &RenderNodeState::translation_x}, {"TranslationY", &RenderNodeState::translation_y},
           {"TranslationZ", &RenderNodeState::translation_z}, {"Rotation", &RenderNodeState::rotation},
           {"RotationX", &RenderNodeState::rotation_x}, {"RotationY", &RenderNodeState::rotation_y},
           {"ScaleX", &RenderNodeState::scale_x}, {"ScaleY", &RenderNodeState::scale_y},
           {"CameraDistance", &RenderNodeState::camera_distance}}) {
    FloatField f = field;
    r.bind(rn, std::string("nSet") + name, "(JF)Z", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      return rt::ret_bool(set_changed((*n).*f, a.f32(2)));
    });
    r.bind(rn, std::string("nGet") + name, "(J)F", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      return rt::ret_float((*n).*f);
    });
  }
  // Pivot: explicit once set; otherwise the centre of the bounds (HWUI).
  for (bool x : {true, false}) {
    r.bind(rn, x ? "nSetPivotX" : "nSetPivotY", "(JF)Z", [x](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      bool changed = set_changed(x ? n->pivot_x : n->pivot_y, a.f32(2)) || !n->pivot_explicit;
      n->pivot_explicit = true;
      return rt::ret_bool(changed);
    });
    r.bind(rn, x ? "nGetPivotX" : "nGetPivotY", "(J)F", [x](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      if (n->pivot_explicit) return rt::ret_float(x ? n->pivot_x : n->pivot_y);
      return rt::ret_float(x ? float(n->right - n->left) / 2 : float(n->bottom - n->top) / 2);
    });
  }
  r.bind(rn, "nIsPivotExplicitlySet", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_bool(n->pivot_explicit);
  });
  r.bind(rn, "nResetPivot", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_bool(set_changed(n->pivot_explicit, false));
  });

  // Bounds.
  using IntField = int32_t RenderNodeState::*;
  for (const auto& [name, field] : std::initializer_list<std::pair<const char*, IntField>>{
           {"Left", &RenderNodeState::left}, {"Top", &RenderNodeState::top},
           {"Right", &RenderNodeState::right}, {"Bottom", &RenderNodeState::bottom},
           {"LayerType", &RenderNodeState::layer_type}, {"AmbientShadowColor", &RenderNodeState::ambient_shadow},
           {"SpotShadowColor", &RenderNodeState::spot_shadow}}) {
    IntField f = field;
    r.bind(rn, std::string("nSet") + name, "(JI)Z", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      return rt::ret_bool(set_changed((*n).*f, a.i32(2)));
    });
    r.bind(rn, std::string("nGet") + name, "(J)I", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      return rt::ret_int((*n).*f);
    });
  }
  r.bind(rn, "nSetLeftTopRightBottom", "(JIIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    bool changed = set_changed(n->left, a.i32(2));
    changed |= set_changed(n->top, a.i32(3));
    changed |= set_changed(n->right, a.i32(4));
    changed |= set_changed(n->bottom, a.i32(5));
    return rt::ret_bool(changed);
  });
  for (bool horizontal : {true, false}) {
    r.bind(rn, horizontal ? "nOffsetLeftAndRight" : "nOffsetTopAndBottom", "(JI)Z",
           [horizontal](Runtime& rt, NativeArgs& a) -> uint64_t {
             auto n = node(rt, a.i64(0));
             std::lock_guard<std::mutex> guard(n->lock);
             int32_t d = a.i32(2);
             if (d == 0) return rt::ret_bool(false);
             (horizontal ? n->left : n->top) += d;
             (horizontal ? n->right : n->bottom) += d;
             return rt::ret_bool(true);
           });
  }
  r.bind(rn, "nGetWidth", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_int(n->right - n->left);
  });
  r.bind(rn, "nGetHeight", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_int(n->bottom - n->top);
  });

  // Boolean properties: nSetX(JZ)Z / nGetX(J)Z.
  using BoolField = bool RenderNodeState::*;
  for (const auto& [set, get, field] : std::initializer_list<std::tuple<const char*, const char*, BoolField>>{
           {"nSetClipToBounds", "nGetClipToBounds", &RenderNodeState::clip_to_bounds},
           {"nSetClipToOutline", "nGetClipToOutline", &RenderNodeState::clip_to_outline},
           {"nSetHasOverlappingRendering", "nHasOverlappingRendering", &RenderNodeState::overlapping},
           {"nSetAllowForceDark", "nGetAllowForceDark", &RenderNodeState::allow_force_dark},
           {"nSetProjectBackwards", nullptr, &RenderNodeState::project_backwards},
           {"nSetProjectionReceiver", nullptr, &RenderNodeState::projection_receiver}}) {
    BoolField f = field;
    r.bind(rn, set, "(JZ)Z", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      return rt::ret_bool(set_changed((*n).*f, a.z(2)));
    });
    if (get) {
      r.bind(rn, get, "(J)Z", [f](Runtime& rt, NativeArgs& a) -> uint64_t {
        auto n = node(rt, a.i64(0));
        std::lock_guard<std::mutex> guard(n->lock);
        return rt::ret_bool((*n).*f);
      });
    }
  }

  // Clip bounds and outline.
  r.bind(rn, "nSetClipBounds", "(JIIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    bool changed = set_changed(n->has_clip_bounds, true);
    changed |= set_changed(n->clip_left, a.i32(2));
    changed |= set_changed(n->clip_top, a.i32(3));
    changed |= set_changed(n->clip_right, a.i32(4));
    changed |= set_changed(n->clip_bottom, a.i32(5));
    return rt::ret_bool(changed);
  });
  r.bind(rn, "nSetClipBoundsEmpty", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_bool(set_changed(n->has_clip_bounds, false));
  });
  auto outline = [](bool present, bool empty) {
    return [present, empty](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto n = node(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(n->lock);
      bool changed = set_changed(n->has_outline, present);
      changed |= set_changed(n->outline_empty, empty);
      return rt::ret_bool(changed);
    };
  };
  r.bind(rn, "nSetOutlineNone", "(J)Z", outline(false, false));
  r.bind(rn, "nSetOutlineEmpty", "(J)Z", outline(true, true));
  r.bind(rn, "nSetOutlineRoundRect", "(JIIIIFF)Z", outline(true, false));
  r.bind(rn, "nSetOutlinePath", "(JJF)Z", outline(true, false));
  r.bind(rn, "nHasShadow", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    return rt::ret_bool(n->has_outline && !n->outline_empty && n->alpha > 0 && n->elevation + n->translation_z > 0);
  });

  // Transforms: identity unless a property or a matrix changes it.
  r.bind(rn, "nHasIdentityMatrix", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto n = node(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(n->lock);
    bool identity = n->translation_x == 0 && n->translation_y == 0 && n->rotation == 0 && n->rotation_x == 0 &&
                    n->rotation_y == 0 && n->scale_x == 1 && n->scale_y == 1 && !n->has_static_matrix &&
                    !n->has_animation_matrix;
    return rt::ret_bool(identity);
  });
  for (bool animation : {false, true}) {
    r.bind(rn, animation ? "nSetAnimationMatrix" : "nSetStaticMatrix", "(JJ)Z",
           [animation](Runtime& rt, NativeArgs& a) -> uint64_t {
             auto n = node(rt, a.i64(0));
             std::lock_guard<std::mutex> guard(n->lock);
             return rt::ret_bool(set_changed(animation ? n->has_animation_matrix : n->has_static_matrix, a.i64(2) != 0));
           },
           S::kPartial, "matrice notée présente ou absente (valeurs : J7)");
  }
  r.bind(rn, "nGetAnimationMatrix", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    node(rt, a.i64(0));
    return rt::ret_bool(false);
  }, S::kPartial, "pas de matrice d'animation conservée avant J7");

  r.bind(rn, "nGetUniqueId", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_long(node(rt, a.i64(0))->unique_id);
  });
  // No display list is recorded before the software renderer (J7).
  r.bind(rn, "nIsValid", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    node(rt, a.i64(0));
    return rt::ret_bool(false);
  }, S::kPartial, "aucune liste d'affichage avant J7");
  for (const char* name : {"nGetAllocatedSize", "nGetUsageSize"}) {
    r.bind(rn, name, "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
      node(rt, a.i64(0));
      return rt::ret_int(0);
    });
  }
  auto noop = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    node(rt, a.i64(0));
    return 0;
  };
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"nDiscardDisplayList", "(J)V"}, {"nOutput", "(J)V"}, {"nEndAllAnimators", "(J)V"},
           {"nForceEndAnimators", "(J)V"}, {"nSetIsTextureView", "(J)V"}, {"nSetUsageHint", "(JI)V"},
           {"nAddAnimator", "(JJ)V"}, {"nRequestPositionUpdates", "(JLjava/lang/ref/WeakReference;)V"}}) {
    r.bind(rn, name, sig, noop, S::kStub, "sans objet avant le rendu (J7)");
  }
  auto unchanged = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    node(rt, a.i64(0));
    return rt::ret_bool(false);
  };
  for (const auto& [name, sig] : std::initializer_list<std::pair<const char*, const char*>>{
           {"nSetRevealClip", "(JZFFF)Z"}, {"nStretch", "(JFFFF)Z"}, {"nClearStretch", "(J)Z"},
           {"nSetRenderEffect", "(JJ)Z"}, {"nSetBackdropRenderEffect", "(JJ)Z"}, {"nSetLayerPaint", "(JJ)Z"}}) {
    r.bind(rn, name, sig, unchanged, S::kStub, "effet de rendu sans objet avant J7");
  }
}

}  // namespace

void register_framework_graphics(rt::NativeRegistry& r) {
  register_color_space(r);
  register_render_node(r);
  register_framework_paint(r);
  register_framework_fonts(r);
  register_framework_path(r);
  register_framework_matrix(r);
}

}  // namespace aa::bridge
