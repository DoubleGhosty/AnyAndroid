#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"
#include "text/font_collection.h"
#include "text/layout.h"
#include "vfs/vfs.h"

namespace aa::bridge {

template <typename P>
P& peer_or_throw(rt::Runtime& rt, rt::Handle h, const char* what) {
  rt::Object& o = rt.nonnull(h, what);
  auto* p = dynamic_cast<P*>(o.peer.get());
  if (!p) {
    rt.throw_new("Ljava/lang/IllegalStateException;",
                 "[AnyAndroid] état natif absent pour " + o.klass->java_name() + " (constructeur non appelé ?)");
  }
  return *p;
}

struct BoxPeer final : rt::NativePeer {
  char type = 'I';  // primitive descriptor
  uint64_t bits = 0;
};

rt::Handle box(rt::Runtime& rt, char type, uint64_t bits);
// Reads a boxed number/char/boolean as int64 / double (for String.format).
bool unbox(rt::Runtime& rt, rt::Handle h, char& type, uint64_t& bits);
std::u16string java_format(rt::Runtime& rt, const std::u16string& fmt, rt::Handle args_array);
bool java_equals(rt::Runtime& rt, rt::Handle a, rt::Handle b);

void register_java_lang(rt::NativeRegistry& r, Host& host);
void register_java_util(rt::NativeRegistry& r, Host& host);
void register_java_reflect(rt::NativeRegistry& r, Host& host);
void register_android_app(rt::NativeRegistry& r, Host& host);
void register_android_widget(rt::NativeRegistry& r, Host& host);
void register_android_res(rt::NativeRegistry& r, Host& host);
void register_libcore_charsets(rt::NativeRegistry& r);
void register_libcore_files(rt::NativeRegistry& r);
void register_framework_natives(rt::NativeRegistry& r);
void register_framework_graphics(rt::NativeRegistry& r);
// Paint (framework_paint.cpp), registered by register_framework_graphics.
void register_framework_paint(rt::NativeRegistry& r);
// Path (framework_path.cpp), registered by register_framework_graphics.
void register_framework_path(rt::NativeRegistry& r);
// The verbs (SkPath::Verb), point coordinates and conic weights of a Path
// (unit tests).
void path_contents(rt::Runtime& rt, int64_t handle, std::vector<uint8_t>& verbs, std::vector<float>& coords,
                   std::vector<float>& weights);
// The same, with the fill type (Path.FillType's native values, in
// SkPathFillType's order), read under one lock for drawing.
struct PathSnapshot {
  std::vector<uint8_t> verbs;
  std::vector<float> coords, weights;
  int32_t fill_type = 0;
};
PathSnapshot path_snapshot(rt::Runtime& rt, int64_t handle);
// An SVG path string as HWUI's PathParser keeps it (framework_path.cpp), shared
// with VectorDrawable: verb letters, the float count of each, the floats.
struct SvgPathData {
  std::vector<char> verbs;
  std::vector<size_t> sizes;
  std::vector<float> points;
  bool operator==(const SvgPathData&) const = default;
};
// Parses s[0, length) as PathParser::getPathDataFromAsciiString; on failure,
// false and HWUI's message.
bool parse_svg_path(const std::string& s, size_t length, SvgPathData& out, std::string& error);
// The contents of a PathParser.PathData handle (IllegalStateException if invalid).
SvgPathData svg_path_data(rt::Runtime& rt, int64_t handle);
// Matrix (framework_matrix.cpp), registered by register_framework_graphics.
void register_framework_matrix(rt::NativeRegistry& r);
// SkMatrix (framework_matrix.cpp), shared with Canvas's current matrix.
namespace skmatrix {
using Values = std::array<float, 9>;  // Matrix.getValues's order
Values identity();
void pre_translate(Values& m, float dx, float dy);
void pre_scale(Values& m, float sx, float sy);
void pre_rotate(Values& m, float degrees);
void pre_skew(Values& m, float kx, float ky);
// SkMatrix::preRotate and preScale about a pivot.
void pre_rotate(Values& m, float degrees, float px, float py);
void pre_scale(Values& m, float sx, float sy, float px, float py);
void pre_concat(Values& m, const Values& other);
bool invert(const Values& m, Values& out);
bool is_scale_translate(const Values& m);
// Maps {left, top, right, bottom} in place, sorted; true when the result is
// exactly the mapped rectangle.
bool map_rect(const Values& m, std::array<float, 4>& rect);
// A Matrix handle's values, and their replacement (IllegalStateException if invalid).
Values of(rt::Runtime& rt, int64_t handle);
void assign(rt::Runtime& rt, int64_t handle, const Values& values);
}  // namespace skmatrix
// Images: ImageDecoder, Bitmap, NinePatch (framework_images.cpp), registered by register_framework_graphics.
void register_framework_images(rt::NativeRegistry& r);
// The divisions and colours of a NinePatch chunk handle (Res_png_9patch),
// IllegalStateException if invalid.
struct NinePatchDivs {
  std::vector<int32_t> x_divs, y_divs;
  std::vector<uint32_t> colors;
};
NinePatchDivs nine_patch_divs(rt::Runtime& rt, int64_t chunk);
// Pixels of a Bitmap or of a window buffer, shared with the canvases that
// draw into them: Skia's N32 layout (RGBA bytes, as Android), rows packed.
struct PixelBuffer {
  std::vector<uint32_t> data;
};
// A drawing target: width x height pixels, premultiplied unless said
// otherwise, opaque when its format has no alpha (SkImageInfo::isOpaque).
struct PixelTarget {
  std::shared_ptr<PixelBuffer> pixels;
  int32_t width = 0, height = 0;
  bool opaque = false, premultiplied = true;
  std::shared_ptr<void> owner;  // the Bitmap, when there is one
};
// The pixels of a Bitmap handle (IllegalStateException if invalid).
PixelTarget bitmap_pixels(rt::Runtime& rt, int64_t handle);
// Shader, ColorFilter, MaskFilter and PathEffect (framework_effects.cpp),
// registered by register_framework_graphics.
void register_framework_effects(rt::NativeRegistry& r);
// Canvas state (framework_canvas.cpp), registered by register_framework_graphics.
void register_framework_canvas(rt::NativeRegistry& r);
// Points a Java Canvas at a window buffer (none: no pixels), clipped to
// dirty (left, top, right, bottom) when given, as Surface's nativeLockCanvas
// sets an SkBitmap on the native canvas.
void canvas_target_surface(rt::Runtime& rt, rt::Handle canvas, const PixelTarget& target, const int32_t* dirty);
// Region and RegionIterator (framework_region.cpp), registered by register_framework_graphics.
void register_framework_region(rt::NativeRegistry& r);
// The rectangles of a Region handle (left, top, right, bottom), top to bottom
// and left to right (IllegalStateException if invalid).
std::vector<std::array<int32_t, 4>> region_rects(rt::Runtime& rt, int64_t handle);
// The transfer parameters (a, b, c, d, e, f, g) and RGB to XYZ (D50) matrix,
// in Java's order, of a ColorSpace.Rgb native handle; false if invalid.
bool color_space_params(int64_t handle, std::array<float, 7>& transfer, std::array<float, 9>& to_xyz);
// VectorDrawable and VirtualRefBasePtr (framework_vector_drawable.cpp), registered by register_framework_graphics.
void register_framework_vector_drawable(rt::NativeRegistry& r);
// SurfaceControl, its Transaction and Surface (framework_surface.cpp), registered by register_framework_graphics.
void register_framework_surface(rt::NativeRegistry& r);
// Runs fn on the thread of a MessageQueue (its mPtr), from its nativePollOnce,
// once `deadline` (monotonic nanoseconds) passes, as Looper::pollOnce runs the
// callbacks of watched descriptors (framework_natives.cpp). Wakes the poller.
void post_queue_callback(rt::Runtime& rt, int64_t queue_ptr, int64_t deadline, std::function<void(rt::Runtime&)> fn);
// Returns pointer to open AssetStream bytes, or nullptr.
const std::vector<uint8_t>* asset_data(int64_t handle);
// Typeface and fonts (framework_fonts.cpp), registered by register_framework_graphics.
void register_framework_fonts(rt::NativeRegistry& r);
// Minikin's locale list ids (LocaleListCache): 0 is the empty list, then ids
// in order of first use of a list of language tags (a bounded registry:
// IllegalArgumentException when full).
int32_t minikin_locale_list_id(rt::Runtime& rt, const std::u16string& tags);
bool minikin_locale_list_valid(int32_t id);
// HWUI's Typeface::resolveDefault: the minikin collection and style of a
// Typeface handle (0: the default typeface).
struct TypefaceInfo {
  std::shared_ptr<const text::FontCollection> collection;
  text::FontStyle style;
  int32_t api_style = 0;
};
TypefaceInfo resolve_typeface(rt::Runtime& rt, int64_t handle);
// Installs the text engine's Unicode services over the ICU4J of the libcore
// (text_services.cpp), answering through `rt`. Called before using the engine.
void ensure_text_services(rt::Runtime& rt);
// The minikin paint of a Paint handle (MinikinUtils::prepareMinikinPaint, in
// framework_paint.cpp) and the locale list id of the paint.
text::Paint minikin_paint_of(rt::Runtime& rt, int64_t paint);
uint32_t paint_locale_list_id(rt::Runtime& rt, int64_t paint);
// android.graphics.text.MeasuredText and LineBreaker (framework_text.cpp),
// registered by register_framework_graphics.
void register_framework_text(rt::NativeRegistry& r);
void register_parcel_natives(rt::NativeRegistry& r);
// libbinder's int32 and String8 in a Java Parcel (its mNativePtr), for the
// natives that marshal their own state; a read past the end clears *ok.
int32_t parcel_read_i32(rt::Runtime& rt, rt::Handle parcel, bool* ok);
void parcel_write_i32(rt::Runtime& rt, rt::Handle parcel, int32_t v);
std::string parcel_read_string8(rt::Runtime& rt, rt::Handle parcel, bool* ok);
void parcel_write_string8(rt::Runtime& rt, rt::Handle parcel, const std::string& v);
// KeyCharacterMap, KeyEvent (framework_input.cpp), registered by register_framework_natives.
void register_framework_input(rt::NativeRegistry& r);
void register_appmem_natives(rt::NativeRegistry& r);
void register_resource_natives(rt::NativeRegistry& r);
// Android system properties of the emulated device (build.prop and
// AnyAndroid's identity), as android.os.SystemProperties sees them.
std::optional<std::string> system_property(const std::string& key);
// File system of the emulated process (/dev only when none is configured).
vfs::Vfs& libcore_vfs();

// Descriptor I/O of the emulated process (libcore_files.cpp): 0-2 are the
// host's standard streams, the other descriptors go through the Vfs. Errors
// are thrown as vfs::VfsError, for the caller to report the way its Java API
// expects (ErrnoException, UnixException, IOException).
size_t read_fd(int32_t fd, std::span<uint8_t> out);
size_t write_fd(int32_t fd, std::span<const uint8_t> data);
vfs::VStat fstat_fd(int32_t fd);
// Private copy of `length` bytes of a file from `offset`, in emulated native
// memory (mmap without MAP_SHARED); ENOMEM beyond 2 GiB, checked first. A
// read-only mapping is read when first touched, as the pages of a real one.
int64_t map_fd_copy(rt::Runtime& rt, int32_t fd, uint64_t offset, uint64_t length, bool writable);

void register_libcore_nio(rt::NativeRegistry& r);
void register_libcore_zip(rt::NativeRegistry& r);
void register_libcore_crypto(rt::NativeRegistry& r);
void register_libcore_dex(rt::NativeRegistry& r);
void register_libcore_regex(rt::NativeRegistry& r);

// Class loaders (libcore_dex.cpp, ADR-006 J2). The defining loader of a class
// as Class.getClassLoader() reports it (boot space: BootClassLoader, or null
// when boot_as_null, as ART's internal view).
rt::Handle class_loader_of(rt::Runtime& rt, rt::Class* cls, bool boot_as_null);
// VMClassLoader.findLoadedClass: the class `loader` would answer for a binary
// name without running Java code, or nullptr.
rt::Class* find_loaded_class(rt::Runtime& rt, rt::Handle loader, const std::string& name);
// Class.classForName: through `loader` (null or BootClassLoader: boot space).
// Throws ClassNotFoundException.
rt::Class* class_for_name(rt::Runtime& rt, rt::Handle name, rt::Handle loader);

// Helpers of the libcore natives (libcore_natives.cpp).
rt::Field* find_field(rt::Class* cls, std::string_view name, std::string_view type);
int32_t fd_of(rt::Runtime& rt, rt::Handle file_descriptor);
// Creates an object of a (bytecode) class by running one of its constructors.
rt::Handle construct(rt::Runtime& rt, std::string_view cls, std::string_view sig, std::vector<uint32_t> args);
// Throws android.system.ErrnoException(function, err).
[[noreturn]] void throw_errno(rt::Runtime& rt, const char* function, int32_t err);
// strerror(3) text of a Linux errno value, as bionic words it.
std::string strerror_text(int32_t err);
rt::Handle string_array(rt::Runtime& rt, const std::vector<std::string>& items);
// Native "free functions" of libcore.util.NativeAllocationRegistry: the
// getNativeFinalizer() natives return these bounded ids instead of C function
// pointers, and applyFreeFunction(id, ptr) calls the matching function.
int64_t register_free_function(std::function<void(int64_t)> fn);

}  // namespace aa::bridge
