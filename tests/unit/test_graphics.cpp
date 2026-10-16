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
#include "third_party/miniz/miniz.h"

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
    R"(
.class public Landroid/graphics/Rect;
.super Ljava/lang/Object;
.field public left:I
.field public top:I
.field public right:I
.field public bottom:I
.method public void <init>(IIII)V
.registers 5
iput p1, p0, Landroid/graphics/Rect;->left:I
iput p2, p0, Landroid/graphics/Rect;->top:I
iput p3, p0, Landroid/graphics/Rect;->right:I
iput p4, p0, Landroid/graphics/Rect;->bottom:I
return-void
.end method
)",
    R"(
.class public Landroid/graphics/NinePatch;
.super Ljava/lang/Object;
)",
    R"(
.class public Landroid/graphics/NinePatch$InsetStruct;
.super Ljava/lang/Object;
.method public void <init>(IIIIIIIIFIF)V
.registers 12
return-void
.end method
)",
    R"(
.class public Landroid/graphics/ColorSpace;
.super Ljava/lang/Object;
)",
    R"(
.class public Landroid/graphics/Bitmap;
.super Ljava/lang/Object;
.field public mNativePtr:J
.field public mWidth:I
.field public mHeight:I
.field public mDensity:I
.field public mIsMutable:Z
.field public mNinePatchChunk:[B
.field public mNinePatchInsets:Landroid/graphics/NinePatch$InsetStruct;
.method void <init>(JIIIZ[BLandroid/graphics/NinePatch$InsetStruct;)V
.registers 9
iput-wide p1, p0, Landroid/graphics/Bitmap;->mNativePtr:J
return-void
.end method
)",
    R"(
.class public Landroid/graphics/ImageDecoder$Source;
.super Ljava/lang/Object;
)",
    R"(
.class public Landroid/graphics/ImageDecoder;
.super Ljava/lang/Object;
.field public mNativePtr:J
.field public mWidth:I
.field public mHeight:I
.field public mNinePatchChunk:[B
.field public mOpticalInsets:Landroid/graphics/Rect;
.field public mOutlineInsets:Landroid/graphics/Rect;
.field public mOutlineRadius:F
.field public mOutlineAlpha:I
.method void <init>(JIIZZLjava/lang/String;Landroid/graphics/ColorSpace;Landroid/graphics/Rect;)V
.registers 10
iput-wide p1, p0, Landroid/graphics/ImageDecoder;->mNativePtr:J
return-void
.end method
)",
    R"(
.class public Landroid/graphics/Region;
.super Ljava/lang/Object;
.field public mNativeRegion:J
)",
    R"(
