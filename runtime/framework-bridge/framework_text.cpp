// android.graphics.text.MeasuredText, MeasuredText$Builder and LineBreaker
// natives (G3, ADR-006 J5, ADR-007), each signature checked against the
// framework.jar of Android 17: HWUI's jni/text/MeasuredText.cpp and
// LineBreaker.cpp over the text engine's port of minikin.
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/internal.h"
#include "text/line_breaker.h"
#include "text/measured_text.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

struct Builder {
  std::mutex lock;
  std::vector<text::TextRun> runs;
};

struct Measured {
  std::unique_ptr<text::MeasuredText> text;
};

// minikin::android::StaticLayoutNative.
struct LineBreakerConfig {
  text::BreakStrategy strategy;
  text::HyphenationFrequency frequency;
  bool justified;
  std::vector<float> indents;
  bool use_bounds_for_width;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<Builder> builders{0x111A};
  HandleTable<Measured> measured{0x111B};
  HandleTable<LineBreakerConfig> breakers{0x111C};
  HandleTable<text::LineBreakResult> results{0x111D};
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: handles may be freed at exit
  return *t;
}

template <typename T>
std::shared_ptr<T> lookup(Runtime& rt, const HandleTable<T>& table, int64_t h, const char* what) {
  auto v = table.get(h);
  if (!v) rt.throw_new("Ljava/lang/IllegalStateException;", std::string("[AnyAndroid] ") + what + " invalide");
  return v;
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

std::vector<float> floats_of(Runtime& rt, Handle array) {
  std::vector<float> out;
  if (array == kNull) return out;
  rt::ArrayObject& a = rt.array(array, "float[]");
  if (a.width != 4) rt.throw_new("Ljava/lang/IllegalArgumentException;", "float[] attendu");
  out.resize(a.length);
  std::memcpy(out.data(), a.data.data(), size_t(a.length) * 4);
  return out;
}

// jintArrayToFloatVector.
std::vector<float> int_array_as_floats(Runtime& rt, Handle array) {
  std::vector<float> out;
  if (array == kNull) return out;
  rt::ArrayObject& a = rt.array(array, "int[]");
  if (a.width != 4) rt.throw_new("Ljava/lang/IllegalArgumentException;", "int[] attendu");
  for (uint32_t i = 0; i < a.length; ++i) {
    int32_t v;
    std::memcpy(&v, a.data.data() + size_t(i) * 4, 4);
    out.push_back(float(v));
  }
  return out;
}

void set_int(Runtime& rt, Handle obj, const char* name, int32_t v) {
  rt::Object& o = rt.nonnull(obj, name);
  rt::Field* f = find_field(o.klass, name, "I");
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
  o.fields.at(f->slot) = uint32_t(v);
}

// Skia's saturating float to int conversions.
int32_t saturate(float x) {
  if (!(x == x)) return 0;
  return int32_t(std::fmin(std::fmax(x, -2147483520.0f), 2147483520.0f));
}

// The text given back to a query: the one measured (Java passes the same
// array), and a range within it.
std::u16string query_text(Runtime& rt, const text::MeasuredText& mt, Handle array, int32_t start, int32_t end) {
  std::u16string s = chars_of(rt, array);
  if (s.size() != mt.widths.size()) rt.throw_new("Ljava/lang/IllegalArgumentException;", "texte différent de la mesure");
  if (start < 0 || start > end || size_t(end) > s.size()) throw_aioobe(rt);
  return s;
}

void register_measured_text_builder(rt::NativeRegistry& r) {
  const char* b = "Landroid/graphics/text/MeasuredText$Builder;";
  r.bind(b, "nInitBuilder", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(tables().builders.add(std::make_shared<Builder>()));
  });
  r.bind(b, "nAddStyleRun", "(JJIIZIIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto builder = lookup(rt, tables().builders, a.i64(0), "MeasuredText.Builder");
    int32_t start = a.i32(7), end = a.i32(8);
    if (start < 0 || start > end) throw_aioobe(rt);
    text::Paint paint = minikin_paint_of(rt, a.i64(2));
    std::lock_guard<std::mutex> guard(builder->lock);
    builder->runs.push_back(text::TextRun::style(uint32_t(start), uint32_t(end), std::move(paint), a.i32(4), a.i32(5),
                                                 a.z(6), a.z(9)));
    return 0;
  });
  r.bind(b, "nAddReplacementRun", "(JJIIF)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto builder = lookup(rt, tables().builders, a.i64(0), "MeasuredText.Builder");
    int32_t start = a.i32(4), end = a.i32(5);
    if (start < 0 || start > end) throw_aioobe(rt);
    uint32_t locales = paint_locale_list_id(rt, a.i64(2));
    std::lock_guard<std::mutex> guard(builder->lock);
    builder->runs.push_back(text::TextRun::replacement(uint32_t(start), uint32_t(end), a.f32(6), locales));
    return 0;
  });
  r.bind(b, "nBuildMeasuredText", "(JJ[CZZZZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto builder = lookup(rt, tables().builders, a.i64(0), "MeasuredText.Builder");
    std::u16string s = chars_of(rt, a.h(4));
    std::vector<text::TextRun> runs;
    {
      std::lock_guard<std::mutex> guard(builder->lock);
      runs = std::move(builder->runs);  // minikin moves the runs out
      builder->runs.clear();
    }
    for (const text::TextRun& run : runs) {
      if (run.end > s.size()) throw_aioobe(rt);
    }
    ensure_text_services(rt);
    // The hint (a.i64(2)) and computeLayout (a.z(6)) only save work in
    // minikin; the engine's layout cache does the same.
    auto measured = std::make_shared<Measured>();
    measured->text = text::MeasuredText::build(s, std::move(runs), a.z(5), a.z(7), a.z(8));
    return rt::ret_long(tables().measured.add(std::move(measured)));
  });
  r.bind(b, "nFreeBuilder", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().builders.remove(a.i64(0));
    return 0;
  });
}

