// android.graphics.Paint natives (G3, ADR-006 J4), each signature checked
// against the framework.jar of Android 17. A paint is a property store with
// HWUI's defaults (hwui/Paint.cpp, SkPaint, SkFont): what Java sets, Java
// reads back. Text is measured by the text engine (runtime/text, ADR-007)
// with the rules of HWUI's jni/Paint.cpp and MinikinUtils.cpp.
#include "framework-bridge/internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/text.h"
#include "framework-bridge/handles.h"
#include "framework-bridge/skia_bridge.h"
#include "text/layout.h"

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
  int64_t typeface = 0, xfermode = 0;
  // The effects, referenced when set (HWUI's Paint holds the Skia objects).
  sk_sp<SkShader> shader;
  sk_sp<SkColorFilter> color_filter;
  sk_sp<SkMaskFilter> mask_filter;
  sk_sp<SkPathEffect> path_effect;
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


// --- Text measurement (HWUI's jni/Paint.cpp and MinikinUtils.cpp) ---------

using rt::Handle;
using rt::kNull;

constexpr int32_t kEmbeddedBitmapText = 0x400, kForceAutoHinting = 0x800, kVerticalText = 0x1000,
                  kTextRunLeftEdge = 0x2000, kTextRunRightEdge = 0x4000;
constexpr int32_t kElegantDisabled = 0, kElegantEnabled = 1;
// hwui/Paint.h: underline and strike-through when the font has none.
constexpr float kStdStrikeThruOffset = -6.0f / 21.0f;
constexpr float kStdUnderlineOffset = 1.0f / 9.0f;
constexpr float kStdUnderlineThickness = 1.0f / 18.0f;
constexpr float kStdUnderlineTop = kStdUnderlineOffset - 0.5f * kStdUnderlineThickness;
constexpr float kStdStrikeThruThickness = kStdUnderlineThickness;
constexpr float kStdStrikeThruTop = kStdStrikeThruOffset - 0.5f * kStdStrikeThruThickness;

PaintState snapshot(Runtime& rt, int64_t h) {
  auto p = paint(rt, h);
  std::lock_guard<std::mutex> guard(p->lock);
  return p->s;
}

// The SkFont that HWUI's Paint holds (Paint::setJavaFlags, setHinting).
text::FontSpec skia_font_of(const PaintState& s) {
  text::FontSpec f;
  f.size = s.text_size;
  f.scale_x = s.text_scale_x;
  f.skew_x = s.text_skew_x;
  f.hinting = s.hinting == 0 ? text::Hinting::kNone : text::Hinting::kNormal;
  f.edging = (s.flags & kAntiAlias) ? text::Edging::kAntiAlias : text::Edging::kAlias;
  f.embolden = (s.flags & kFakeBold) != 0;
  f.linear_metrics = (s.flags & kLinearText) != 0;
  f.subpixel = (s.flags & kSubpixelText) != 0;
  f.embedded_bitmaps = (s.flags & kEmbeddedBitmapText) != 0;
  f.force_autohint = (s.flags & kForceAutoHinting) != 0;
  return f;
}

// MinikinUtils::prepareMinikinPaint.
text::Paint minikin_paint(Runtime& rt, const PaintState& s) {
  ensure_text_services(rt);
  TypefaceInfo t = resolve_typeface(rt, s.typeface);
  text::Paint p;
  p.font = t.collection;
  p.size = (s.flags & kLinearText) ? s.text_size : float(int(s.text_size));
  p.scale_x = s.text_scale_x;
  p.skew_x = s.text_skew_x;
  p.letter_spacing = s.letter_spacing;
  p.word_spacing = s.word_spacing;
  p.flags = skia_font_of(s);
  p.locale_list_id = uint32_t(s.locale_list_id);
  p.font_style = t.style;
  p.font_features = text::parse_font_features(text::utf16_to_utf8(s.font_features));
  p.vertical_text = (s.flags & kVerticalText) != 0;
  // An unset elegant text height lays out with the elegant variant.
  p.family_variant = s.elegant_text_height == kElegantDisabled ? text::FamilyVariant::kDefault
                                                                : text::FamilyVariant::kElegant;
  return p;
}

text::StartHyphenEdit start_hyphen_of(const PaintState& s) { return text::StartHyphenEdit(s.start_hyphen & 0x3); }
text::EndHyphenEdit end_hyphen_of(const PaintState& s) { return text::EndHyphenEdit(s.end_hyphen & 0x7); }
text::RunFlag run_flag_of(const PaintState& s) {
  return text::RunFlag(((s.flags & kTextRunLeftEdge) ? text::kRunLeftEdge : 0) |
                       ((s.flags & kTextRunRightEdge) ? text::kRunRightEdge : 0));
}

