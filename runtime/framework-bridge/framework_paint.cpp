// android.graphics.Paint natives (G3, ADR-006 J4), each signature checked
// against the framework.jar of Android 17. Until the software renderer (J7)
// a paint is a faithful property store with HWUI's defaults (hwui/Paint.cpp,
// SkPaint, SkFont): what Java sets, Java reads back. Text measurement
// (metrics, advances, bounds, breaks) needs the font engine and stays
// unbound until then: a call fails visibly instead of returning made-up sizes.
#include "framework-bridge/internal.h"

#include <memory>
#include <mutex>
#include <string>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// Paint's flags (android.graphics.Paint), stored as Java sees them.
constexpr int32_t kAntiAlias = 0x01, kFilterBitmap = 0x02, kDither = 0x04, kUnderline = 0x08,
                  kStrikeThru = 0x10, kFakeBold = 0x20, kLinearText = 0x40, kSubpixelText = 0x80;
constexpr int32_t kElegantTextHeightUnset = -1;
constexpr int32_t kHintingOn = 1;  // SkFont's default hinting (kNormal)

struct PaintState {
  int32_t flags = 0;
  int32_t color = int32_t(0xFF000000);  // SkPaint: opaque black
  int64_t color_long = 0, color_space = 0;
  bool wide_color = false;  // last set through nSetColor(JJJ)
  int32_t style = 0, cap = 0, join = 0, align = 0;  // FILL, BUTT, MITER, LEFT
  float stroke_width = 0, stroke_miter = 4;
  float text_size = 12, text_scale_x = 1, text_skew_x = 0;  // SkFont
  float letter_spacing = 0, word_spacing = 0;
  int32_t hinting = kHintingOn, elegant_text_height = kElegantTextHeightUnset;
  int32_t start_hyphen = 0, end_hyphen = 0;
  int32_t locale_list_id = 0;
  std::u16string font_features;
  int64_t typeface = 0, shader = 0, color_filter = 0, mask_filter = 0, path_effect = 0, xfermode = 0;
  int32_t blend_mode = 3;  // SRC_OVER
  float shadow_radius = 0, shadow_dx = 0, shadow_dy = 0;
  int64_t shadow_color_space = 0, shadow_color = 0;
};

struct Paint {
  std::mutex lock;
  PaintState s;
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<Paint> paints{0x1103};
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: handles may be freed at exit
  return *t;
}

std::shared_ptr<Paint> paint(Runtime& rt, int64_t h) {
  auto p = tables().paints.get(h);
  if (!p) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Paint invalide");
  return p;
}

// Runs fn(state, args) under the paint's lock; the paint is argument 0.
template <typename Fn>
rt::NativeFn with_paint(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto p = paint(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(p->lock);
    return fn(p->s, a);
  };
}

int64_t add_paint(const PaintState& s) {
  auto p = std::make_shared<Paint>();
  p->s = s;
  return tables().paints.add(std::move(p));
}

void bind_flag(rt::NativeRegistry& r, const char* cls, const char* name, int32_t bit) {
  r.bind(cls, name, "(JZ)V", with_paint([bit](PaintState& s, NativeArgs& a) -> uint64_t {
    s.flags = a.z(2) ? s.flags | bit : s.flags & ~bit;
    return 0;
  }));
}

}  // namespace

