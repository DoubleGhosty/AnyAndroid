// Graphics natives of framework.jar (G3): Path's SkPath model, Paint's
// property store and the style rules of fonts and typefaces (HWUI, minikin).
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/native_memory.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kBootClasses[] = {
    R"(
.class public Landroid/graphics/RectF;
.super Ljava/lang/Object;
.field public left:F
.field public top:F
.field public right:F
.field public bottom:F
)",
    R"(
.class public Ljava/nio/Buffer;
.super Ljava/lang/Object;
.field address:J
.field capacity:I
)",
    R"(
.class public Landroid/graphics/Typeface;
.super Ljava/lang/Object;
.field public static final DEFAULT:Landroid/graphics/Typeface;
)",
};

struct Vm {
  std::unique_ptr<dex::DexFile> boot;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;

  Vm() {
    std::vector<tool::ClassDef> b;
    for (const char* s : kBootClasses) b.push_back(tool::assemble_smali(s, "boot.smali"));
    boot = dex::DexFile::open(tool::write_dex(b), "core-test.jar!classes.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_boot_dex(boot.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
  }
  ~Vm() {
    scope.reset();
    rt.reset();
  }
};

// Native call arguments, in 32-bit words as the interpreter passes them.
struct Args {
  std::vector<uint32_t> w;
  Args& j(int64_t v) {
    w.push_back(uint32_t(v));
    w.push_back(uint32_t(uint64_t(v) >> 32));
    return *this;
  }
  Args& i(int32_t v) {
    w.push_back(uint32_t(v));
    return *this;
  }
  Args& f(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    w.push_back(bits);
    return *this;
  }
  Args& h(rt::Handle v) {
    w.push_back(v);
    return *this;
  }
};

uint64_t call(rt::Runtime& rt, const char* cls, const char* name, const char* sig, Args args = {}) {
  const rt::NativeBinding* b = rt.registry().binding(cls, name, sig);
  CHECK(b != nullptr);
  rt::NativeArgs a(rt, args.w.data(), uint32_t(args.w.size()));
  return b->fn(rt, a);
}

template <typename F>
std::string thrown(rt::Runtime& rt, F&& f) {
  try {
    f();
  } catch (const rt::JavaThrow& t) {
    return rt.class_of(t.exception)->java_name();
  }
  return "";
}

const char* kPath = "Landroid/graphics/Path;";

struct Contents {
  std::vector<uint8_t> verbs;
  std::vector<float> coords, weights;
};

Contents contents(rt::Runtime& rt, int64_t path) {
  Contents c;
  bridge::path_contents(rt, path, c.verbs, c.coords, c.weights);
  return c;
}

enum : uint8_t { kMove, kLine, kQuad, kConic, kCubic, kClose };

float float_field(rt::Runtime& rt, rt::Handle h, const char* name) {
  rt::Object& o = rt.nonnull(h, "o");
  rt::Field* f = o.klass->declared_field(name, "F");
  float v;
  uint32_t bits = o.fields.at(f->slot);
  std::memcpy(&v, &bits, 4);
  return v;
}

}  // namespace