int32_t target_sdk(Runtime& rt) {
  Handle vm = Handle(rt.call_static("Ldalvik/system/VMRuntime;", "getRuntime", "()Ldalvik/system/VMRuntime;"));
  return vm == kNull ? 10000 : int32_t(rt.call_virtual(vm, "getTargetSdkVersion", "()I"));
}

// getMetricsInternal: the metrics of the typeface's base font.
float paint_metrics(Runtime& rt, const PaintState& s, text::FontMetrics* m, bool use_locale) {
  ensure_text_services(rt);
  TypefaceInfo t = resolve_typeface(rt, s.typeface);
  text::FakedFont base = t.collection->base_font_faked(t.style);
  text::FontSpec f = skia_font_of(s);
  f.embolden = f.embolden || base.fakery.fake_bold;
  if (base.fakery.fake_italic) f.skew_x -= 0.25f;
  *m = text::FontMetrics{};
  float spacing = 0;
  if (auto face = base.face()) spacing = face->metrics(f, m);
  // Hard-coded elegant metrics, before target SDK 35 only.
  if (s.elegant_text_height == kElegantEnabled && target_sdk(rt) < 35) {
    float size = s.text_size;
    m->top = -size * 2500 / 2048;
    m->bottom = -size * -1000 / 2048;
    m->ascent = -size * 1900 / 2048;
    m->descent = -size * -500 / 2048;
    m->leading = size * 0 / 2048;
    spacing = m->descent - m->ascent + m->leading;
  }
  if (use_locale) {
    text::Extent e = text::reference_extent_for_locale(minikin_paint(rt, s));
    m->ascent = std::min(e.ascent, m->ascent);
    m->descent = std::max(e.descent, m->descent);
    m->top = std::min(m->ascent, m->top);
    m->bottom = std::max(m->descent, m->bottom);
  }
  return spacing;
}

// Skia's float to int conversions (saturating).
int32_t saturate(float x) {
  if (!(x == x)) return 0;
  return int32_t(std::clamp(x, -2147483520.0f, 2147483520.0f));
}
int32_t round_to_int(float x) { return saturate(std::floor(x + 0.5f)); }
int32_t floor_to_int(float x) { return saturate(std::floor(x)); }
int32_t ceil_to_int(float x) { return saturate(std::ceil(x)); }

void set_field(Runtime& rt, Handle obj, const char* name, const char* type, uint32_t bits) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = find_field(o.klass, name, type);
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
  o.fields.at(f->slot) = bits;
}
void set_int(Runtime& rt, Handle obj, const char* name, int32_t v) { set_field(rt, obj, name, "I", uint32_t(v)); }
void set_float(Runtime& rt, Handle obj, const char* name, float v) {
  set_field(rt, obj, name, "F", std::bit_cast<uint32_t>(v));
}

// GraphicsJNI::set_metrics and set_metrics_int.
void set_metrics(Runtime& rt, Handle obj, const text::FontMetrics& m) {
  if (obj == kNull) return;
  set_float(rt, obj, "top", m.top);
  set_float(rt, obj, "ascent", m.ascent);
  set_float(rt, obj, "descent", m.descent);
  set_float(rt, obj, "bottom", m.bottom);
  set_float(rt, obj, "leading", m.leading);
}
int32_t set_metrics_int(Runtime& rt, Handle obj, const text::FontMetrics& m) {
  int32_t ascent = round_to_int(m.ascent), descent = round_to_int(m.descent), leading = round_to_int(m.leading);
  if (obj != kNull) {
    set_int(rt, obj, "top", floor_to_int(m.top));
    set_int(rt, obj, "ascent", ascent);
    set_int(rt, obj, "descent", descent);
    set_int(rt, obj, "bottom", ceil_to_int(m.bottom));
    set_int(rt, obj, "leading", leading);
  }
  return descent - ascent + leading;
}