void register_framework_paint(rt::NativeRegistry& r) {
  const char* p = "Landroid/graphics/Paint;";
  static const int64_t paint_free = register_free_function([](int64_t h) { tables().paints.remove(h); });
  r.bind(p, "nGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(paint_free); });
  r.bind(p, "nInit", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return uint64_t(add_paint(PaintState{})); },
         S::kPartial, "magasin de propriétés (rendu et mesure du texte : J7)");
  r.bind(p, "nInitWithPaint", "(J)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = paint(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(src->lock);
    return uint64_t(add_paint(src->s));
  });
  r.bind(p, "nReset", "(J)V", with_paint([](PaintState& s, NativeArgs&) -> uint64_t {
    s = PaintState{};
    return 0;
  }));
  r.bind(p, "nSet", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dst = paint(rt, a.i64(0));
    auto src = paint(rt, a.i64(2));
    if (dst == src) return 0;
    std::scoped_lock guard(dst->lock, src->lock);
    dst->s = src->s;
    return 0;
  });

  // Flags, as a whole and bit by bit.
  r.bind(p, "nGetFlags", "(J)I", with_paint([](PaintState& s, NativeArgs&) -> uint64_t { return rt::ret_int(s.flags); }));
  r.bind(p, "nSetFlags", "(JI)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.flags = a.i32(2);
    return 0;
  }));
  bind_flag(r, p, "nSetAntiAlias", kAntiAlias);
  bind_flag(r, p, "nSetFilterBitmap", kFilterBitmap);
  bind_flag(r, p, "nSetDither", kDither);
  bind_flag(r, p, "nSetUnderlineText", kUnderline);
  bind_flag(r, p, "nSetStrikeThruText", kStrikeThru);
  bind_flag(r, p, "nSetFakeBoldText", kFakeBold);
  bind_flag(r, p, "nSetLinearText", kLinearText);
  bind_flag(r, p, "nSetSubpixelText", kSubpixelText);

  // Integer properties: nGetX(J)I / nSetX(JI)V.
  using IntField = int32_t PaintState::*;
  for (const auto& [name, field, has_get] : std::initializer_list<std::tuple<const char*, IntField, bool>>{
           {"Style", &PaintState::style, true}, {"StrokeCap", &PaintState::cap, true},
           {"StrokeJoin", &PaintState::join, true}, {"TextAlign", &PaintState::align, true},
           {"Hinting", &PaintState::hinting, true}, {"ElegantTextHeight", &PaintState::elegant_text_height, true},
           {"StartHyphenEdit", &PaintState::start_hyphen, true}, {"EndHyphenEdit", &PaintState::end_hyphen, true}}) {
    IntField f = field;
    r.bind(p, std::string("nSet") + name, "(JI)V", with_paint([f](PaintState& s, NativeArgs& a) -> uint64_t {
      s.*f = a.i32(2);
      return 0;
    }));
    if (has_get) {
      r.bind(p, std::string("nGet") + name, "(J)I", with_paint([f](PaintState& s, NativeArgs&) -> uint64_t {
        return rt::ret_int(s.*f);
      }));
    }
  }

  // Float properties: nGetX(J)F / nSetX(JF)V.
  using FloatField = float PaintState::*;
  for (const auto& [name, field] : std::initializer_list<std::pair<const char*, FloatField>>{
           {"StrokeWidth", &PaintState::stroke_width}, {"StrokeMiter", &PaintState::stroke_miter},
           {"TextSize", &PaintState::text_size}, {"TextScaleX", &PaintState::text_scale_x},
           {"TextSkewX", &PaintState::text_skew_x}, {"LetterSpacing", &PaintState::letter_spacing},
           {"WordSpacing", &PaintState::word_spacing}}) {
    FloatField f = field;
    r.bind(p, std::string("nSet") + name, "(JF)V", with_paint([f](PaintState& s, NativeArgs& a) -> uint64_t {
      s.*f = a.f32(2);
      return 0;
    }));
    r.bind(p, std::string("nGet") + name, "(J)F", with_paint([f](PaintState& s, NativeArgs&) -> uint64_t {
      return rt::ret_float(s.*f);
    }));
  }

  // Colour: Java keeps the colour it reports (mColor); the native side only
  // needs it to draw.
  r.bind(p, "nSetColor", "(JI)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.color = a.i32(2);
    s.wide_color = false;
    return 0;
  }));
  r.bind(p, "nSetColor", "(JJJ)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.color_space = a.i64(2);
    s.color_long = a.i64(4);
    s.wide_color = true;
    return 0;
  }), S::kPartial, "couleur longue conservée telle quelle (conversion : J7)");
  r.bind(p, "nSetAlpha", "(JI)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.color = int32_t((uint32_t(s.color) & 0x00FFFFFFu) | (uint32_t(a.i32(2) & 0xFF) << 24));
    return 0;
  }));

  // Effects: the paint keeps the other native object's handle and, as HWUI,
  // returns it (0 clears).
  using HandleField = int64_t PaintState::*;
  for (const auto& [name, field] : std::initializer_list<std::pair<const char*, HandleField>>{
           {"nSetShader", &PaintState::shader}, {"nSetColorFilter", &PaintState::color_filter},
           {"nSetMaskFilter", &PaintState::mask_filter}, {"nSetPathEffect", &PaintState::path_effect}}) {
    HandleField f = field;
    r.bind(p, name, "(JJ)J", with_paint([f](PaintState& s, NativeArgs& a) -> uint64_t {
      s.*f = a.i64(2);
      return rt::ret_long(s.*f);
    }), S::kPartial, "référence conservée (effet appliqué au rendu : J7)");
  }
  r.bind(p, "nSetXfermode", "(JI)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.blend_mode = a.i32(2);
    s.xfermode = 0;
    return 0;
  }));
  r.bind(p, "nSetXfermode", "(JJ)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.xfermode = a.i64(2);
    return 0;
  }), S::kPartial, "référence conservée (effet appliqué au rendu : J7)");
  r.bind(p, "nSetTypeface", "(JJ)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.typeface = a.i64(2);
    return 0;
  }), S::kPartial, "référence conservée (polices : J7)");
  r.bind(p, "nSetShadowLayer", "(JFFFJJ)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.shadow_radius = a.f32(2);
    s.shadow_dx = a.f32(3);
    s.shadow_dy = a.f32(4);
    s.shadow_color_space = a.i64(5);
    s.shadow_color = a.i64(7);
    return 0;
  }));
  // HWUI drops the shadow when the radius is not positive.
  r.bind(p, "nHasShadowLayer", "(J)Z", with_paint([](PaintState& s, NativeArgs&) -> uint64_t {
    return rt::ret_bool(s.shadow_radius > 0);
  }));

  // Text attributes.
  r.bind(p, "nSetTextLocales", "(JLjava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = paint(rt, a.i64(0));
    int32_t id = minikin_locale_list_id(rt, rt.string_value(a.h(2)));
    std::lock_guard<std::mutex> guard(pt->lock);
    pt->s.locale_list_id = id;
    return rt::ret_int(id);
  });
  r.bind(p, "nSetTextLocalesByMinikinLocaleListId", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = paint(rt, a.i64(0));
    int32_t id = a.i32(2);
    if (!minikin_locale_list_valid(id)) rt.throw_new("Ljava/lang/IllegalArgumentException;", "liste de locales inconnue");
    std::lock_guard<std::mutex> guard(pt->lock);
    pt->s.locale_list_id = id;
    return 0;
  });
  r.bind(p, "nSetFontFeatureSettings", "(JLjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto pt = paint(rt, a.i64(0));
    std::u16string settings = a.h(2) == rt::kNull ? std::u16string() : rt.string_value(a.h(2));
    std::lock_guard<std::mutex> guard(pt->lock);
    pt->s.font_features = std::move(settings);
    return 0;
  });
  // What changes the size or shape of text (minikin's MinikinPaint equality).
  r.bind(p, "nEqualsForTextMeasurement", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto x = paint(rt, a.i64(0));
    auto y = paint(rt, a.i64(2));
    if (x == y) return rt::ret_bool(true);
    std::scoped_lock guard(x->lock, y->lock);
    const PaintState &l = x->s, &m = y->s;
    constexpr int32_t kTextFlags = kFakeBold | kLinearText | kSubpixelText;
    return rt::ret_bool(l.text_size == m.text_size && l.text_scale_x == m.text_scale_x &&
                        l.text_skew_x == m.text_skew_x && l.letter_spacing == m.letter_spacing &&
                        l.word_spacing == m.word_spacing && (l.flags & kTextFlags) == (m.flags & kTextFlags) &&
                        l.hinting == m.hinting && l.elegant_text_height == m.elegant_text_height &&
                        l.start_hyphen == m.start_hyphen && l.end_hyphen == m.end_hyphen &&
                        l.locale_list_id == m.locale_list_id && l.font_features == m.font_features &&
                        l.typeface == m.typeface);
  });
}

}  // namespace aa::bridge