.class public Landroid/util/Size;
.super Ljava/lang/Object;
.field public mWidth:I
.field public mHeight:I
.method public void <init>(II)V
.registers 3
iput p1, p0, Landroid/util/Size;->mWidth:I
iput p2, p0, Landroid/util/Size;->mHeight:I
return-void
.end method
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
  Args& z(bool v) {
    w.push_back(v ? 1 : 0);
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

const char* kPathParser = "Landroid/util/PathParser;";

// "java.lang.X: message" of the exception thrown by f, or "".
template <typename F>
std::string thrown_text(rt::Runtime& rt, F&& f) {
  try {
    f();
  } catch (const rt::JavaThrow& t) {
    std::string d = rt.describe_throwable(t.exception);
    return d.substr(0, d.find('\n'));
  }
  return "";
}

float float_field(rt::Runtime& rt, rt::Handle h, const char* name) {
  rt::Object& o = rt.nonnull(h, "o");
  rt::Field* f = o.klass->declared_field(name, "F");
  float v;
  uint32_t bits = o.fields.at(f->slot);
  std::memcpy(&v, &bits, 4);
  return v;
}

int64_t get_native_ptr(rt::Runtime& rt, rt::Handle h, const char* cls = "Landroid/graphics/Bitmap;") {
  rt::Object& o = rt.nonnull(h, cls);
  rt::Field* f = o.klass->declared_field("mNativePtr", "J");
  uint64_t low = o.fields.at(f->slot);
  uint64_t high = o.fields.at(f->slot + 1);
  return int64_t(low | (high << 32));
}

int32_t get_field_i32(rt::Runtime& rt, rt::Handle h, const char* name) {
  rt::Object& o = rt.nonnull(h, "o");
  rt::Field* f = o.klass->declared_field(name, "I");
  return int32_t(o.fields.at(f->slot));
}

std::vector<uint8_t> make_dummy_9patch_chunk() {
  std::vector<uint8_t> chunk(64, 0);
  chunk[0] = 0;
  chunk[1] = 2;
  chunk[2] = 2;
  chunk[3] = 4;
  int32_t pad = 4;
  std::memcpy(&chunk[12], &pad, 4);
  std::memcpy(&chunk[16], &pad, 4);
  std::memcpy(&chunk[20], &pad, 4);
  std::memcpy(&chunk[24], &pad, 4);
  int32_t x0 = 2, x1 = 8;
  std::memcpy(&chunk[32], &x0, 4);
  std::memcpy(&chunk[36], &x1, 4);
  int32_t y0 = 2, y1 = 8;
  std::memcpy(&chunk[40], &y0, 4);
  std::memcpy(&chunk[44], &y1, 4);
  uint32_t c = 1;
  for (int i = 0; i < 4; ++i) std::memcpy(&chunk[48 + i * 4], &c, 4);
  return chunk;
}

std::vector<uint8_t> make_png_rgba(int width, int height, uint32_t argb_color,
                                   const std::vector<uint8_t>& nptc = {}) {
  std::vector<uint8_t> png;
  const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.insert(png.end(), sig, sig + sizeof(sig));

  auto write_chunk = [&](const char type[4], const uint8_t* data, size_t len) {
    uint8_t len_bytes[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
    png.insert(png.end(), len_bytes, len_bytes + 4);
    size_t type_start = png.size();
    png.insert(png.end(), type, type + 4);
    if (len > 0) png.insert(png.end(), data, data + len);
    uint32_t crc = uint32_t(mz_crc32(0, png.data() + type_start, len + 4));
    uint8_t crc_bytes[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    png.insert(png.end(), crc_bytes, crc_bytes + 4);
  };

  uint8_t ihdr[13] = {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width),
                      uint8_t(height >> 24), uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height),
                      8, 6, 0, 0, 0};
  write_chunk("IHDR", ihdr, 13);

  if (!nptc.empty()) {
    write_chunk("npTc", nptc.data(), nptc.size());
  }

  std::vector<uint8_t> raw;
  uint8_t a = uint8_t((argb_color >> 24) & 0xff);
  uint8_t r = uint8_t((argb_color >> 16) & 0xff);
  uint8_t g = uint8_t((argb_color >> 8) & 0xff);
  uint8_t b = uint8_t(argb_color & 0xff);
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // None filter
    for (int x = 0; x < width; ++x) {
      raw.push_back(r);
      raw.push_back(g);
      raw.push_back(b);
      raw.push_back(a);
    }
  }

  mz_ulong comp_len = mz_compressBound(mz_ulong(raw.size()));
  std::vector<uint8_t> comp(comp_len);
  mz_compress(comp.data(), &comp_len, raw.data(), mz_ulong(raw.size()));
  write_chunk("IDAT", comp.data(), comp_len);
  write_chunk("IEND", nullptr, 0);

  return png;
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
// android.util.PathParser: VectorDrawable's SVG path strings, parsed as HWUI
// does (numbers split at '-' and at a second '.', exponents kept), turned
// into SkPath verbs (smooth curves reflect the previous control point, 'z'
// moves the pen back, arcs become conics as in SkPath::arcTo), and morphed.
TEST(graphics_path_parser_follows_hwui) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  auto parse = [&](const std::string& s) {
    int64_t p = int64_t(call(rt, kPath, "nInit", "()J"));
    rt::GlobalRef str(rt, rt.new_string_utf8(s));
    call(rt, kPathParser, "nParseStringForPath", "(JLjava/lang/String;I)V",
         Args().j(p).h(str.get()).i(int32_t(s.size())));
    return p;
  };
  auto data = [&](const std::string& s) {
    rt::GlobalRef str(rt, rt.new_string_utf8(s));
    return int64_t(call(rt, kPathParser, "nCreatePathDataFromString", "(Ljava/lang/String;I)J",
                        Args().h(str.get()).i(int32_t(s.size()))));
  };

  Contents c = contents(rt, parse("M10,20 L30,40 z m5-5 10,0 h-3 v2 Z"));
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine, kClose, kMove, kMove, kLine, kLine, kLine, kClose, kMove}));
  CHECK(c.coords == (std::vector<float>{10, 20, 30, 40, 10, 20, 15, 15, 25, 15, 22, 15, 22, 17, 15, 15}));

  c = contents(rt, parse("M0,0 C10,0 20,10 20,20 S30,40 40,40"));
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kCubic, kCubic}));
  CHECK(c.coords == (std::vector<float>{0, 0, 10, 0, 20, 10, 20, 20, 20, 30, 30, 40, 40, 40}));
  c = contents(rt, parse("M0,0 Q10,0 10,10 T10,30 M1e-1-2 L.5.5"));
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kQuad, kQuad, kMove, kLine}));
  CHECK(c.coords == (std::vector<float>{0, 0, 10, 0, 10, 10, 10, 20, 10, 30, 0.1f, -2, 0.5f, 0.5f}));

  // The upper half circle: two quarter conics, on integers like the input.
  c = contents(rt, parse("M0,0 A10,10 0 0,1 20,0"));
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kConic, kConic}));
  CHECK(c.coords == (std::vector<float>{0, 0, 0, -10, 10, -10, 20, -10, 20, 0}));
  CHECK_EQ(c.weights.size(), size_t(2));
  CHECK(std::fabs(c.weights[0] - 0.70710678f) < 1e-6f);
  // A null radius is a line; sweep 0 goes the other way round.
  c = contents(rt, parse("M0,0 a0,5 0 0 0 5,5 A10,10 0 0,0 25,5"));
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine, kConic, kConic}));
  CHECK(c.coords == (std::vector<float>{0, 0, 5, 5, 5, 15, 15, 15, 25, 15, 25, 5}));

  // HWUI's messages, word for word.
  const std::string iae = "java.lang.IllegalArgumentException: ";
  CHECK_EQ(thrown_text(rt, [&] { parse("M10"); }),
           iae + "M needs to be followed by a multiple of 2 floats. However, 1 float(s) are found. "
                 "Failure occurred at position 0 of path: M10");
  CHECK_EQ(thrown_text(rt, [&] { parse("M1e1-2.5.5"); }),
           iae + "M needs to be followed by a multiple of 2 floats. However, 3 float(s) are found. "
                 "Failure occurred at position 0 of path: M1e1-2.5.5");
  CHECK_EQ(thrown_text(rt, [&] { parse("M1,2 L3,4.5.6"); }),
           iae + "L needs to be followed by a multiple of 2 floats. However, 3 float(s) are found. "
                 "Failure occurred at position 5 of path: M1,2 L3,4.5.6");
  CHECK_EQ(thrown_text(rt, [&] { parse("X1"); }),
           iae + "X is not a valid verb. Failure occurred at position 0 of path: X1");
  CHECK_EQ(thrown_text(rt, [&] { parse("   "); }), iae + "Path string cannot be empty.");

  // Morphing: same verbs and float counts, floats interpolated.
  int64_t a = data("M0,0 L10,0"), b = data("M0,0 L20,10"), cubic = data("M0,0 C1,1 2,2 3,3");
  CHECK_EQ(call(rt, kPathParser, "nCanMorph", "(JJ)Z", Args().j(a).j(b)), uint64_t(1));
  CHECK_EQ(call(rt, kPathParser, "nCanMorph", "(JJ)Z", Args().j(a).j(cubic)), uint64_t(0));
  int64_t out = int64_t(call(rt, kPathParser, "nCreateEmptyPathData", "()J"));
  CHECK_EQ(call(rt, kPathParser, "nInterpolatePathData", "(JJJF)Z", Args().j(out).j(a).j(b).f(0.5f)), uint64_t(1));
  CHECK_EQ(call(rt, kPathParser, "nInterpolatePathData", "(JJJF)Z", Args().j(out).j(a).j(cubic).f(0.5f)),
           uint64_t(0));
  int64_t p = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPathParser, "nCreatePathFromPathData", "(JJ)V", Args().j(p).j(out));
  c = contents(rt, p);
  CHECK(c.verbs == (std::vector<uint8_t>{kMove, kLine}));
  CHECK(c.coords == (std::vector<float>{0, 0, 15, 5}));
  int64_t copy = int64_t(call(rt, kPathParser, "nCreatePathData", "(J)J", Args().j(cubic)));
  call(rt, kPathParser, "nSetPathData", "(JJ)V", Args().j(out).j(copy));
  call(rt, kPathParser, "nCreatePathFromPathData", "(JJ)V", Args().j(p).j(out));
  CHECK(contents(rt, p).verbs == (std::vector<uint8_t>{kMove, kCubic}));
  CHECK_EQ(thrown_text(rt, [&] { data("Q1"); }),
           iae + "Q needs to be followed by a multiple of 4 floats. However, 1 float(s) are found. "
                 "Failure occurred at position 0 of path: Q1");
  call(rt, kPathParser, "nFinalize", "(J)V", Args().j(copy));
  CHECK_EQ(thrown(rt, [&] { call(rt, kPathParser, "nCreatePathData", "(J)J", Args().j(copy)); }),
           std::string("java.lang.IllegalStateException"));
}

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