TEST(graphics_path_contours_follow_skpath) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  int64_t p = int64_t(call(rt, kPath, "nInit", "()J"));
  CHECK_EQ(call(rt, kPath, "nIsEmpty", "(J)Z", Args().j(p)), uint64_t(1));
  CHECK_EQ(call(rt, kPath, "nGetGenerationID", "(J)I", Args().j(p)), uint64_t(1));  // empty paths share id 1
  // A lineTo without moveTo starts at (0, 0).
  call(rt, kPath, "nLineTo", "(JFF)V", Args().j(p).f(10).f(0));
  call(rt, kPath, "nRLineTo", "(JFF)V", Args().j(p).f(0).f(5));
  call(rt, kPath, "nClose", "(J)V", Args().j(p));
  call(rt, kPath, "nClose", "(J)V", Args().j(p));  // a second close adds nothing
  // After a close, the next verb starts from the contour's start point.
  call(rt, kPath, "nRLineTo", "(JFF)V", Args().j(p).f(1).f(1));
  Contents c = contents(rt, p);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine, kLine, kClose, kMove, kLine}));
  CHECK(c.coords == (std::vector<float>{0, 0, 10, 0, 10, 5, 0, 0, 1, 1}));
  CHECK(call(rt, kPath, "nGetGenerationID", "(J)I", Args().j(p)) > 1);

  // Conics: weight 1 is a quad, a non-positive weight a line.
  int64_t q = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nConicTo", "(JFFFFF)V", Args().j(q).f(1).f(1).f(2).f(0).f(1));
  call(rt, kPath, "nConicTo", "(JFFFFF)V", Args().j(q).f(3).f(1).f(4).f(0).f(0));
  call(rt, kPath, "nConicTo", "(JFFFFF)V", Args().j(q).f(5).f(1).f(6).f(0).f(0.5f));
  c = contents(rt, q);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kQuad, kLine, kConic}));
  CHECK(c.weights == std::vector<float>{0.5f});

  // Bounds include the control points.
  rt::GlobalRef rect(rt, rt.alloc(rt.find_class("Landroid/graphics/RectF;")));
  call(rt, kPath, "nComputeBounds", "(JLandroid/graphics/RectF;)V", Args().j(q).h(rect.get()));
  CHECK_EQ(float_field(rt, rect.get(), "left"), 0.0f);
  CHECK_EQ(float_field(rt, rect.get(), "top"), 0.0f);
  CHECK_EQ(float_field(rt, rect.get(), "right"), 6.0f);
  CHECK_EQ(float_field(rt, rect.get(), "bottom"), 1.0f);

  // A copy shares the generation until edited; reset forgets the fill type.
  uint64_t gen = call(rt, kPath, "nGetGenerationID", "(J)I", Args().j(q));
  int64_t copy = int64_t(call(rt, kPath, "nInit", "(J)J", Args().j(q)));
  CHECK_EQ(call(rt, kPath, "nGetGenerationID", "(J)I", Args().j(copy)), gen);
  call(rt, kPath, "nSetFillType", "(JI)V", Args().j(copy).i(1));
  call(rt, kPath, "nOffset", "(JFF)V", Args().j(copy).f(1).f(1));
  CHECK(call(rt, kPath, "nGetGenerationID", "(J)I", Args().j(copy)) != gen);
  CHECK_EQ(contents(rt, copy).coords[2], 2.0f);
  CHECK_EQ(contents(rt, q).coords[2], 1.0f);
  call(rt, kPath, "nReset", "(J)V", Args().j(copy));
  CHECK_EQ(call(rt, kPath, "nGetFillType", "(J)I", Args().j(copy)), uint64_t(0));
  CHECK_EQ(call(rt, kPath, "nIsEmpty", "(J)Z", Args().j(copy)), uint64_t(1));

  // Adding a path to itself doubles it.
  call(rt, kPath, "nAddPath", "(JJFF)V", Args().j(p).j(p).f(100).f(0));
  c = contents(rt, p);
  CHECK_EQ(c.verbs.size(), size_t(12));
  CHECK_EQ(c.coords[10], 100.0f);
  CHECK_EQ(thrown(rt, [&] { call(rt, kPath, "nIsEmpty", "(J)Z", Args().j(0x1234)); }),
           std::string("java.lang.IllegalStateException"));
}