[[noreturn]] void throw_aioobe(Runtime& rt) { rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", ""); }

// The characters of a char[] (copied: the engine may call into Java).
std::u16string chars_of(Runtime& rt, Handle array) {
  if (array == kNull) rt.throw_npe("text");
  rt::ArrayObject& a = rt.array(array, "char[]");
  if (a.width != 2) rt.throw_new("Ljava/lang/IllegalArgumentException;", "char[] attendu");
  std::u16string out(a.length, u'\0');
  std::memcpy(out.data(), a.data.data(), size_t(a.length) * 2);
  return out;
}
std::u16string string_of(Runtime& rt, Handle str) {
  if (str == kNull) rt.throw_npe("text");
  return rt.string_value(str);
}
// [start, start + count) within a buffer of `size` characters.
void check_range(Runtime& rt, int64_t start, int64_t count, int64_t size) {
  if (start < 0 || count < 0 || start + count > size) throw_aioobe(rt);
}
void write_floats(Runtime& rt, Handle array, int32_t index, const float* values, size_t count) {
  rt::ArrayObject& a = rt.array(array, "float[]");
  if (a.width != 4 || index < 0 || int64_t(index) + int64_t(count) > int64_t(a.length)) throw_aioobe(rt);
  std::memcpy(a.data.data() + size_t(index) * 4, values, count * 4);
}

// doTextAdvances, on buf = the context.
float text_advances(Runtime& rt, const PaintState& s, std::u16string_view buf, int32_t start, int32_t count,
                    int32_t bidi, Handle advances, int32_t advances_index) {
  if ((start | count | advances_index) < 0 || int64_t(start) + count > int64_t(buf.size())) throw_aioobe(rt);
  if (count == 0) return 0;
  if (advances != kNull) {
    rt::ArrayObject& a = rt.array(advances, "float[]");
    if (int64_t(count) + advances_index > int64_t(a.length)) throw_aioobe(rt);
  }
  std::vector<float> out(size_t(count), 0.0f);
  text::Paint mp = minikin_paint(rt, s);
  float advance = text::measure_text(buf, size_t(start), size_t(count), text::Bidi(bidi & 7), mp, start_hyphen_of(s),
                                     end_hyphen_of(s), out.data(), nullptr, nullptr, run_flag_of(s));
  if (advances != kNull) write_floats(rt, advances, advances_index, out.data(), out.size());
  return advance;
}

// doRunAdvance: the advance from `start` to `offset`.
float run_advance(Runtime& rt, const PaintState& s, std::u16string_view buf, int32_t start, int32_t count, bool rtl,
                  int32_t offset, Handle advances, int32_t advances_index, text::TextRect* bounds,
                  uint32_t* cluster_count) {
  if (advances != kNull) {
    rt::ArrayObject& a = rt.array(advances, "float[]");
    if (int64_t(count) + advances_index > int64_t(a.length)) throw_aioobe(rt);
  }
  text::Bidi bidi = rtl ? text::Bidi::kForceRtl : text::Bidi::kForceLtr;
  text::Paint mp = minikin_paint(rt, s);
  std::vector<float> out(size_t(count), 0.0f);
  float total = text::measure_text(buf, size_t(start), size_t(count), bidi, mp, start_hyphen_of(s), end_hyphen_of(s),
                                   out.data(), bounds, cluster_count, run_flag_of(s));
  if (offset == start + count && advances == kNull) return total;
  float result = text::run_advance(out.data(), buf.data(), size_t(start), size_t(count), size_t(offset));
  if (advances != kNull) {
    text::distribute_advances(out.data(), buf.data(), size_t(start), size_t(count));
    write_floats(rt, advances, advances_index, out.data(), out.size());
  }
  return result;
}

// Layout::nGlyphs and friends for hasGlyph.
bool layout_contains_notdef(const text::TextLayout& l) {
  for (const auto& g : l.glyphs) {
    if (g.glyph == 0) return true;
  }
  return false;
}
size_t count_non_space_glyphs(const text::TextLayout& l) {
  size_t count = 0;
  constexpr uint32_t kSpaceGlyphId = 3;  // as HWUI assumes
  for (size_t i = 0; i < l.glyphs.size(); ++i) {
    float advance = i < l.advances.size() ? l.advances[i] : 0.0f;  // getCharAdvance(i), as HWUI calls it
    if (l.glyphs[i].glyph != kSpaceGlyphId || advance != 0.0f) count++;
  }
  return count;
}
bool is_flag(std::u16string_view s) {
  if (s.size() != 4 || s[0] != 0xD83C || s[2] != 0xD83C) return false;
  return 0xDDE6 <= s[1] && s[1] <= 0xDDFF && 0xDDE6 <= s[3] && s[3] <= 0xDDFF;
}

bool has_glyph(Runtime& rt, const PaintState& s, int32_t bidi, std::u16string_view str) {
  size_t chars = 0;
  constexpr uint32_t kStartOfString = 0xFFFFFFFF;
  uint32_t prev = kStartOfString;
  TypefaceInfo t = resolve_typeface(rt, s.typeface);
  for (size_t i = 0; i < str.size(); i++) {
    char16_t cu = str[i];
    uint32_t cp = cu;
    if ((cu & 0xFC00) == 0xDC00) return false;
    if ((cu & 0xFC00) == 0xD800) {
      if (i + 1 == str.size()) return false;
      char16_t cu2 = str[++i];
      if ((cu2 & 0xFC00) != 0xDC00) return false;
      cp = 0x10000 + ((uint32_t(cu) - 0xD800) << 10) + (uint32_t(cu2) - 0xDC00);
    }
    if (prev != kStartOfString && ((0xFE00 <= cp && cp <= 0xFE0F) || (0xE0100 <= cp && cp <= 0xE01EF))) {
      if (!t.collection->has_variation_selector(prev, cp)) return false;
      if (chars == 1 && i + 1 == str.size()) return true;
    }
    chars++;
    prev = cp;
  }
  text::Paint mp = minikin_paint(rt, s);
  text::Bidi b = text::Bidi(bidi & 7);
  text::TextLayout layout = text::do_layout(str, 0, str.size(), b, mp, start_hyphen_of(s), end_hyphen_of(s),
                                            run_flag_of(s));
  size_t glyphs = count_non_space_glyphs(layout);
  if (glyphs != 1 && chars > 1) return false;
  if (glyphs == 0 || layout_contains_notdef(layout)) return false;
  if (chars == 2 && is_flag(str)) {
    static const std::u16string zz = u"\U0001F1FF\U0001F1FF";
    text::TextLayout zl = text::do_layout(zz, 0, 4, b, mp, start_hyphen_of(s), end_hyphen_of(s), run_flag_of(s));
    if (zl.glyphs.size() != 1 || layout_contains_notdef(zl)) return true;
    return zl.glyphs[0].glyph != layout.glyphs[0].glyph;
  }
  return true;
}

int32_t break_text(Runtime& rt, const PaintState& s, std::u16string_view text, float max_width, int32_t bidi,
                   Handle measured_out, bool forward) {
  size_t count = text.size();
  std::vector<float> adv(count, 0.0f);
  text::Paint mp = minikin_paint(rt, s);
  text::measure_text(text, 0, count, text::Bidi(bidi & 7), mp, start_hyphen_of(s), end_hyphen_of(s), adv.data(),
                     nullptr, nullptr, run_flag_of(s));
  size_t measured_count = 0;
  float measured = 0;
  for (size_t i = 0; i < count; i++) {
    size_t index = forward ? i : count - i - 1;
    float width = adv[index];
    if (measured + width > max_width) break;
    if (forward || width != 0.0f) measured_count = i + 1;
    measured += width;
  }
  if (measured_out != kNull) {
    rt::ArrayObject& a = rt.array(measured_out, "float[]");
    if (a.length > 0) write_floats(rt, measured_out, 0, &measured, 1);
  }
  return int32_t(measured_count);
}

// doTextBounds: the bounds rounded out to a Rect.
void text_bounds(Runtime& rt, const PaintState& s, std::u16string_view text, int32_t bidi, Handle rect) {
  text::Paint mp = minikin_paint(rt, s);
  text::TextRect r = text::text_bounds(text, 0, text.size(), text::Bidi(bidi & 7), mp, start_hyphen_of(s),
                                       end_hyphen_of(s));
  if (rect == kNull) rt.throw_npe("bounds");
  set_int(rt, rect, "left", floor_to_int(r.left));
  set_int(rt, rect, "top", floor_to_int(r.top));
  set_int(rt, rect, "right", ceil_to_int(r.right));
  set_int(rt, rect, "bottom", ceil_to_int(r.bottom));
}

// doFontExtent.
void font_extent_int(Runtime& rt, const PaintState& s, std::u16string_view buf, int32_t start, int32_t count,
                     bool rtl, Handle fmi) {
  text::Paint mp = minikin_paint(rt, s);
  text::Extent e = text::text_extent(buf, size_t(start), size_t(count), rtl ? text::Bidi::kForceRtl : text::Bidi::kForceLtr, mp);
  text::FontMetrics m;
  paint_metrics(rt, s, &m, false);
  m.ascent = e.ascent;
  m.descent = e.descent;
  m.top = std::min(m.ascent, m.top);
  m.bottom = std::max(m.descent, m.bottom);
  set_metrics_int(rt, fmi, m);
}

void register_paint_text(rt::NativeRegistry& r) {
  const char* p = "Landroid/graphics/Paint;";
  r.bind(p, "nGetFontMetrics", "(JLandroid/graphics/Paint$FontMetrics;Z)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    text::FontMetrics m;
    float spacing = paint_metrics(rt, snapshot(rt, a.i64(0)), &m, a.z(3));
    set_metrics(rt, a.h(2), m);
    return rt::ret_float(spacing);
  });
  r.bind(p, "nGetFontMetricsInt", "(JLandroid/graphics/Paint$FontMetricsInt;Z)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    text::FontMetrics m;
    paint_metrics(rt, snapshot(rt, a.i64(0)), &m, a.z(3));
    return rt::ret_int(set_metrics_int(rt, a.h(2), m));
  });
  r.bind(p, "nAscent", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    text::FontMetrics m;
    paint_metrics(rt, snapshot(rt, a.i64(0)), &m, false);
    return rt::ret_float(m.ascent);
  });
  r.bind(p, "nDescent", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    text::FontMetrics m;
    paint_metrics(rt, snapshot(rt, a.i64(0)), &m, false);
    return rt::ret_float(m.descent);
  });
  r.bind(p, "nGetUnderlinePosition", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    text::FontMetrics m;
    paint_metrics(rt, s, &m, false);
    if (m.flags & text::FontMetrics::kUnderlinePositionValid) return rt::ret_float(m.underline_position);
    return rt::ret_float(kStdUnderlineTop * s.text_size);
  });
  r.bind(p, "nGetUnderlineThickness", "(J)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    text::FontMetrics m;
    paint_metrics(rt, s, &m, false);
    if (m.flags & text::FontMetrics::kUnderlineThicknessValid) return rt::ret_float(m.underline_thickness);
    return rt::ret_float(kStdUnderlineThickness * s.text_size);
  });
  r.bind(p, "nGetStrikeThruPosition", "(J)F", with_paint([](PaintState& s, NativeArgs&) -> uint64_t {
    return rt::ret_float(kStdStrikeThruTop * s.text_size);
  }));
  r.bind(p, "nGetStrikeThruThickness", "(J)F", with_paint([](PaintState& s, NativeArgs&) -> uint64_t {
    return rt::ret_float(kStdStrikeThruThickness * s.text_size);
  }));

  r.bind(p, "nGetTextAdvances", "(J[CIIIII[FI)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = chars_of(rt, a.h(2));
    int32_t index = a.i32(3), count = a.i32(4), ctx = a.i32(5), ctx_count = a.i32(6);
    if ((index | count | ctx | ctx_count) < 0 || ctx_count < count) throw_aioobe(rt);
    check_range(rt, ctx, ctx_count, int64_t(text.size()));
    std::u16string_view buf = std::u16string_view(text).substr(size_t(ctx), size_t(ctx_count));
    return rt::ret_float(text_advances(rt, s, buf, index - ctx, count, a.i32(7), a.h(8), a.i32(9)));
  });
  r.bind(p, "nGetTextAdvances", "(JLjava/lang/String;IIIII[FI)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = string_of(rt, a.h(2));
    int32_t start = a.i32(3), end = a.i32(4), ctx_start = a.i32(5), ctx_end = a.i32(6);
    if ((start | end | ctx_start | ctx_end) < 0 || ctx_end - ctx_start < end - start) throw_aioobe(rt);
    check_range(rt, ctx_start, ctx_end - ctx_start, int64_t(text.size()));
    std::u16string_view buf = std::u16string_view(text).substr(size_t(ctx_start), size_t(ctx_end - ctx_start));
    return rt::ret_float(text_advances(rt, s, buf, start - ctx_start, end - start, a.i32(7), a.h(8), a.i32(9)));
  });
  // Instance natives: argument 0 is the Paint.
  auto cursor = [](Runtime& rt, const PaintState& s, std::u16string_view text, int32_t start, int32_t count,
                   int32_t dir, int32_t offset, int32_t opt) -> uint64_t {
    check_range(rt, start, count, int64_t(text.size()));
    if (offset < start || offset > start + count) throw_aioobe(rt);
    std::u16string_view buf = text.substr(0, size_t(start + count));
    std::vector<float> adv(size_t(count), 0.0f);
    text::Paint mp = minikin_paint(rt, s);
    text::measure_text(buf, size_t(start), size_t(count), dir == 1 ? text::Bidi::kForceRtl : text::Bidi::kForceLtr, mp,
                       start_hyphen_of(s), end_hyphen_of(s), adv.data(), nullptr, nullptr, run_flag_of(s));
    size_t result = text::text_run_cursor(adv.data(), buf.data(), size_t(start), size_t(count), size_t(offset),
                                          text::CursorOpt(std::clamp(opt, 0, 4)));
    return rt::ret_int(int32_t(result));
  };
  r.bind(p, "nGetTextRunCursor", "(J[CIIIII)I", [cursor](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(1));
    std::u16string text = chars_of(rt, a.h(3));
    return cursor(rt, s, text, a.i32(4), a.i32(5), a.i32(6), a.i32(7), a.i32(8));
  });
  r.bind(p, "nGetTextRunCursor", "(JLjava/lang/String;IIIII)I", [cursor](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(1));
    std::u16string text = string_of(rt, a.h(3));
    return cursor(rt, s, text, a.i32(4), a.i32(5) - a.i32(4), a.i32(6), a.i32(7), a.i32(8));
  });

  auto run_args = [](Runtime& rt, NativeArgs& a, std::u16string& text, int32_t* start, int32_t* count) {
    text = chars_of(rt, a.h(2));
    int32_t s0 = a.i32(3), e0 = a.i32(4), cs = a.i32(5), ce = a.i32(6);
    if (cs < 0 || ce < cs || s0 < cs || e0 < s0 || ce < e0) throw_aioobe(rt);
    check_range(rt, cs, ce - cs, int64_t(text.size()));
    text = text.substr(size_t(cs), size_t(ce - cs));
    *start = s0 - cs;
    *count = e0 - s0;
    return cs;
  };
  r.bind(p, "nGetRunAdvance", "(J[CIIIIZI)F", [run_args](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text;
    int32_t start, count;
    int32_t cs = run_args(rt, a, text, &start, &count);
    int32_t offset = a.i32(8) - cs;
    if (offset < start || offset > start + count) throw_aioobe(rt);
    return rt::ret_float(run_advance(rt, s, text, start, count, a.z(7), offset, kNull, 0, nullptr, nullptr));
  });
  auto character_advance = [run_args](Runtime& rt, NativeArgs& a, bool with_run_info) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text;
    int32_t start, count;
    int32_t cs = run_args(rt, a, text, &start, &count);
    int32_t offset = a.i32(8) - cs;
    if (offset < start || offset > start + count) throw_aioobe(rt);
    Handle bounds_obj = a.h(11);
    text::TextRect bounds;
    uint32_t clusters = 0;
    float result = run_advance(rt, s, text, start, count, a.z(7), offset, a.h(9), a.i32(10),
                               bounds_obj != kNull ? &bounds : nullptr, &clusters);
    if (bounds_obj != kNull) {
      set_float(rt, bounds_obj, "left", bounds.left);
      set_float(rt, bounds_obj, "top", bounds.top);
      set_float(rt, bounds_obj, "right", bounds.right);
      set_float(rt, bounds_obj, "bottom", bounds.bottom);
    }
    if (with_run_info && a.h(12) != kNull) set_int(rt, a.h(12), "mClusterCount", int32_t(clusters));
    return rt::ret_float(result);
  };
  r.bind(p, "nGetRunCharacterAdvance", "(J[CIIIIZI[FILandroid/graphics/RectF;)F",
         [character_advance](Runtime& rt, NativeArgs& a) { return character_advance(rt, a, false); });
  r.bind(p, "nGetRunCharacterAdvance", "(J[CIIIIZI[FILandroid/graphics/RectF;Landroid/graphics/Paint$RunInfo;)F",
         [character_advance](Runtime& rt, NativeArgs& a) { return character_advance(rt, a, true); });
  r.bind(p, "nGetOffsetForAdvance", "(J[CIIIIZF)I", [run_args](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text;
    int32_t start, count;
    int32_t cs = run_args(rt, a, text, &start, &count);
    std::vector<float> adv(size_t(count), 0.0f);
    text::Paint mp = minikin_paint(rt, s);
    text::measure_text(text, size_t(start), size_t(count), a.z(7) ? text::Bidi::kForceRtl : text::Bidi::kForceLtr, mp,
                       start_hyphen_of(s), end_hyphen_of(s), adv.data(), nullptr, nullptr, run_flag_of(s));
    size_t offset = text::offset_for_advance(adv.data(), text.data(), size_t(start), size_t(count), a.f32(8));
    return rt::ret_int(int32_t(offset) + cs);
  });
  r.bind(p, "nGetFontMetricsIntForText", "(J[CIIIIZLandroid/graphics/Paint$FontMetricsInt;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           PaintState s = snapshot(rt, a.i64(0));
           std::u16string text = chars_of(rt, a.h(2));
           int32_t start = a.i32(3), count = a.i32(4), ctx = a.i32(5), ctx_count = a.i32(6);
           check_range(rt, ctx, ctx_count, int64_t(text.size()));
           if (start < ctx || count < 0 || start + count > ctx + ctx_count) throw_aioobe(rt);
           std::u16string_view buf = std::u16string_view(text).substr(size_t(ctx), size_t(ctx_count));
           font_extent_int(rt, s, buf, start - ctx, count, a.z(7), a.h(8));
           return 0;
         });
  r.bind(p, "nGetFontMetricsIntForText", "(JLjava/lang/String;IIIIZLandroid/graphics/Paint$FontMetricsInt;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           PaintState s = snapshot(rt, a.i64(0));
           std::u16string text = string_of(rt, a.h(2));
           int32_t start = a.i32(3), count = a.i32(4), ctx = a.i32(5), ctx_count = a.i32(6);
           check_range(rt, ctx, ctx_count, int64_t(text.size()));
           if (start < ctx || count < 0 || start + count > ctx + ctx_count) throw_aioobe(rt);
           std::u16string_view buf = std::u16string_view(text).substr(size_t(ctx), size_t(ctx_count));
           font_extent_int(rt, s, buf, start - ctx, count, a.z(7), a.h(8));
           return 0;
         });
  r.bind(p, "nHasGlyph", "(JILjava/lang/String;)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string str = string_of(rt, a.h(3));
    return rt::ret_bool(has_glyph(rt, s, a.i32(2), str));
  });
  r.bind(p, "nGetStringBounds", "(JLjava/lang/String;IIILandroid/graphics/Rect;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = string_of(rt, a.h(2));
    int32_t start = a.i32(3), end = a.i32(4);
    check_range(rt, start, end - start, int64_t(text.size()));
    text_bounds(rt, s, std::u16string_view(text).substr(size_t(start), size_t(end - start)), a.i32(5), a.h(6));
    return 0;
  });
  r.bind(p, "nGetCharArrayBounds", "(J[CIIILandroid/graphics/Rect;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = chars_of(rt, a.h(2));
    int32_t index = a.i32(3), count = a.i32(4);
    check_range(rt, index, count, int64_t(text.size()));
    text_bounds(rt, s, std::u16string_view(text).substr(size_t(index), size_t(count)), a.i32(5), a.h(6));
    return 0;
  });
  r.bind(p, "nBreakText", "(J[CIIFI[F)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = chars_of(rt, a.h(2));
    int32_t index = a.i32(3), count = a.i32(4);
    bool forward = count >= 0;
    if (!forward) count = -count;
    if (index < 0 || int64_t(index) + count > int64_t(text.size())) throw_aioobe(rt);
    return rt::ret_int(break_text(rt, s, std::u16string_view(text).substr(size_t(index), size_t(count)), a.f32(5),
                                  a.i32(6), a.h(7), forward));
  });
  r.bind(p, "nBreakText", "(JLjava/lang/String;ZFI[F)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    PaintState s = snapshot(rt, a.i64(0));
    std::u16string text = string_of(rt, a.h(2));
    return rt::ret_int(break_text(rt, s, text, a.f32(4), a.i32(5), a.h(6), a.z(3)));
  });
}

}  // namespace