// Canvas without pixels (J4): HWUI's save stack with its partial saves, the
// current matrix, and the clip's device bounds as SkCanvas rounds them.
TEST(graphics_canvas_state_follows_skia) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* cv = "Landroid/graphics/Canvas;";
  rt::GlobalRef rect(rt, rt.alloc(rt.find_class("Landroid/graphics/Rect;")));
  auto clip = [&](int64_t c) {
    uint64_t nonempty = call(rt, cv, "nGetClipBounds", "(JLandroid/graphics/Rect;)Z", Args().j(c).h(rect.get()));
    std::string out = nonempty ? "1:" : "0:";
    for (const char* f : {"left", "top", "right", "bottom"}) out += std::to_string(get_field_i32(rt, rect.get(), f)) + " ";
    return out;
  };
  auto i32 = [&](int64_t c, const char* name) { return int32_t(call(rt, cv, name, "(J)I", Args().j(c))); };
  auto reject = [&](int64_t c, float l, float t, float r, float b) {
    return call(rt, cv, "nQuickReject", "(JFFFF)Z", Args().j(c).f(l).f(t).f(r).f(b)) != 0;
  };
  auto clip_rect = [&](int64_t c, float l, float t, float r, float b, int32_t op) {
    return call(rt, cv, "nClipRect", "(JFFFFI)Z", Args().j(c).f(l).f(t).f(r).f(b).i(op)) != 0;
  };

  // new Canvas(): no target, so an empty clip.
  int64_t none = int64_t(call(rt, cv, "nInitRaster", "(J)J", Args().j(0)));
  CHECK_EQ(i32(none, "nGetWidth"), 0);
  CHECK_EQ(i32(none, "nGetSaveCount"), 1);
  CHECK_EQ(clip(none), std::string("0:0 0 0 0 "));
  CHECK(reject(none, 0, 0, 10, 10));
  CHECK_EQ(call(rt, cv, "nRestore", "(J)Z", Args().j(none)), uint64_t(0));  // Canvas.java: underflow

  rt::Handle bm = rt::Handle(call(rt, "Landroid/graphics/Bitmap;", "nativeCreate",
                                  "([IIIIIIZJ)Landroid/graphics/Bitmap;",
                                  Args().h(rt::kNull).i(0).i(100).i(100).i(50).i(5).z(true).j(0)));
  rt::GlobalRef keep(rt, bm);
  int64_t bitmap = get_native_ptr(rt, bm);
  int64_t c = int64_t(call(rt, cv, "nInitRaster", "(J)J", Args().j(bitmap)));
  CHECK_EQ(i32(c, "nGetWidth"), 100);
  CHECK_EQ(i32(c, "nGetHeight"), 50);
  CHECK_EQ(call(rt, cv, "nIsOpaque", "(J)Z", Args().j(c)), uint64_t(0));
  CHECK_EQ(clip(c), std::string("1:0 0 100 50 "));

  // Matrix and a clip rounded to device pixels (no antialiasing).
  CHECK_EQ(int32_t(call(rt, cv, "nSave", "(JI)I", Args().j(c).i(3))), 1);
  call(rt, cv, "nTranslate", "(JFF)V", Args().j(c).f(10).f(5));
  call(rt, cv, "nScale", "(JFF)V", Args().j(c).f(2).f(2));
  CHECK(clip_rect(c, 0, 0, 20.3f, 10, 1));  // device 10,5 to 50.6,25: 10 5 51 25
  CHECK_EQ(clip(c), std::string("1:0 0 21 10 "));  // 20.5 rounds up
  CHECK(reject(c, 30, 0, 40, 5));    // device x 70..90, beyond the clip outset by 1
  CHECK(!reject(c, 20, 0, 30, 5));   // device x 50..70 meets 9..52
  int64_t m = int64_t(call(rt, "Landroid/graphics/Matrix$ExtraNatives;", "nCreate", "(J)J", Args().j(0)));
  call(rt, cv, "nGetMatrix", "(JJ)V", Args().j(c).j(m));
  CHECK(bridge::skmatrix::of(rt, m) == (bridge::skmatrix::Values{2, 0, 10, 0, 2, 5, 0, 0, 1}));
  CHECK_EQ(call(rt, cv, "nRestore", "(J)Z", Args().j(c)), uint64_t(1));
  CHECK_EQ(i32(c, "nGetSaveCount"), 1);
  CHECK_EQ(clip(c), std::string("1:0 0 100 50 "));

  // Difference: a band along a whole edge shrinks the bounds, a hole does not.
  call(rt, cv, "nSave", "(JI)I", Args().j(c).i(3));
  CHECK(clip_rect(c, 0, 0, 100, 10, 0));
  CHECK(clip_rect(c, 40, 20, 60, 30, 0));
  CHECK_EQ(clip(c), std::string("1:0 10 100 50 "));
  call(rt, cv, "nRestoreToCount", "(JI)V", Args().j(c).i(1));

  // A save of the matrix only (HWUI's partial save): the clip outlives restore.
  CHECK_EQ(int32_t(call(rt, cv, "nSave", "(JI)I", Args().j(c).i(1))), 1);
  call(rt, cv, "nTranslate", "(JFF)V", Args().j(c).f(5).f(0));
  clip_rect(c, 0, 0, 10, 10, 1);
  call(rt, cv, "nRestore", "(J)Z", Args().j(c));
  CHECK_EQ(clip(c), std::string("1:5 0 15 10 "));  // identity matrix again

  // A new target resets everything; a quarter turn keeps rectangles exact.
  call(rt, cv, "nSetBitmap", "(JJ)V", Args().j(c).j(bitmap));
  call(rt, cv, "nTranslate", "(JFF)V", Args().j(c).f(50).f(0));
  call(rt, cv, "nRotate", "(JF)V", Args().j(c).f(90));
  CHECK(clip_rect(c, 0, 0, 10, 20, 1));  // device 30 0 50 10
  CHECK_EQ(clip(c), std::string("1:0 0 10 20 "));
  call(rt, cv, "nSetMatrix", "(JJ)V", Args().j(c).j(0));
  CHECK_EQ(clip(c), std::string("1:30 0 50 10 "));

  // Clip by a path: its bounds; an empty path rejects everything.
  call(rt, cv, "nSetBitmap", "(JJ)V", Args().j(c).j(bitmap));
  int64_t tri = int64_t(call(rt, kPath, "nInit", "()J"));
  call(rt, kPath, "nMoveTo", "(JFF)V", Args().j(tri).f(0.5f).f(0));
  call(rt, kPath, "nLineTo", "(JFF)V", Args().j(tri).f(30).f(0));
  call(rt, kPath, "nLineTo", "(JFF)V", Args().j(tri).f(0).f(39.5f));
  CHECK_EQ(call(rt, cv, "nClipPath", "(JJI)Z", Args().j(c).j(tri).i(1)), uint64_t(1));
  CHECK_EQ(clip(c), std::string("1:0 0 30 40 "));
  int64_t nothing = int64_t(call(rt, kPath, "nInit", "()J"));
  CHECK_EQ(call(rt, cv, "nQuickReject", "(JJ)Z", Args().j(c).j(nothing)), uint64_t(1));
  CHECK_EQ(call(rt, cv, "nQuickReject", "(JJ)Z", Args().j(c).j(tri)), uint64_t(0));
  CHECK_EQ(thrown(rt, [&] { call(rt, cv, "nGetSaveCount", "(J)I", Args().j(tri)); }),
           std::string("java.lang.IllegalStateException"));
}