TEST(graphics_path_shapes_follow_skpath) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  int64_t p = int64_t(call(rt, kPath, "nInit", "()J"));
  // Rectangles start at the top left corner, clockwise (0) or not (1).
  call(rt, kPath, "nAddRect", "(JFFFFI)V", Args().j(p).f(0).f(0).f(4).f(2).i(0));
  call(rt, kPath, "nAddRect", "(JFFFFI)V", Args().j(p).f(0).f(0).f(4).f(2).i(1));
  Contents c = contents(rt, p);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine, kLine, kLine, kClose, kMove, kLine, kLine, kLine, kClose}));
  CHECK(c.coords == (std::vector<float>{0, 0, 4, 0, 4, 2, 0, 2, 0, 0, 0, 2, 4, 2, 4, 0}));

  // Ovals: four conics from the middle of the right side.
  int64_t o = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nAddOval", "(JFFFFI)V", Args().j(o).f(0).f(0).f(4).f(2).i(0));
  c = contents(rt, o);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kConic, kConic, kConic, kConic, kClose}));
  CHECK(c.coords ==
        (std::vector<float>{4, 1, 4, 2, 2, 2, 0, 2, 0, 1, 0, 0, 2, 0, 4, 0, 4, 1}));
  CHECK_EQ(c.weights.size(), size_t(4));
  CHECK(std::fabs(c.weights[0] - 0.70710678f) < 1e-6f);
  // A circle of non-positive radius adds nothing.
  call(rt, kPath, "nAddCircle", "(JFFFI)V", Args().j(o).f(1).f(1).f(0).i(0));
  CHECK_EQ(contents(rt, o).verbs.size(), size_t(6));

  // Rounded rectangles: radii larger than the sides make an oval; small ones
  // a line then a conic per corner, from the left side.
  int64_t rr = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nAddRoundRect", "(JFFFFFFI)V", Args().j(rr).f(0).f(0).f(4).f(4).f(5).f(5).i(0));
  CHECK_EQ(contents(rt, rr).verbs.size(), size_t(6));
  call(rt, kPath, "nReset", "(J)V", Args().j(rr));
  call(rt, kPath, "nAddRoundRect", "(JFFFFFFI)V", Args().j(rr).f(0).f(0).f(10).f(10).f(1).f(2).i(0));
  c = contents(rt, rr);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine, kConic, kLine, kConic, kLine, kConic, kLine, kConic, kClose}));
  CHECK_EQ(c.coords[0], 0.0f);  // (left, bottom - ry)
  CHECK_EQ(c.coords[1], 8.0f);
  CHECK_EQ(c.coords[2], 0.0f);  // up the left side to (left, top + ry)
  CHECK_EQ(c.coords[3], 2.0f);
  CHECK_EQ(c.coords[4], 0.0f);  // control point: the top left corner
  CHECK_EQ(c.coords[5], 0.0f);
  CHECK_EQ(c.coords[6], 1.0f);  // (left + rx, top)
  CHECK_EQ(c.coords[7], 0.0f);
  // Negative radii: nothing; zero radii: a rectangle.
  call(rt, kPath, "nReset", "(J)V", Args().j(rr));
  call(rt, kPath, "nAddRoundRect", "(JFFFFFFI)V", Args().j(rr).f(0).f(0).f(10).f(10).f(-1).f(2).i(0));
  CHECK_EQ(call(rt, kPath, "nIsEmpty", "(J)Z", Args().j(rr)), uint64_t(1));
  // Per-corner radii are scaled down together when they overlap (CSS rule):
  // 8 + 8 on a side of 10 scales all radii by 10/16.
  rt::GlobalRef radii(rt, rt.new_array("[F", 8));
  float values[8] = {8, 8, 8, 8, 8, 8, 0, 0};
  std::memcpy(rt.array(radii.get(), "f").data.data(), values, sizeof values);
  call(rt, kPath, "nAddRoundRect", "(JFFFF[FI)V", Args().j(rr).f(0).f(0).f(10).f(10).h(radii.get()).i(0));
  c = contents(rt, rr);
  CHECK(c.verbs.size() == 10);
  CHECK_EQ(c.coords[6], 5.0f);  // left + 8 * 10/16
  rt::GlobalRef short_radii(rt, rt.new_array("[F", 7));
  CHECK_EQ(thrown(rt, [&] {
             call(rt, kPath, "nAddRoundRect", "(JFFFF[FI)V", Args().j(rr).f(0).f(0).f(1).f(1).h(short_radii.get()).i(0));
           }),
           std::string("java.lang.ArrayIndexOutOfBoundsException"));
}