text::Paint minikin_paint_of(Runtime& rt, int64_t paint) { return minikin_paint(rt, snapshot(rt, paint)); }

uint32_t paint_locale_list_id(Runtime& rt, int64_t paint) { return uint32_t(snapshot(rt, paint).locale_list_id); }

int32_t SkiaPaint::java_flags() const {
  int32_t flags = paint.isAntiAlias() ? kAntiAlias : 0;
  if (paint.isDither()) flags |= kDither;
  if (filter_bitmap) flags |= kFilterBitmap;
  if (underline) flags |= kUnderline;
  if (strike_thru) flags |= kStrikeThru;
  if (font.isEmbolden()) flags |= kFakeBold;
  if (font.isLinearMetrics()) flags |= kLinearText;
  if (font.isSubpixel()) flags |= kSubpixelText;
  if (font.isEmbeddedBitmaps()) flags |= kEmbeddedBitmapText;
  if (font.isForceAutoHinting()) flags |= kForceAutoHinting;
  return flags;
}

void SkiaPaint::set_java_flags(int32_t flags) {
  paint.setAntiAlias((flags & kAntiAlias) != 0);
  paint.setDither((flags & kDither) != 0);
  font.setEmbolden((flags & kFakeBold) != 0);
  font.setLinearMetrics((flags & kLinearText) != 0);
  font.setSubpixel((flags & kSubpixelText) != 0);
  font.setEmbeddedBitmaps((flags & kEmbeddedBitmapText) != 0);
  font.setForceAutoHinting((flags & kForceAutoHinting) != 0);
  font.setEdging((flags & kAntiAlias) ? SkFont::Edging::kAntiAlias : SkFont::Edging::kAlias);
  filter_bitmap = (flags & kFilterBitmap) != 0;
  underline = (flags & kUnderline) != 0;
  strike_thru = (flags & kStrikeThru) != 0;
}