// Region: SkRegion's canonical bands, boolean operations, iteration order
// and toString, the instance natives reading Region.mNativeRegion.
TEST(graphics_region_follows_skregion) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* rg = "Landroid/graphics/Region;";
  auto make = [&] { return int64_t(call(rt, rg, "nativeConstructor", "()J")); };
  auto set_rect = [&](int64_t r, int32_t l, int32_t t, int32_t rr, int32_t b) {
    return call(rt, rg, "nativeSetRect", "(JIIII)Z", Args().j(r).i(l).i(t).i(rr).i(b)) != 0;
  };
  auto op_rect = [&](int64_t r, int32_t l, int32_t t, int32_t rr, int32_t b, int32_t op) {
    return call(rt, rg, "nativeOp", "(JIIIII)Z", Args().j(r).i(l).i(t).i(rr).i(b).i(op)) != 0;
  };
  auto text = [&](int64_t r) {
    return rt.string_utf8(rt::Handle(call(rt, rg, "nativeToString", "(J)Ljava/lang/String;", Args().j(r))));
  };
  // A Java Region around a native one, for the instance natives.
  auto java = [&](int64_t r) {
    rt::Handle h = rt.alloc(rt.find_class(rg));
    rt::Object& o = rt.nonnull(h, "Region");
    rt::Field* f = o.klass->declared_field("mNativeRegion", "J");
    o.fields.at(f->slot) = uint32_t(uint64_t(r));
    o.fields.at(f->slot + 1) = uint32_t(uint64_t(r) >> 32);
    return h;
  };
  auto is = [&](rt::Handle h, const char* name) { return call(rt, rg, name, "()Z", Args().h(h)) != 0; };

  int64_t a = make();
  CHECK(set_rect(a, 0, 0, 10, 10));
  CHECK(op_rect(a, 5, 5, 15, 15, 2));  // union
  CHECK_EQ(text(a), std::string("SkRegion((0,0,10,5)(0,5,15,10)(5,10,15,15))"));
  rt::GlobalRef ja(rt, java(a));
  CHECK(!is(ja.get(), "isRect"));
  CHECK(is(ja.get(), "isComplex"));
  CHECK_EQ(call(rt, rg, "contains", "(II)Z", Args().h(ja.get()).i(12).i(7)), uint64_t(1));
  CHECK_EQ(call(rt, rg, "contains", "(II)Z", Args().h(ja.get()).i(12).i(2)), uint64_t(0));
  rt::GlobalRef rect(rt, rt.alloc(rt.find_class("Landroid/graphics/Rect;")));
  CHECK_EQ(call(rt, rg, "nativeGetBounds", "(JLandroid/graphics/Rect;)Z", Args().j(a).h(rect.get())), uint64_t(1));
  CHECK_EQ(get_field_i32(rt, rect.get(), "right"), 15);

  // The square minus the union; the union with itself in xor is empty.
  int64_t square = make(), diff = make();
  set_rect(square, 0, 0, 15, 15);
  CHECK_EQ(call(rt, rg, "nativeOp", "(JJJI)Z", Args().j(diff).j(square).j(a).i(0)), uint64_t(1));
  CHECK_EQ(text(diff), std::string("SkRegion((10,0,15,5)(0,10,5,15))"));
  CHECK_EQ(call(rt, rg, "nativeOp", "(JJJI)Z", Args().j(diff).j(a).j(a).i(3)), uint64_t(0));
  rt::GlobalRef jd(rt, java(diff));
  CHECK(is(jd.get(), "isEmpty"));

  // Canonical form: two halves make the same region as the whole.
  int64_t halves = make(), whole = make();
  set_rect(halves, 0, 0, 10, 5);
  op_rect(halves, 0, 5, 10, 10, 2);
  set_rect(whole, 0, 0, 10, 10);
  CHECK_EQ(call(rt, rg, "nativeEquals", "(JJ)Z", Args().j(halves).j(whole)), uint64_t(1));
  rt::GlobalRef jh(rt, java(halves));
  CHECK(is(jh.get(), "isRect"));
  CHECK_EQ(call(rt, rg, "quickContains", "(IIII)Z", Args().h(jh.get()).i(1).i(1).i(9).i(9)), uint64_t(1));
  CHECK_EQ(call(rt, rg, "quickReject", "(IIII)Z", Args().h(jh.get()).i(10).i(0).i(20).i(5)), uint64_t(1));

  // Rect op region (intersect), translate and scale (rounded as roundf).
  rt::GlobalRef r8(rt, rt.alloc(rt.find_class("Landroid/graphics/Rect;")));
  rt::Object& ro = rt.nonnull(r8.get(), "Rect");
  int32_t vals[4] = {2, 2, 8, 8};
  const char* names[4] = {"left", "top", "right", "bottom"};
  for (int i = 0; i < 4; ++i) ro.fields.at(ro.klass->declared_field(names[i], "I")->slot) = uint32_t(vals[i]);
  int64_t inter = make();
  CHECK_EQ(call(rt, rg, "nativeOp", "(JLandroid/graphics/Rect;JI)Z", Args().j(inter).h(r8.get()).j(diff).i(1)), uint64_t(0));
  CHECK_EQ(call(rt, rg, "nativeOp", "(JLandroid/graphics/Rect;JI)Z", Args().j(inter).h(r8.get()).j(a).i(1)), uint64_t(1));
  CHECK_EQ(text(inter), std::string("SkRegion((2,2,8,8))"));
  call(rt, rg, "translate", "(IILandroid/graphics/Region;)V", Args().h(jh.get()).i(5).i(-1).h(rt::kNull));
  CHECK_EQ(text(halves), std::string("SkRegion((5,-1,15,9))"));
  rt::GlobalRef js(rt, java(square));
  call(rt, rg, "scale", "(FLandroid/graphics/Region;)V", Args().h(js.get()).f(0.5f).h(jd.get()));
  CHECK_EQ(text(diff), std::string("SkRegion((0,0,8,8))"));

  // RegionIterator: band by band, left to right, on a copy.
  const char* it = "Landroid/graphics/RegionIterator;";
  int64_t iter = int64_t(call(rt, it, "nativeConstructor", "(J)J", Args().j(a)));
  set_rect(a, 0, 0, 1, 1);
  std::string seen;
  while (call(rt, it, "nativeNext", "(JLandroid/graphics/Rect;)Z", Args().j(iter).h(rect.get()))) {
    seen += std::to_string(get_field_i32(rt, rect.get(), "left")) + "," +
            std::to_string(get_field_i32(rt, rect.get(), "top")) + " ";
  }
  CHECK_EQ(seen, std::string("0,0 0,5 5,10 "));
  CHECK_EQ(thrown(rt, [&] { op_rect(a, 0, 0, 1, 1, 9); }), std::string("java.lang.IllegalArgumentException"));
}