// Path.approximate: [fraction, x, y] triplets, as HWUI computes them.
TEST(graphics_path_approximate) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  auto approximate = [&](int64_t p, float error) {
    rt::Handle a = rt::Handle(call(rt, kPath, "nApproximate", "(JF)[F", Args().j(p).f(error)));
    rt::ArrayObject& arr = rt.array(a, "f");
    std::vector<float> v(arr.length);
    std::memcpy(v.data(), arr.data.data(), arr.length * 4);
    return v;
  };
  // A lone moveTo: two copies of its point.
  int64_t p = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nMoveTo", "(JFF)V", Args().j(p).f(5).f(5));
  CHECK(approximate(p, 0.5f) == (std::vector<float>{0, 5, 5, 1, 5, 5}));
  // Lines: fractions of the total length; a close goes back to the start.
  call(rt, kPath, "nLineTo", "(JFF)V", Args().j(p).f(8).f(9));
  call(rt, kPath, "nClose", "(J)V", Args().j(p));
  CHECK(approximate(p, 0.5f) == (std::vector<float>{0, 5, 5, 0.5f, 8, 9, 1, 5, 5}));
  // The cubic of PathInterpolator(0.4, 0, 0.2, 1): ends exact, increasing
  // fractions, every chord within the error of the curve's middle.
  int64_t c = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nMoveTo", "(JFF)V", Args().j(c).f(0).f(0));
  call(rt, kPath, "nCubicTo", "(JFFFFFF)V", Args().j(c).f(0.4f).f(0).f(0.2f).f(1).f(1).f(1));
  std::vector<float> v = approximate(c, 0.002f);
  CHECK(v.size() % 3 == 0 && v.size() > 30);
  CHECK_EQ(v[0], 0.0f);
  CHECK_EQ(v[v.size() - 3], 1.0f);
  CHECK_EQ(v[v.size() - 2], 1.0f);
  CHECK_EQ(v[v.size() - 1], 1.0f);
  for (size_t i = 3; i < v.size(); i += 3) CHECK(v[i] > v[i - 3]);
  // An oval (conics) is approximated too, ending where it started.
  int64_t o = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nAddOval", "(JFFFFI)V", Args().j(o).f(0).f(0).f(2).f(2).i(0));
  v = approximate(o, 0.01f);
  CHECK(v.size() > 12);
  CHECK_EQ(v[1], 2.0f);
  CHECK_EQ(v[v.size() - 2], 2.0f);
  CHECK(std::fabs(v[v.size() - 1] - 1.0f) < 1e-6f);
}

