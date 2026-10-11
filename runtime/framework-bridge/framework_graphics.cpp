// Native methods of AOSP's framework.jar graphics classes (G3 J4):
// Paint, Typeface, RenderNode as property stores. Actual rendering comes
// at J7 (software Canvas path). These stubs allow View initialization
// without blocking.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "framework-bridge/internal.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// --- Paint ---------------------------------------------------------------------

// Paint native peer: stores paint properties (color, stroke width, text size,
// flags). No actual drawing yet (J7).
struct PaintPeer final : rt::NativePeer {
  int32_t color = 0xFF000000;  // opaque black
  float stroke_width = 0.0f;
  float text_size = 12.0f;
  int32_t flags = 0;
  int32_t style = 0;  // 0=FILL, 1=STROKE, 2=FILL_AND_STROKE
  int32_t cap = 0;    // 0=BUTT, 1=ROUND, 2=SQUARE
  int32_t join = 0;   // 0=MITER, 1=ROUND, 2=BEVEL
  int32_t align = 0;  // 0=LEFT, 1=CENTER, 2=RIGHT
  Handle typeface = kNull;

  void trace(std::vector<Handle>& out) const override { out.push_back(typeface); }
};

void register_paint(rt::NativeRegistry& r) {
  const char* paint = "Landroid/graphics/Paint;";

  r.bind(paint, "nInit", "()J", [](Runtime& rt, NativeArgs&) -> uint64_t {
    auto peer = std::make_unique<PaintPeer>();
    return rt::ret_long(rt.id_of_peer(std::move(peer)));
  }, S::kPartial, "Paint natif : magasin de propriétés (pas de dessin encore)");

  r.bind(paint, "nGetNativeFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(0); });

  r.bind(paint, "nSetColor", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->color = a.i32(2);
    return 0;
  });

  r.bind(paint, "nGetColor", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<PaintPeer>(id);
    return rt::ret_int(p ? p->color : 0xFF000000);
  });

  r.bind(paint, "nSetStrokeWidth", "(JF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->stroke_width = a.f32(2);
    return 0;
  });

  r.bind(paint, "nSetTextSize", "(JF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->text_size = a.f32(2);
    return 0;
  });

  r.bind(paint, "nGetTextSize", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<PaintPeer>(id);
    return rt::ret_float(p ? p->text_size : 12.0f);
  });

  r.bind(paint, "nSetFlags", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->flags = a.i32(2);
    return 0;
  });

  r.bind(paint, "nGetFlags", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<PaintPeer>(id);
    return rt::ret_int(p ? p->flags : 0);
  });

  r.bind(paint, "nSetStyle", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->style = a.i32(2);
    return 0;
  });

  r.bind(paint, "nSetStrokeCap", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->cap = a.i32(2);
    return 0;
  });

  r.bind(paint, "nSetStrokeJoin", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->join = a.i32(2);
    return 0;
  });

  r.bind(paint, "nSetTextAlign", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) p->align = a.i32(2);
    return 0;
  });

  r.bind(paint, "nSetTypeface", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    int64_t typeface_id = a.i64(2);
    if (auto* p = rt.peer_by_id<PaintPeer>(id)) {
      // Store typeface handle (null if typeface_id is 0)
      p->typeface = typeface_id ? rt.handle_of_peer(typeface_id) : kNull;
    }
    return rt::ret_long(typeface_id);
  });

  // Text measurement stubs (return placeholder values for J4)
  r.bind(paint, "nGetTextAdvances", "(JLjava/lang/String;IIIII[FI)F",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
    // Placeholder: assume 8 pixels per character
    Handle str = a.h(2);
    if (!str) return rt::ret_float(0.0f);
    int32_t start = a.i32(3);
    int32_t end = a.i32(4);
    int32_t count = end - start;
    return rt::ret_float(count * 8.0f);
  }, S::kStub, "mesure de texte : 8px par caractère (polices à J6)");

  r.bind(paint, "nGetTextBounds", "(JLjava/lang/String;IIILandroid/graphics/Rect;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
    // Placeholder: set rect to text width × height
    Handle str = a.h(2);
    Handle rect = a.h(6);
    if (!str || !rect) return 0;
    int32_t start = a.i32(3);
    int32_t end = a.i32(4);
    int32_t count = end - start;
    int32_t width = count * 8;
    int32_t height = 12;
    rt.set_field_int(rect, "left", 0);
    rt.set_field_int(rect, "top", -height);
    rt.set_field_int(rect, "right", width);
    rt.set_field_int(rect, "bottom", 0);
    return 0;
  }, S::kStub, "limites de texte : placeholder (polices à J6)");
}