// VectorDrawable: HWUI's tree as a property store, its defaults and property
// layouts, owned by VirtualRefBasePtr (the last reference frees every node).
TEST(graphics_vector_drawable_follows_hwui) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* vd = "Landroid/graphics/drawable/VectorDrawable;";
  const char* ref = "Lcom/android/internal/util/VirtualRefBasePtr;";
  auto f32 = [](uint64_t v) {
    float f;
    uint32_t bits = uint32_t(v);
    std::memcpy(&f, &bits, 4);
    return f;
  };
  int64_t root = int64_t(call(rt, vd, "nCreateGroup", "()J"));
  int64_t path = int64_t(call(rt, vd, "nCreateFullPath", "()J"));
  int64_t clip = int64_t(call(rt, vd, "nCreateClipPath", "()J"));
  call(rt, vd, "nAddChild", "(JJ)V", Args().j(root).j(clip));
  call(rt, vd, "nAddChild", "(JJ)V", Args().j(root).j(path));
  CHECK_EQ(f32(call(rt, vd, "nGetScaleX", "(J)F", Args().j(root))), 1.0f);
  call(rt, vd, "nSetRotation", "(JF)V", Args().j(root).f(45));
  call(rt, vd, "nUpdateGroupProperties", "(JFFFFFFF)V", Args().j(root).f(90).f(1).f(2).f(3).f(4).f(5).f(6));
  rt::GlobalRef floats(rt, rt.new_array("[F", 7));
  CHECK_EQ(call(rt, vd, "nGetGroupProperties", "(J[FI)Z", Args().j(root).h(floats.get()).i(7)), uint64_t(1));
  float g[7];
  std::memcpy(g, rt.array(floats.get(), "f").data.data(), sizeof g);
  CHECK(g[0] == 90 && g[3] == 3 && g[6] == 6);

  // FullPathProperties::PrimitiveFields, defaults then updated.
  rt::GlobalRef bytes(rt, rt.new_array("[B", 48));
  CHECK_EQ(call(rt, vd, "nGetFullPathProperties", "(J[BI)Z", Args().j(path).h(bytes.get()).i(48)), uint64_t(1));
  float fields[12];
  std::memcpy(fields, rt.array(bytes.get(), "b").data.data(), 48);
  CHECK(fields[0] == 0 && fields[2] == 1 && fields[4] == 1 && fields[6] == 1 && fields[10] == 4);  // widths, alphas, trim end, miter
  call(rt, vd, "nUpdateFullPathProperties", "(JFIFIFFFFFIII)V",
       Args().j(path).f(2).i(int32_t(0xFF0000FF)).f(0.5f).i(int32_t(0xFF00FF00)).f(0.25f).f(0.1f).f(0.9f).f(0.2f).f(8).i(1).i(2).i(1));
  CHECK_EQ(uint32_t(call(rt, vd, "nGetFillColor", "(J)I", Args().j(path))), uint32_t(0xFF00FF00));
  CHECK_EQ(f32(call(rt, vd, "nGetTrimPathEnd", "(J)F", Args().j(path))), 0.9f);
  call(rt, vd, "nSetStrokeWidth", "(JF)V", Args().j(path).f(3));
  CHECK_EQ(f32(call(rt, vd, "nGetStrokeWidth", "(J)F", Args().j(path))), 3.0f);
  CHECK_EQ(thrown(rt, [&] { call(rt, vd, "nGetFullPathProperties", "(J[BI)Z", Args().j(path).h(bytes.get()).i(40)); }),
           std::string("java.lang.IllegalArgumentException"));
  rt::GlobalRef bad(rt, rt.new_string_utf8("M 1"));
  CHECK_EQ(thrown(rt, [&] { call(rt, vd, "nSetPathString", "(JLjava/lang/String;I)V", Args().j(path).h(bad.get()).i(3)); }),
           std::string("java.lang.IllegalArgumentException"));
  CHECK_EQ(thrown(rt, [&] { call(rt, vd, "nGetRotation", "(J)F", Args().j(path)); }),  // a path is not a group
           std::string("java.lang.IllegalStateException"));

  // The tree: root alpha, a copy keeping the viewport, freed with its nodes.
  int64_t tree = int64_t(call(rt, vd, "nCreateTree", "(J)J", Args().j(root)));
  call(rt, ref, "nIncStrong", "(J)V", Args().j(tree));
  call(rt, vd, "nSetRendererViewportSize", "(JFF)V", Args().j(tree).f(24).f(24));
  CHECK_EQ(call(rt, vd, "nSetRootAlpha", "(JF)Z", Args().j(tree).f(0.5f)), uint64_t(1));
  CHECK_EQ(call(rt, vd, "nSetRootAlpha", "(JF)Z", Args().j(tree).f(0.5f)), uint64_t(0));  // unchanged
  int64_t root2 = int64_t(call(rt, vd, "nCreateGroup", "(J)J", Args().j(root)));
  CHECK_EQ(f32(call(rt, vd, "nGetRotation", "(J)F", Args().j(root2))), 90.0f);
  int64_t tree2 = int64_t(call(rt, vd, "nCreateTreeFromCopy", "(JJ)J", Args().j(tree).j(root2)));
  CHECK_EQ(f32(call(rt, vd, "nGetRootAlpha", "(J)F", Args().j(tree2))), 0.5f);
  call(rt, ref, "nDecStrong", "(J)V", Args().j(tree));
  CHECK_EQ(thrown(rt, [&] { call(rt, vd, "nGetStrokeWidth", "(J)F", Args().j(path)); }),
           std::string("java.lang.IllegalStateException"));
  CHECK_EQ(f32(call(rt, vd, "nGetRotation", "(J)F", Args().j(root2))), 90.0f);  // the copy's tree lives on
}