TEST(graphics_paint_properties) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* paint = "Landroid/graphics/Paint;";
  int64_t p = int64_t(call(rt, paint, "nInit", "()J"));
  // SkFont's and SkPaint's defaults.
  float size;
  uint32_t bits = uint32_t(call(rt, paint, "nGetTextSize", "(J)F", Args().j(p)));
  std::memcpy(&size, &bits, 4);
  CHECK_EQ(size, 12.0f);
  CHECK_EQ(call(rt, paint, "nGetStrokeMiter", "(J)F", Args().j(p)), uint64_t(0x40800000));  // 4.0f
  call(rt, paint, "nSetAntiAlias", "(JZ)V", Args().j(p).i(1));
  call(rt, paint, "nSetFakeBoldText", "(JZ)V", Args().j(p).i(1));
  call(rt, paint, "nSetAntiAlias", "(JZ)V", Args().j(p).i(0));
  CHECK_EQ(call(rt, paint, "nGetFlags", "(J)I", Args().j(p)), uint64_t(0x20));
  // Locale lists: the empty list is 0, the same tags the same id.
  int32_t en = int32_t(call(rt, paint, "nSetTextLocales", "(JLjava/lang/String;)I", Args().j(p).h(rt.new_string_utf8("en-US"))));
  CHECK(en > 0);
  CHECK_EQ(int32_t(call(rt, paint, "nSetTextLocales", "(JLjava/lang/String;)I", Args().j(p).h(rt.new_string_utf8("en-US")))), en);
  CHECK_EQ(int32_t(call(rt, paint, "nSetTextLocales", "(JLjava/lang/String;)I", Args().j(p).h(rt.new_string_utf8("")))), 0);
  CHECK_EQ(thrown(rt, [&] { call(rt, paint, "nSetTextLocalesByMinikinLocaleListId", "(JI)V", Args().j(p).i(1 << 20)); }),
           std::string("java.lang.IllegalArgumentException"));
  // A copy measures text the same until a text property changes.
  int64_t q = int64_t(call(rt, paint, "nInitWithPaint", "(J)J", Args().j(p)));
  CHECK_EQ(call(rt, paint, "nEqualsForTextMeasurement", "(JJ)Z", Args().j(p).j(q)), uint64_t(1));
  call(rt, paint, "nSetColor", "(JI)V", Args().j(q).i(int32_t(0xFFFF0000)));
  CHECK_EQ(call(rt, paint, "nEqualsForTextMeasurement", "(JJ)Z", Args().j(p).j(q)), uint64_t(1));
  call(rt, paint, "nSetLetterSpacing", "(JF)V", Args().j(q).f(0.1f));
  CHECK_EQ(call(rt, paint, "nEqualsForTextMeasurement", "(JJ)Z", Args().j(p).j(q)), uint64_t(0));
  // HWUI keeps a shadow only for a positive radius.
  call(rt, paint, "nSetShadowLayer", "(JFFFJJ)V", Args().j(p).f(0).f(1).f(1).j(0).j(0));
  CHECK_EQ(call(rt, paint, "nHasShadowLayer", "(J)Z", Args().j(p)), uint64_t(0));
}