SkiaPaint skia_paint_of(Runtime& rt, int64_t handle, bool with_text) {
  SkiaPaint p;
  if (handle == 0) return p;
  PaintState s = snapshot(rt, handle);
  if (s.wide_color) {
    p.paint.setColor(color4f_of_long(s.color_long), skia_color_space(rt, s.color_space).get());
  } else {
    p.paint.setColor(SkColor(uint32_t(s.color)));
  }
  // Paint.Style, Cap and Join use Skia's order; out of range values are
  // refused by Java's enums.
  p.paint.setStyle(SkPaint::Style(std::clamp(s.style, 0, 2)));
  p.paint.setStrokeCap(SkPaint::Cap(std::clamp(s.cap, 0, 2)));
  p.paint.setStrokeJoin(SkPaint::Join(std::clamp(s.join, 0, 2)));
  p.paint.setStrokeWidth(s.stroke_width);  // negative: ignored, as SkPaint
  p.paint.setStrokeMiter(s.stroke_miter);
  if (s.xfermode == 0) p.paint.setBlendMode(SkBlendMode(std::clamp(s.blend_mode, 0, int(SkBlendMode::kLastMode))));
  p.paint.setShader(s.shader);
  p.paint.setColorFilter(s.color_filter);
  p.paint.setMaskFilter(s.mask_filter);
  p.paint.setPathEffect(s.path_effect);
  p.set_java_flags(s.flags);
  if (s.text_size >= 0) p.font.setSize(s.text_size);
  p.font.setScaleX(s.text_scale_x);
  p.font.setSkewX(s.text_skew_x);
  p.font.setHinting(s.hinting == 0 ? SkFontHinting::kNone : SkFontHinting::kNormal);
  p.align = s.align;
  // Paint.setShadowLayer: BlurDrawLooper::Make, Blur::convertRadiusToSigma.
  if (s.shadow_radius > 0) {
    ShadowLooper l;
    l.color = color4f_of_long(s.shadow_color);
    if (sk_sp<SkColorSpace> cs = skia_color_space(rt, s.shadow_color_space)) {
      SkPaint tmp;
      tmp.setColor(l.color, cs.get());  // converted to sRGB
      l.color = tmp.getColor4f();
    }
    l.sigma = 0.57735f * s.shadow_radius + 0.5f;
    l.offset = {s.shadow_dx, s.shadow_dy};
    p.looper = l;
  }
  if (with_text) {
    p.minikin = minikin_paint(rt, s);
    p.start_hyphen = start_hyphen_of(s);
    p.end_hyphen = end_hyphen_of(s);
    p.run_flag = run_flag_of(s);
  }
  return p;
}

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

  // Effects: the paint takes a reference to the Skia object and, as HWUI,
  // returns the handle (0 clears).
  auto set_effect = [&r, p](const char* name, auto resolve, auto field) {
    r.bind(p, name, "(JJ)J", [resolve, field](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto value = resolve(rt, a.i64(2));
      auto pt = paint(rt, a.i64(0));
      std::lock_guard<std::mutex> guard(pt->lock);
      pt->s.*field = std::move(value);
      return rt::ret_long(pt->s.*field ? a.i64(2) : 0);
    });
  };
  set_effect("nSetShader", skia_shader, &PaintState::shader);
  set_effect("nSetColorFilter", skia_color_filter, &PaintState::color_filter);
  set_effect("nSetMaskFilter", skia_mask_filter, &PaintState::mask_filter);
  set_effect("nSetPathEffect", skia_path_effect, &PaintState::path_effect);
  r.bind(p, "nSetXfermode", "(JI)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.blend_mode = a.i32(2);
    s.xfermode = 0;
    return 0;
  }));
  r.bind(p, "nSetXfermode", "(JJ)V", with_paint([](PaintState& s, NativeArgs& a) -> uint64_t {
    s.xfermode = a.i64(2);
    return 0;
  }), S::kPartial, "RuntimeXfermode (AGSL) non appliqué : mélange SRC_OVER");
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

  register_paint_text(r);
}

}  // namespace aa::bridge