TEST(graphics_nine_patch_natives) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* np = "Landroid/graphics/NinePatch;";

  // isNinePatchChunk with null returns false
  CHECK_EQ(call(rt, np, "isNinePatchChunk", "([B)Z", Args().h(rt::kNull)), uint64_t(0));

  // isNinePatchChunk with short invalid array returns false
  rt::Handle bad_arr = rt.new_array("[B", 10);
  CHECK_EQ(call(rt, np, "isNinePatchChunk", "([B)Z", Args().h(bad_arr)), uint64_t(0));

  // isNinePatchChunk with valid chunk returns true
  auto chunk_data = make_dummy_9patch_chunk();
  rt::Handle good_arr = rt.new_array("[B", uint32_t(chunk_data.size()));
  std::memcpy(rt.array(good_arr, "byte[]").data.data(), chunk_data.data(), chunk_data.size());
  CHECK_EQ(call(rt, np, "isNinePatchChunk", "([B)Z", Args().h(good_arr)), uint64_t(1));

  // validateNinePatchChunk with null throws
  CHECK_EQ(thrown(rt, [&] { call(rt, np, "validateNinePatchChunk", "([B)J", Args().h(rt::kNull)); }),
           std::string("java.lang.IllegalArgumentException"));

  // validateNinePatchChunk with bad array throws
  CHECK_EQ(thrown(rt, [&] { call(rt, np, "validateNinePatchChunk", "([B)J", Args().h(bad_arr)); }),
           std::string("java.lang.IllegalArgumentException"));

  // validateNinePatchChunk with valid chunk returns handle (tag 0x110C)
  uint64_t h = call(rt, np, "validateNinePatchChunk", "([B)J", Args().h(good_arr));
  CHECK_EQ(uint16_t(h >> 48), uint16_t(0x110C));

  // nativeFinalize cleans up handle
  call(rt, np, "nativeFinalize", "(J)V", Args().j(int64_t(h)));
}