void register_measured_text(rt::NativeRegistry& r) {
  const char* m = "Landroid/graphics/text/MeasuredText;";
  static const int64_t measured_free = register_free_function([](int64_t h) { tables().measured.remove(h); });
  r.bind(m, "nGetReleaseFunc", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(measured_free); });
  r.bind(m, "nGetWidth", "(JII)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto mt = lookup(rt, tables().measured, a.i64(0), "MeasuredText");
    int32_t start = a.i32(2), end = a.i32(3);
    if (start < 0 || size_t(std::max(start, end)) > mt->text->widths.size()) throw_aioobe(rt);
    float width = 0.0f;
    for (int32_t i = start; i < end; ++i) width += mt->text->widths[size_t(i)];
    return rt::ret_float(width);
  });
  r.bind(m, "nGetCharWidthAt", "(JI)F", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto mt = lookup(rt, tables().measured, a.i64(0), "MeasuredText");
    int32_t offset = a.i32(2);
    if (offset < 0 || size_t(offset) >= mt->text->widths.size()) throw_aioobe(rt);
    return rt::ret_float(mt->text->widths[size_t(offset)]);
  });
  r.bind(m, "nGetMemoryUsage", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto mt = lookup(rt, tables().measured, a.i64(0), "MeasuredText");
    return rt::ret_int(int32_t(mt->text->memory_usage()));
  });
  r.bind(m, "nGetBounds", "(J[CIILandroid/graphics/Rect;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto mt = lookup(rt, tables().measured, a.i64(0), "MeasuredText");
    int32_t start = a.i32(3), end = a.i32(4);
    std::u16string s = query_text(rt, *mt->text, a.h(2), start, end);
    ensure_text_services(rt);
    text::TextRect b = mt->text->bounds(s, size_t(start), size_t(end));
    // SkRect::roundOut, then GraphicsJNI::irect_to_jrect.
    Handle rect = a.h(5);
    set_int(rt, rect, "left", saturate(std::floor(b.left)));
    set_int(rt, rect, "top", saturate(std::floor(b.top)));
    set_int(rt, rect, "right", saturate(std::ceil(b.right)));
    set_int(rt, rect, "bottom", saturate(std::ceil(b.bottom)));
    return 0;
  });
  r.bind(m, "nGetExtent", "(J[CII)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto mt = lookup(rt, tables().measured, a.i64(0), "MeasuredText");
    int32_t start = a.i32(3), end = a.i32(4);
    std::u16string s = query_text(rt, *mt->text, a.h(2), start, end);
    ensure_text_services(rt);
    text::Extent e = mt->text->extent(s, size_t(start), size_t(end));
    int32_t ascent = saturate(std::floor(e.ascent + 0.5f)), descent = saturate(std::floor(e.descent + 0.5f));
    // As HWUI: the descent is sign-extended over the ascent's bits.
    return rt::ret_long((int64_t(ascent) << 32) | int64_t(descent));
  });
}