TEST(graphics_typeface_styles_follow_hwui) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* tf = "Landroid/graphics/Typeface;";
  const char* fb = "Landroid/graphics/fonts/Font$Builder;";
  const char* ffb = "Landroid/graphics/fonts/FontFamily$Builder;";
  // No default typeface yet: a typeface relative to "null" cannot be made.
  CHECK_EQ(thrown(rt, [&] { call(rt, tf, "nativeCreateFromTypeface", "(JI)J", Args().j(0).i(1)); }),
           std::string("java.lang.IllegalStateException"));

  // A font over a direct buffer (its address and capacity).
  int64_t data = bridge::native_alloc(rt, 16);
  rt::GlobalRef buffer(rt, rt.alloc(rt.find_class("Ljava/nio/Buffer;")));
  rt::Object& b = rt.nonnull(buffer.get(), "b");
  rt::Field* address = b.klass->declared_field("address", "J");
  rt::Field* capacity = b.klass->declared_field("capacity", "I");
  b.fields.at(address->slot) = uint32_t(data);
  b.fields.at(address->slot + 1) = uint32_t(uint64_t(data) >> 32);
  b.fields.at(capacity->slot) = 16;
  auto font = [&](int32_t weight, bool italic) {
    int64_t builder = int64_t(call(rt, fb, "nInitBuilder", "()J"));
    call(rt, fb, "nAddAxis", "(JIF)V", Args().j(builder).i(0x77676874).f(float(weight)));  // 'wght'
    return int64_t(call(rt, fb, "nBuild", "(JLjava/nio/ByteBuffer;Ljava/lang/String;Ljava/lang/String;IZI)J",
                        Args().j(builder).h(buffer.get()).h(rt.new_string_utf8("/system/fonts/X.ttf"))
                            .h(rt.new_string_utf8("en")).i(weight).i(italic).i(0)));
  };
  const char* fo = "Landroid/graphics/fonts/Font;";
  int64_t bold = font(700, false), light_italic = font(300, true);
  CHECK_EQ(call(rt, fo, "nGetPackedStyle", "(J)I", Args().j(light_italic)), uint64_t(300 | 1 << 16));
  CHECK_EQ(call(rt, fo, "nGetAxisInfo", "(JI)J", Args().j(bold).i(0)), (uint64_t(0x77676874) << 32) | 0x442F0000);
  CHECK_EQ(call(rt, fo, "nGetBufferAddress", "(J)J", Args().j(bold)), uint64_t(data));
  // The builder is consumed by nBuild.
  int64_t used = int64_t(call(rt, fb, "nInitBuilder", "()J"));
  call(rt, fb, "nBuild", "(JLjava/nio/ByteBuffer;Ljava/lang/String;Ljava/lang/String;IZI)J",
       Args().j(used).h(buffer.get()).h(rt::kNull).h(rt::kNull).i(400).i(0).i(0));
  CHECK_EQ(thrown(rt, [&] { call(rt, fb, "nAddAxis", "(JIF)V", Args().j(used).i(1).f(1)); }),
           std::string("java.lang.IllegalStateException"));

  int64_t family_builder = int64_t(call(rt, ffb, "nInitBuilder", "()J"));
  CHECK_EQ(thrown(rt, [&] {
             call(rt, ffb, "nBuild", "(JLjava/lang/String;IZZI)J", Args().j(family_builder).h(rt::kNull).i(0).i(0).i(0).i(0));
           }),
           std::string("java.lang.IllegalArgumentException"));  // no font: no coverage
  family_builder = int64_t(call(rt, ffb, "nInitBuilder", "()J"));
  call(rt, ffb, "nAddFont", "(JJ)V", Args().j(family_builder).j(light_italic));
  call(rt, ffb, "nAddFont", "(JJ)V", Args().j(family_builder).j(bold));
  int64_t family = int64_t(call(rt, ffb, "nBuild", "(JLjava/lang/String;IZZI)J",
                                Args().j(family_builder).h(rt::kNull).i(0).i(0).i(0).i(0)));

  // Style resolved from the font closest to (400, upright): the light italic
  // one (1 + 2) and the bold one (3) tie, the first wins.
  rt::GlobalRef families(rt, rt.new_array("[J", 1));
  std::memcpy(rt.array(families.get(), "j").data.data(), &family, 8);
  int64_t base = int64_t(call(rt, tf, "nativeCreateFromArray", "([JJII)J", Args().h(families.get()).j(0).i(-1).i(-1)));
  CHECK_EQ(call(rt, tf, "nativeGetWeight", "(J)I", Args().j(base)), uint64_t(300));
  CHECK_EQ(call(rt, tf, "nativeGetStyle", "(J)I", Args().j(base)), uint64_t(2));  // ITALIC
  // Relative styles add 300 for BOLD to the base weight.
  int64_t bold_italic = int64_t(call(rt, tf, "nativeCreateFromTypeface", "(JI)J", Args().j(base).i(3)));
  CHECK_EQ(call(rt, tf, "nativeGetWeight", "(J)I", Args().j(bold_italic)), uint64_t(600));
  CHECK_EQ(call(rt, tf, "nativeGetStyle", "(J)I", Args().j(bold_italic)), uint64_t(3));
  int64_t exact = int64_t(call(rt, tf, "nativeCreateFromTypefaceWithExactStyle", "(JIZ)J", Args().j(base).i(650).i(0)));
  CHECK_EQ(call(rt, tf, "nativeGetStyle", "(J)I", Args().j(exact)), uint64_t(1));  // BOLD from 600
  int64_t alias = int64_t(call(rt, tf, "nativeCreateWeightAlias", "(JI)J", Args().j(base).i(900)));
  CHECK_EQ(call(rt, tf, "nativeGetWeight", "(J)I", Args().j(alias)), uint64_t(900));
  // Once a default exists, "null" means it.
  call(rt, tf, "nativeSetDefault", "(J)V", Args().j(exact));
  int64_t from_default = int64_t(call(rt, tf, "nativeCreateFromTypeface", "(JI)J", Args().j(0).i(0)));
  CHECK_EQ(call(rt, tf, "nativeGetWeight", "(J)I", Args().j(from_default)), uint64_t(300));

  // nativeForceSetStaticFinalField writes Typeface's static fields.
  rt::GlobalRef value(rt, rt.alloc(rt.find_class("Landroid/graphics/Typeface;")));
  call(rt, tf, "nativeForceSetStaticFinalField", "(Ljava/lang/String;Landroid/graphics/Typeface;)V",
       Args().h(rt.new_string_utf8("DEFAULT")).h(value.get()));
  rt::Class* c = rt.find_class("Landroid/graphics/Typeface;");
  CHECK_EQ(c->statics.at(c->declared_field("DEFAULT", "Landroid/graphics/Typeface;")->slot), value.get());
  CHECK_EQ(thrown(rt, [&] {
             call(rt, tf, "nativeForceSetStaticFinalField", "(Ljava/lang/String;Landroid/graphics/Typeface;)V",
                  Args().h(rt.new_string_utf8("ABSENT")).h(value.get()));
           }),
           std::string("java.lang.RuntimeException"));
  bridge::native_free(rt, data);
}