TEST(graphics_bitmap_natives) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* bm = "Landroid/graphics/Bitmap;";

  // nativeCreate: 4x4 ARGB_8888 bitmap
  rt::Handle bm_obj = rt::Handle(call(rt, bm, "nativeCreate", "([IIIIIIZJ)Landroid/graphics/Bitmap;",
                                      Args().h(rt::kNull).i(0).i(4).i(4).i(4).i(5).z(true).j(0)));
  CHECK(bm_obj != rt::kNull);
  int64_t ptr = get_native_ptr(rt, bm_obj);
  CHECK_EQ(uint16_t(uint64_t(ptr) >> 48), uint16_t(0x110B));

  // Property queries
  CHECK_EQ(call(rt, bm, "nativeRowBytes", "(J)I", Args().j(ptr)), uint64_t(16));
  CHECK_EQ(call(rt, bm, "nativeConfig", "(J)I", Args().j(ptr)), uint64_t(5));
  CHECK_EQ(call(rt, bm, "nativeHasAlpha", "(J)Z", Args().j(ptr)), uint64_t(1));
  CHECK_EQ(call(rt, bm, "nativeIsPremultiplied", "(J)Z", Args().j(ptr)), uint64_t(1));
  CHECK_EQ(call(rt, bm, "nativeGetAllocationByteCount", "(J)I", Args().j(ptr)), uint64_t(64));

  // Pixel read/write
  CHECK_EQ(call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(ptr).i(1).i(2)), uint64_t(0));
  call(rt, bm, "nativeSetPixel", "(JIII)V", Args().j(ptr).i(1).i(2).i(int32_t(0xFF112233)));
  CHECK_EQ(uint32_t(call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(ptr).i(1).i(2))), uint32_t(0xFF112233));

  // Out of bounds checks
  CHECK_EQ(thrown(rt, [&] { call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(ptr).i(10).i(2)); }),
           std::string("java.lang.IllegalArgumentException"));
  CHECK_EQ(thrown(rt, [&] { call(rt, bm, "nativeSetPixel", "(JIII)V", Args().j(ptr).i(1).i(20).i(0)); }),
           std::string("java.lang.IllegalArgumentException"));

  // Erase with color
  call(rt, bm, "nativeErase", "(JI)V", Args().j(ptr).i(int32_t(0xFFAABBCC)));
  CHECK_EQ(uint32_t(call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(ptr).i(0).i(0))), uint32_t(0xFFAABBCC));
  CHECK_EQ(uint32_t(call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(ptr).i(3).i(3))), uint32_t(0xFFAABBCC));

  // Copy and sameAs
  rt::Handle copy_obj = rt::Handle(call(rt, bm, "nativeCopy", "(JIZ)Landroid/graphics/Bitmap;",
                                        Args().j(ptr).i(5).z(true)));
  int64_t copy_ptr = get_native_ptr(rt, copy_obj);
  CHECK_EQ(call(rt, bm, "nativeSameAs", "(JJ)Z", Args().j(ptr).j(copy_ptr)), uint64_t(1));

  call(rt, bm, "nativeSetPixel", "(JIII)V", Args().j(copy_ptr).i(0).i(0).i(0));
  CHECK_EQ(call(rt, bm, "nativeSameAs", "(JJ)Z", Args().j(ptr).j(copy_ptr)), uint64_t(0));

  // nativeGetNativeFinalizer returns non-zero
  CHECK(call(rt, bm, "nativeGetNativeFinalizer", "()J") != 0);

  // Recycle
  call(rt, bm, "nativeRecycle", "(J)V", Args().j(ptr));
  call(rt, bm, "nativeRecycle", "(J)V", Args().j(copy_ptr));
}