void register_line_breaker(rt::NativeRegistry& r) {
  const char* lb = "Landroid/graphics/text/LineBreaker;";
  static const int64_t breaker_free = register_free_function([](int64_t h) { tables().breakers.remove(h); });
  static const int64_t result_free = register_free_function([](int64_t h) { tables().results.remove(h); });
  r.bind(lb, "nGetReleaseFunc", "()J", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(breaker_free); });
  r.bind(lb, "nGetReleaseResultFunc", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(result_free); });
  r.bind(lb, "nInit", "(IIZ[IZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto config = std::make_shared<LineBreakerConfig>(LineBreakerConfig{
        text::BreakStrategy(a.i32(0)), text::HyphenationFrequency(a.i32(1)), a.z(2), int_array_as_floats(rt, a.h(3)),
        a.z(4)});
    return rt::ret_long(tables().breakers.add(std::move(config)));
  });
  r.bind(lb, "nComputeLineBreaks", "(J[CJIFIF[FFI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto config = lookup(rt, tables().breakers, a.i64(0), "LineBreaker");
    std::u16string s = chars_of(rt, a.h(2));
    auto mt = lookup(rt, tables().measured, a.i64(3), "MeasuredText");
    int32_t length = a.i32(5);
    if (length < 0 || size_t(length) > s.size() || size_t(length) > mt->text->widths.size()) throw_aioobe(rt);
    for (const text::TextRun& run : mt->text->runs) {
      if (run.end > uint32_t(length)) rt.throw_new("Ljava/lang/IllegalArgumentException;", "run hors du texte");
    }
    text::LineWidths widths;
    widths.first_width = a.f32(6);
    widths.first_line_count = a.i32(7);
    widths.rest_width = a.f32(8);
    widths.indents = config->indents;
    widths.indents_offset = a.i32(11);
    text::TabStops tabs{floats_of(rt, a.h(9)), a.f32(10)};
    ensure_text_services(rt);
    auto result = std::make_shared<text::LineBreakResult>(
        text::break_into_lines(std::u16string_view(s).substr(0, size_t(length)), config->strategy, config->frequency,
                               config->justified, *mt->text, widths, tabs, config->use_bounds_for_width));
    return rt::ret_long(tables().results.add(std::move(result)));
  });
  r.bind(lb, "nGetLineCount", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto res = lookup(rt, tables().results, a.i64(0), "LineBreaker.Result");
    return rt::ret_int(int32_t(res->break_points.size()));
  });
  // The per-line getters: index checked, as the vectors HWUI reads.
  auto line = [](auto get) {
    return [get](Runtime& rt, NativeArgs& a) -> uint64_t {
      auto res = lookup(rt, tables().results, a.i64(0), "LineBreaker.Result");
      int32_t i = a.i32(2);
      if (i < 0 || size_t(i) >= res->break_points.size()) throw_aioobe(rt);
      return get(*res, size_t(i));
    };
  };
  r.bind(lb, "nGetLineBreakOffset", "(JI)I",
         line([](const text::LineBreakResult& res, size_t i) { return rt::ret_int(res.break_points[i]); }));
  r.bind(lb, "nGetLineWidth", "(JI)F",
         line([](const text::LineBreakResult& res, size_t i) { return rt::ret_float(res.widths[i]); }));
  r.bind(lb, "nGetLineAscent", "(JI)F",
         line([](const text::LineBreakResult& res, size_t i) { return rt::ret_float(res.ascents[i]); }));
  r.bind(lb, "nGetLineDescent", "(JI)F",
         line([](const text::LineBreakResult& res, size_t i) { return rt::ret_float(res.descents[i]); }));
  r.bind(lb, "nGetLineFlag", "(JI)I",
         line([](const text::LineBreakResult& res, size_t i) { return rt::ret_int(res.flags[i]); }));
}

}  // namespace

void register_framework_text(rt::NativeRegistry& r) {
  register_measured_text_builder(r);
  register_measured_text(r);
  register_line_breaker(r);
}

}  // namespace aa::bridge