TEST(graphics_matrix_follows_skmatrix) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* mx = "Landroid/graphics/Matrix;";
  const char* ex = "Landroid/graphics/Matrix$ExtraNatives;";
  auto create = [&] { return int64_t(call(rt, ex, "nCreate", "(J)J", Args().j(0))); };
  auto values = [&](int64_t m) {
    rt::GlobalRef arr(rt, rt.new_array("[F", 9));
    call(rt, mx, "nGetValues", "(J[F)V", Args().j(m).h(arr.get()));
    std::vector<float> v(9);
    std::memcpy(v.data(), rt.array(arr.get(), "f").data.data(), 36);
    return v;
  };
  auto map = [&](int64_t m, float x, float y, bool point) {
    rt::GlobalRef arr(rt, rt.new_array("[F", 2));
    float xy[2] = {x, y};
    std::memcpy(rt.array(arr.get(), "f").data.data(), xy, 8);
    call(rt, mx, "nMapPoints", "(J[FI[FIIZ)V", Args().j(m).h(arr.get()).i(0).h(arr.get()).i(0).i(1).i(point));
    std::memcpy(xy, rt.array(arr.get(), "f").data.data(), 8);
    return std::vector<float>{xy[0], xy[1]};
  };
  int64_t m = create();
  CHECK_EQ(call(rt, mx, "nIsIdentity", "(J)Z", Args().j(m)), uint64_t(1));
  // A quarter turn is exact (sine and cosine snapped to zero) and keeps
  // rectangles rectangles.
  call(rt, mx, "nSetRotate", "(JF)V", Args().j(m).f(90));
  CHECK(values(m) == (std::vector<float>{0, -1, 0, 1, 0, 0, 0, 0, 1}));
  CHECK(map(m, 1, 0, true) == (std::vector<float>{0, 1}));
  CHECK_EQ(call(rt, mx, "nRectStaysRect", "(J)Z", Args().j(m)), uint64_t(1));
  call(rt, mx, "nSetRotate", "(JF)V", Args().j(m).f(0));
  CHECK_EQ(call(rt, mx, "nIsIdentity", "(J)Z", Args().j(m)), uint64_t(1));
  // Composition order: pre applies first to the point, post last.
  call(rt, mx, "nSetScale", "(JFF)V", Args().j(m).f(2).f(3));
  call(rt, mx, "nPreTranslate", "(JFF)V", Args().j(m).f(1).f(1));
  call(rt, mx, "nPostTranslate", "(JFF)V", Args().j(m).f(10).f(0));
  CHECK(map(m, 0, 0, true) == (std::vector<float>{12, 3}));  // ((0,0)+(1,1))*(2,3)+(10,0)
  CHECK(map(m, 1, 1, false) == (std::vector<float>{2, 3}));  // vectors ignore the translation
  // Inversion: exact for scale and translate; a singular matrix leaves the
  // destination untouched.
  int64_t inv = create();
  CHECK_EQ(call(rt, mx, "nInvert", "(JJ)Z", Args().j(m).j(inv)), uint64_t(1));
  CHECK(map(inv, 12, 3, true) == (std::vector<float>{0, 0}));
  call(rt, mx, "nSetScale", "(JFF)V", Args().j(m).f(0).f(1));
  CHECK_EQ(call(rt, mx, "nInvert", "(JJ)Z", Args().j(m).j(inv)), uint64_t(0));
  CHECK(map(inv, 12, 3, true) == (std::vector<float>{0, 0}));
  // Rectangles: bounds of the mapped corners, false once rotated off-axis.
  rt::GlobalRef src(rt, rt.alloc(rt.find_class("Landroid/graphics/RectF;")));
  rt::GlobalRef dst(rt, rt.alloc(rt.find_class("Landroid/graphics/RectF;")));
  auto set_rect = [&](rt::Handle h, float l, float t, float r, float b) {
    rt::Object& o = rt.nonnull(h, "r");
    const char* names[4] = {"left", "top", "right", "bottom"};
    float v[4] = {l, t, r, b};
    for (int i = 0; i < 4; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &v[i], 4);
      o.fields.at(o.klass->declared_field(names[i], "F")->slot) = bits;
    }
  };
  set_rect(src.get(), 0, 0, 2, 2);
  call(rt, mx, "nSetRotate", "(JFFF)V", Args().j(m).f(45).f(1).f(1));
  CHECK_EQ(call(rt, mx, "nMapRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z", Args().j(m).h(dst.get()).h(src.get())),
           uint64_t(0));
  CHECK(std::fabs(float_field(rt, dst.get(), "left") - (1 - std::sqrt(2.0f))) < 1e-5f);
  CHECK(std::fabs(float_field(rt, dst.get(), "bottom") - (1 + std::sqrt(2.0f))) < 1e-5f);
  // setRectToRect CENTER: uniform scale, centred on the long axis.
  set_rect(dst.get(), 0, 0, 10, 4);
  CHECK_EQ(call(rt, mx, "nSetRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z",
                Args().j(m).h(src.get()).h(dst.get()).i(2)),
           uint64_t(1));
  CHECK(values(m) == (std::vector<float>{2, 0, 3, 0, 2, 0, 0, 0, 1}));
  // An empty source: identity and false.
  set_rect(src.get(), 1, 1, 1, 5);
  CHECK_EQ(call(rt, mx, "nSetRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z",
                Args().j(m).h(src.get()).h(dst.get()).i(0)),
           uint64_t(0));
  CHECK_EQ(call(rt, mx, "nIsIdentity", "(J)Z", Args().j(m)), uint64_t(1));
  // A copy, then equality by value.
  call(rt, mx, "nSetSkew", "(JFF)V", Args().j(m).f(0.5f).f(0));
  int64_t copy = int64_t(call(rt, ex, "nCreate", "(J)J", Args().j(m)));
  CHECK_EQ(call(rt, mx, "nEquals", "(JJ)Z", Args().j(m).j(copy)), uint64_t(1));
  CHECK_EQ(call(rt, mx, "nEquals", "(JJ)Z", Args().j(m).j(inv)), uint64_t(0));
}