TEST(graphics_image_decoder_png_and_ninepatch) {
  Vm vm;
  rt::Runtime& rt = *vm.rt;
  const char* id = "Landroid/graphics/ImageDecoder;";
  const char* bm = "Landroid/graphics/Bitmap;";

  auto patch = make_dummy_9patch_chunk();
  auto png = make_png_rgba(4, 4, 0xFF4488CC, patch);

  rt::Handle png_arr = rt.new_array("[B", uint32_t(png.size()));
  std::memcpy(rt.array(png_arr, "byte[]").data.data(), png.data(), png.size());

  // nCreate([BIIZLandroid/graphics/ImageDecoder$Source;)
  rt::Handle dec_obj = rt::Handle(call(rt, id, "nCreate", "([BIIZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
                                       Args().h(png_arr).i(0).i(int32_t(png.size())).z(false).h(rt::kNull)));
  CHECK(dec_obj != rt::kNull);
  int64_t dec_ptr = get_native_ptr(rt, dec_obj, "Landroid/graphics/ImageDecoder;");
  CHECK_EQ(uint16_t(uint64_t(dec_ptr) >> 48), uint16_t(0x110D));

  // Size query
  rt::Handle size_obj = rt::Handle(call(rt, id, "nGetSampledSize", "(JI)Landroid/util/Size;", Args().j(dec_ptr).i(1)));
  CHECK_EQ(get_field_i32(rt, size_obj, "mWidth"), 4);
  CHECK_EQ(get_field_i32(rt, size_obj, "mHeight"), 4);

  // Decode to Bitmap
  rt::Handle bm_obj = rt::Handle(call(rt, id, "nDecodeBitmap",
                                      "(JLandroid/graphics/ImageDecoder;IILandroid/graphics/Rect;IIJJ)Landroid/graphics/Bitmap;",
                                      Args().j(dec_ptr).h(dec_obj).i(4).i(4).h(rt::kNull).i(0).i(0).j(0).j(0)));
  CHECK(bm_obj != rt::kNull);
  int64_t bm_ptr = get_native_ptr(rt, bm_obj, "Landroid/graphics/Bitmap;");
  CHECK_EQ(uint32_t(call(rt, bm, "nativeGetPixel", "(JII)I", Args().j(bm_ptr).i(1).i(1))), uint32_t(0xFF4488CC));

  // Verify 9-patch chunk was attached to Bitmap
  rt::Object& bm_o = rt.nonnull(bm_obj, "Bitmap");
  rt::Field* f_chunk = bm_o.klass->declared_field("mNinePatchChunk", "[B");
  CHECK(f_chunk != nullptr);
  rt::Handle chunk_h = bm_o.fields.at(f_chunk->slot);
  CHECK(chunk_h != rt::kNull);
  CHECK_EQ(rt.array(chunk_h, "byte[]").length, uint32_t(patch.size()));

  // Close decoder
  call(rt, id, "nClose", "(J)V", Args().j(dec_ptr));
}