// --- Typeface ------------------------------------------------------------------

struct TypefacePeer final : rt::NativePeer {
  int32_t style = 0;  // 0=NORMAL, 1=BOLD, 2=ITALIC, 3=BOLD_ITALIC
  int32_t weight = 400;
};

void register_typeface(rt::NativeRegistry& r) {
  const char* tf = "Landroid/graphics/Typeface;";

  r.bind(tf, "nativeCreateFromTypeface", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto peer = std::make_unique<TypefacePeer>();
    peer->style = a.i32(2);
    return rt::ret_long(rt.id_of_peer(std::move(peer)));
  }, S::kStub, "Typeface natif : magasin de style");

  r.bind(tf, "nativeCreateWeightAlias", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto peer = std::make_unique<TypefacePeer>();
    int64_t base_id = a.i64(1);
    if (auto* base = rt.peer_by_id<TypefacePeer>(base_id)) peer->style = base->style;
    peer->weight = a.i32(2);
    return rt::ret_long(rt.id_of_peer(std::move(peer)));
  }, S::kStub, "Typeface avec poids : magasin de propriétés");

  r.bind(tf, "nativeGetStyle", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<TypefacePeer>(id);
    return rt::ret_int(p ? p->style : 0);
  });

  r.bind(tf, "nativeGetWeight", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<TypefacePeer>(id);
    return rt::ret_int(p ? p->weight : 400);
  });

  r.bind(tf, "nativeGetReleaseFunc", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(0);
  });
}

// --- RenderNode ----------------------------------------------------------------

struct RenderNodePeer final : rt::NativePeer {
  std::string name;
  float alpha = 1.0f;
  float elevation = 0.0f;
  float translation_x = 0.0f;
  float translation_y = 0.0f;
  float translation_z = 0.0f;
  float rotation = 0.0f;
  float rotation_x = 0.0f;
  float rotation_y = 0.0f;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float pivot_x = 0.0f;
  float pivot_y = 0.0f;
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
  bool clip_to_outline = false;
};

void register_render_node(rt::NativeRegistry& r) {
  const char* rn = "Landroid/graphics/RenderNode;";

  r.bind(rn, "nCreate", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto peer = std::make_unique<RenderNodePeer>();
    Handle name_h = a.h(1);
    if (name_h) peer->name = rt.string_utf8(name_h);
    return rt::ret_long(rt.id_of_peer(std::move(peer)));
  }, S::kStub, "RenderNode natif : magasin de propriétés de transformation");

  r.bind(rn, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(0);
  });

  r.bind(rn, "nSetAlpha", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->alpha = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetElevation", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->elevation = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetTranslationX", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->translation_x = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetTranslationY", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->translation_y = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetTranslationZ", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->translation_z = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetRotation", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->rotation = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetRotationX", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->rotation_x = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetRotationY", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->rotation_y = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetScaleX", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->scale_x = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetScaleY", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->scale_y = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetPivotX", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->pivot_x = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetPivotY", "(JF)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->pivot_y = a.f32(2);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetLeftTopRightBottom", "(JIIII)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->left = a.i32(2);
      p->top = a.i32(3);
      p->right = a.i32(4);
      p->bottom = a.i32(5);
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  r.bind(rn, "nSetClipToBounds", "(JZ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    if (auto* p = rt.peer_by_id<RenderNodePeer>(id)) {
      p->clip_to_outline = a.i32(2) != 0;
      return rt::ret_bool(true);
    }
    return rt::ret_bool(false);
  });

  // Getters
  r.bind(rn, "nGetAlpha", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<RenderNodePeer>(id);
    return rt::ret_float(p ? p->alpha : 1.0f);
  });

  r.bind(rn, "nGetElevation", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(1);
    auto* p = rt.peer_by_id<RenderNodePeer>(id);
    return rt::ret_float(p ? p->elevation : 0.0f);
  });
}

}  // namespace

void register_framework_graphics(rt::NativeRegistry& r) {
  register_paint(r);
  register_typeface(r);
  register_render_node(r);
}

}  // namespace aa::bridge

