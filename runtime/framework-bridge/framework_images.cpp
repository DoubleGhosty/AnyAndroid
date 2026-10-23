// android.graphics.ImageDecoder, Bitmap and NinePatch natives (G3, ADR-006 J4),
// each signature checked against the framework.jar of Android 17.
//
// - PNG decoder: IHDR, PLTE, tRNS, IDAT (inflated via miniz), unfiltering
//   (None, Sub, Up, Average, Paeth) into 32-bit ARGB_8888 pixels.
// - 9-patch chunks: npTc (Res_png_9patch layout, padding, stretch divs, colors),
//   npLb (optical insets / layout bounds) and npOl (outlines).
// - Native handles live in bounded handle tables (handles.h):
//   0x110B (Bitmap), 0x110C (NinePatch chunk), 0x110D (ImageDecoder).
#include "framework-bridge/internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/native_memory.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "third_party/miniz/miniz.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

inline uint32_t lo(uint64_t v) { return uint32_t(v & 0xffffffff); }
inline uint32_t hi(uint64_t v) { return uint32_t((v >> 32) & 0xffffffff); }
inline uint32_t float_bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// -----------------------------------------------------------------------------
// PNG and NinePatch Chunk Data Structures
// -----------------------------------------------------------------------------

struct ResPng9Patch {
  int8_t was_deserialized = 0;
  uint8_t num_x_divs = 0;
  uint8_t num_y_divs = 0;
  uint8_t num_colors = 0;
  int32_t padding_left = 0;
  int32_t padding_right = 0;
  int32_t padding_top = 0;
  int32_t padding_bottom = 0;
  std::vector<int32_t> x_divs;
  std::vector<int32_t> y_divs;
  std::vector<uint32_t> colors;
};

struct PngDecoded {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> pixels;  // 0xAARRGGBB
  std::vector<uint8_t> nptc_chunk;
  bool has_nptc = false;
  ResPng9Patch patch;
  bool has_optical = false;
  int32_t optical_left = 0, optical_top = 0, optical_right = 0, optical_bottom = 0;
  bool has_outline = false;
  int32_t outline_left = 0, outline_top = 0, outline_right = 0, outline_bottom = 0;
  float outline_radius = 0.0f;
  int32_t outline_alpha = 0;
};

static uint8_t paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = std::abs(p - a);
  int pb = std::abs(p - b);
  int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return uint8_t(a);
  if (pb <= pc) return uint8_t(b);
  return uint8_t(c);
}

bool parse_9patch_chunk(const uint8_t* data, size_t size, ResPng9Patch& out) {
  if (size < 32) return false;
  out.was_deserialized = int8_t(data[0]);
  out.num_x_divs = data[1];
  out.num_y_divs = data[2];
  out.num_colors = data[3];

  size_t expected = 32 + size_t(out.num_x_divs) * 4 + size_t(out.num_y_divs) * 4 + size_t(out.num_colors) * 4;
  if (size != expected) return false;

  auto read_i32 = [](const uint8_t* p) -> int32_t {
    return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
  };
  auto read_u32 = [](const uint8_t* p) -> uint32_t {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  };

  out.padding_left = read_i32(data + 12);
  out.padding_right = read_i32(data + 16);
  out.padding_top = read_i32(data + 20);
  out.padding_bottom = read_i32(data + 24);

  size_t off = 32;
  out.x_divs.resize(out.num_x_divs);
  for (uint8_t i = 0; i < out.num_x_divs; ++i, off += 4) out.x_divs[i] = read_i32(data + off);

  out.y_divs.resize(out.num_y_divs);
  for (uint8_t i = 0; i < out.num_y_divs; ++i, off += 4) out.y_divs[i] = read_i32(data + off);

  out.colors.resize(out.num_colors);
  for (uint8_t i = 0; i < out.num_colors; ++i, off += 4) out.colors[i] = read_u32(data + off);

  return true;
}

bool decode_png(const uint8_t* data, size_t size, PngDecoded& out) {
  if (size < 8) return false;
  static const uint8_t kMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(data, kMagic, 8) != 0) return false;

  size_t off = 8;
  uint32_t width = 0, height = 0;
  uint8_t bit_depth = 0, color_type = 0;
  std::vector<uint8_t> idat;
  std::vector<uint8_t> plte;
  std::vector<uint8_t> trns;

  while (off + 8 <= size) {
    uint32_t len = (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
                   (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
    off += 4;
    const uint8_t* type = data + off;
    off += 4;
    if (off + len + 4 > size) return false;
    const uint8_t* chunk_data = data + off;

    if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
      width = (uint32_t(chunk_data[0]) << 24) | (uint32_t(chunk_data[1]) << 16) |
              (uint32_t(chunk_data[2]) << 8) | uint32_t(chunk_data[3]);
      height = (uint32_t(chunk_data[4]) << 24) | (uint32_t(chunk_data[5]) << 16) |
               (uint32_t(chunk_data[6]) << 8) | uint32_t(chunk_data[7]);
      bit_depth = chunk_data[8];
      color_type = chunk_data[9];
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      plte.assign(chunk_data, chunk_data + len);
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      trns.assign(chunk_data, chunk_data + len);
    } else if (std::memcmp(type, "npTc", 4) == 0) {
      out.nptc_chunk.assign(chunk_data, chunk_data + len);
      out.has_nptc = parse_9patch_chunk(chunk_data, len, out.patch);
    } else if (std::memcmp(type, "npLb", 4) == 0 && len == 16) {
      out.has_optical = true;
      auto read_i32 = [](const uint8_t* p) -> int32_t {
        return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
      };
      out.optical_left = read_i32(chunk_data);
      out.optical_top = read_i32(chunk_data + 4);
      out.optical_right = read_i32(chunk_data + 8);
      out.optical_bottom = read_i32(chunk_data + 12);
    } else if (std::memcmp(type, "npOl", 4) == 0 && len == 24) {
      out.has_outline = true;
      auto read_i32 = [](const uint8_t* p) -> int32_t {
        return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
      };
      out.outline_left = read_i32(chunk_data);
      out.outline_top = read_i32(chunk_data + 4);
      out.outline_right = read_i32(chunk_data + 8);
      out.outline_bottom = read_i32(chunk_data + 12);
      std::memcpy(&out.outline_radius, chunk_data + 16, 4);
      out.outline_alpha = read_i32(chunk_data + 20);
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), chunk_data, chunk_data + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    off += len + 4;  // skip data + crc
  }

  if (width == 0 || height == 0 || idat.empty()) return false;
  if (bit_depth != 8) return false;

  int bpp = 0;
  if (color_type == 0) bpp = 1;       // Grayscale
  else if (color_type == 2) bpp = 3;  // RGB
  else if (color_type == 3) bpp = 1;  // Indexed
  else if (color_type == 4) bpp = 2;  // Gray + Alpha
  else if (color_type == 6) bpp = 4;  // RGBA
  else return false;

  size_t row_bytes = width * size_t(bpp);
  size_t decompressed_size = height * (1 + row_bytes);
  std::vector<uint8_t> raw(decompressed_size);
  mz_ulong dest_len = (mz_ulong)decompressed_size;

  int status = mz_uncompress(raw.data(), &dest_len, idat.data(), (mz_ulong)idat.size());
  if (status != MZ_OK || dest_len != decompressed_size) return false;

  out.width = width;
  out.height = height;
  out.pixels.resize(width * height);

  std::vector<uint8_t> prev_row(row_bytes, 0);
  std::vector<uint8_t> cur_row(row_bytes, 0);

  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* scanline = raw.data() + y * (1 + row_bytes);
    uint8_t filter = scanline[0];
    const uint8_t* src = scanline + 1;

    for (size_t x = 0; x < row_bytes; ++x) {
      uint8_t a = (x >= size_t(bpp)) ? cur_row[x - bpp] : 0;
      uint8_t b = prev_row[x];
      uint8_t c = (x >= size_t(bpp)) ? prev_row[x - bpp] : 0;
      uint8_t val = src[x];

      switch (filter) {
        case 0: cur_row[x] = val; break;
        case 1: cur_row[x] = uint8_t(val + a); break;
        case 2: cur_row[x] = uint8_t(val + b); break;
        case 3: cur_row[x] = uint8_t(val + ((a + b) / 2)); break;
        case 4: cur_row[x] = uint8_t(val + paeth(a, b, c)); break;
        default: return false;
      }
    }

    for (uint32_t x = 0; x < width; ++x) {
      uint32_t pixel = 0;
      if (color_type == 6) {  // RGBA
        uint8_t r = cur_row[x * 4];
        uint8_t g = cur_row[x * 4 + 1];
        uint8_t b = cur_row[x * 4 + 2];
        uint8_t a = cur_row[x * 4 + 3];
        pixel = (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
      } else if (color_type == 2) {  // RGB
        uint8_t r = cur_row[x * 3];
        uint8_t g = cur_row[x * 3 + 1];
        uint8_t b = cur_row[x * 3 + 2];
        pixel = 0xFF000000 | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
      } else if (color_type == 0) {  // Gray
        uint8_t v = cur_row[x];
        pixel = 0xFF000000 | (uint32_t(v) << 16) | (uint32_t(v) << 8) | uint32_t(v);
      } else if (color_type == 4) {  // Gray + Alpha
        uint8_t v = cur_row[x * 2];
        uint8_t a = cur_row[x * 2 + 1];
        pixel = (uint32_t(a) << 24) | (uint32_t(v) << 16) | (uint32_t(v) << 8) | uint32_t(v);
      } else if (color_type == 3) {  // Indexed
        uint8_t idx = cur_row[x];
        if (size_t(idx) * 3 + 2 < plte.size()) {
          uint8_t r = plte[idx * 3];
          uint8_t g = plte[idx * 3 + 1];
          uint8_t b = plte[idx * 3 + 2];
          uint8_t a = (size_t(idx) < trns.size()) ? trns[idx] : 0xFF;
          pixel = (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
      }
      out.pixels[y * width + x] = pixel;
    }
    prev_row = cur_row;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Native States and Tables
// -----------------------------------------------------------------------------

struct BitmapState {
  mutable std::mutex lock;
  int32_t width = 0;
  int32_t height = 0;
  int32_t density = 0;
  int32_t config = 5;  // Config.ARGB_8888 (nativeInt: 5)
  bool is_mutable = true;
  bool is_premultiplied = true;
  bool has_alpha = true;
  bool has_mipmap = false;
  int32_t generation_id = 1;
  // Skia's N32 (RGBA bytes), premultiplied unless !is_premultiplied, shared
  // with the canvases that draw into it.
  std::shared_ptr<PixelBuffer> pixels = std::make_shared<PixelBuffer>();
  std::vector<uint8_t> nine_patch_chunk;
  int64_t color_space_ptr = 0;
  bool recycled = false;

  void copy_from(const BitmapState& o) {
    width = o.width;
    height = o.height;
    density = o.density;
    config = o.config;
    is_mutable = o.is_mutable;
    is_premultiplied = o.is_premultiplied;
    has_alpha = o.has_alpha;
    has_mipmap = o.has_mipmap;
    generation_id = o.generation_id;
    pixels = std::make_shared<PixelBuffer>(*o.pixels);
    nine_patch_chunk = o.nine_patch_chunk;
    color_space_ptr = o.color_space_ptr;
    recycled = o.recycled;
  }

  // The pixels as Skia sees them: sRGB (as Android's bitmaps by default),
  // opaque when they have no alpha.
  SkImageInfo info() const {
    SkAlphaType at = !has_alpha ? kOpaque_SkAlphaType : is_premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
    return SkImageInfo::Make(width, height, kN32_SkColorType, at, SkColorSpace::MakeSRGB());
  }
  SkPixmap pixmap() const { return SkPixmap(info(), pixels->data.data(), size_t(width) * 4); }
  void allocate() { pixels = std::make_shared<PixelBuffer>(PixelBuffer{std::vector<uint32_t>(size_t(width) * size_t(height), 0)}); }
};

// SkColor values (unpremultiplied ARGB, as Java's int colors), as Android's
// GraphicsJNI reads and writes them: kBGRA_8888 unpremultiplied sRGB.
SkImageInfo color_info(int32_t w, int32_t h) {
  return SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
}

// Bitmap_getPixels: the colors of [x, x+w) x [y, y+h) into dst (row stride).
void read_colors(const BitmapState& b, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t* dst, size_t stride) {
  if (w <= 0 || h <= 0) return;
  b.pixmap().readPixels(color_info(w, h), dst, stride * 4, x, y);
}

// GraphicsJNI::SetPixels: colors written at (x, y) with the bitmap's format.
void write_colors(BitmapState& b, int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t* src, size_t stride) {
  if (w <= 0 || h <= 0) return;
  SkPixmap colors(color_info(w, h), src, stride * 4);
  SkPixmap dst = b.pixmap();
  colors.readPixels(dst.info().makeWH(w, h), dst.writable_addr(x, y), dst.rowBytes());
}

struct NinePatchChunkState {
  std::vector<uint8_t> chunk;
  ResPng9Patch patch;
};

struct ImageDecoderState {
  PngDecoded decoded;
};

struct ImageTables {
  HandleTable<BitmapState> bitmaps{0x110B};
  HandleTable<NinePatchChunkState> nine_patch_chunks{0x110C};
  HandleTable<ImageDecoderState> decoders{0x110D};
};

ImageTables& img_tables() {
  static auto* t = new ImageTables;
  return *t;
}

std::shared_ptr<BitmapState> bitmap(Runtime& rt, int64_t h) {
  auto b = img_tables().bitmaps.get(h);
  if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Bitmap invalide ou recyclé");
  return b;
}

std::shared_ptr<ImageDecoderState> decoder(Runtime& rt, int64_t h) {
  auto d = img_tables().decoders.get(h);
  if (!d) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] ImageDecoder invalide ou fermé");
  return d;
}

// -----------------------------------------------------------------------------
// Java Helpers
// -----------------------------------------------------------------------------

Handle create_rect(Runtime& rt, int32_t left, int32_t top, int32_t right, int32_t bottom) {
  return construct(rt, "Landroid/graphics/Rect;", "(IIII)V",
                   {uint32_t(left), uint32_t(top), uint32_t(right), uint32_t(bottom)});
}

Handle create_nine_patch_insets(Runtime& rt, int32_t ol, int32_t ot, int32_t oright, int32_t ob,
                                int32_t opt_l, int32_t opt_t, int32_t opt_r, int32_t opt_b,
                                float radius, int32_t alpha, float scale = 1.0f) {
  return construct(rt, "Landroid/graphics/NinePatch$InsetStruct;", "(IIIIIIIIFIF)V",
                   {uint32_t(ol), uint32_t(ot), uint32_t(oright), uint32_t(ob),
                    uint32_t(opt_l), uint32_t(opt_t), uint32_t(opt_r), uint32_t(opt_b),
                    float_bits(radius), uint32_t(alpha), float_bits(scale)});
}

Handle create_bitmap_object(Runtime& rt, int64_t native_ptr, int32_t width, int32_t height, int32_t density,
                            bool is_mutable, Handle nine_patch_chunk, Handle nine_patch_insets) {
  Handle h = construct(rt, "Landroid/graphics/Bitmap;",
                       "(JIIIZ[BLandroid/graphics/NinePatch$InsetStruct;)V",
                       {lo(native_ptr), hi(native_ptr), uint32_t(width), uint32_t(height), uint32_t(density),
                        uint32_t(is_mutable ? 1 : 0), nine_patch_chunk, nine_patch_insets});
  if (h != kNull) {
    if (rt::Object* o = rt.object(h)) {
      if (rt::Field* f = o->klass->declared_field("mNativePtr", "J")) {
        o->fields.at(f->slot) = lo(native_ptr);
        o->fields.at(f->slot + 1) = hi(native_ptr);
      }
      if (nine_patch_chunk != kNull) {
        if (rt::Field* f = o->klass->declared_field("mNinePatchChunk", "[B")) {
          o->fields.at(f->slot) = nine_patch_chunk;
        }
      }
    }
  }
  return h;
}

Handle build_byte_array(Runtime& rt, const std::vector<uint8_t>& vec) {
  if (vec.empty()) return kNull;
  Handle h = rt.new_array("[B", int32_t(vec.size()));
  std::memcpy(rt.array(h, "byte[]").data.data(), vec.data(), vec.size());
  return h;
}

// -----------------------------------------------------------------------------
// NinePatch Natives
// -----------------------------------------------------------------------------

void register_nine_patch(rt::NativeRegistry& r) {
  const char* np = "Landroid/graphics/NinePatch;";

  r.bind(np, "isNinePatchChunk", "([B)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) return rt::ret_bool(false);
    rt::ArrayObject& arr = rt.array(a.h(0), "byte[]");
    ResPng9Patch patch;
    return rt::ret_bool(parse_9patch_chunk(arr.data.data(), arr.length, patch));
  });

  r.bind(np, "validateNinePatchChunk", "([B)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_new("Ljava/lang/IllegalArgumentException;", "chunk cannot be null");
    rt::ArrayObject& arr = rt.array(a.h(0), "byte[]");
    auto chunk_state = std::make_shared<NinePatchChunkState>();
    chunk_state->chunk.assign(arr.data.data(), arr.data.data() + arr.length);
    if (!parse_9patch_chunk(chunk_state->chunk.data(), chunk_state->chunk.size(), chunk_state->patch)) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "Array too small for NinePatch.");
    }
    return uint64_t(img_tables().nine_patch_chunks.add(std::move(chunk_state)));
  });

  r.bind(np, "nativeFinalize", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    img_tables().nine_patch_chunks.remove(a.i64(0));
    return 0;
  });

  r.bind(np, "nativeGetTransparentRegion", "(JJLandroid/graphics/Rect;)J",
         [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
}

// -----------------------------------------------------------------------------
// Bitmap Natives
// -----------------------------------------------------------------------------

// A Color long (android.graphics.Color): sRGB as ARGB in the high 32 bits
// (color space id 0), otherwise half-float red, green, blue and a 10-bit
// alpha; the components are taken as sRGB (GraphicsJNI::convertColorLong).
SkColor4f color4f_of_long(int64_t color) {
  uint64_t c = uint64_t(color);
  if ((c & 0x3f) == 0) return SkColor4f::FromColor(SkColor(uint32_t(c >> 32)));
  auto half = [](uint16_t h) {
    int32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), e - 25);
    return (h & 0x8000) ? -v : v;
  };
  return SkColor4f{half(uint16_t(c >> 48)), half(uint16_t(c >> 32)), half(uint16_t(c >> 16)),
                   float((c >> 6) & 0x3ff) / 1023.0f};
}

void register_bitmap(rt::NativeRegistry& r) {
  const char* bm = "Landroid/graphics/Bitmap;";
  static const int64_t bm_free = register_free_function([](int64_t h) { img_tables().bitmaps.remove(h); });

  r.bind(bm, "nativeGetNativeFinalizer", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return rt::ret_long(bm_free);
  });

  r.bind(bm, "nativeGetNativeNoop", "()J", [](Runtime&, NativeArgs&) -> uint64_t {
    return 0;
  });

  r.bind(bm, "nativeCreate", "([IIIIIIZJ)Landroid/graphics/Bitmap;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t offset = a.i32(1), stride = a.i32(2), width = a.i32(3), height = a.i32(4);
           int32_t config = a.i32(5);
           bool is_mutable = a.z(6);
           int64_t cs_ptr = a.i64(7);

           if (width <= 0 || height <= 0) {
             rt.throw_new("Ljava/lang/IllegalArgumentException;", "width and height must be > 0");
           }

           auto state = std::make_shared<BitmapState>();
           state->width = width;
           state->height = height;
           state->config = config;
           state->is_mutable = is_mutable;
           state->color_space_ptr = cs_ptr;
           state->allocate();

           if (a.h(0) != kNull) {
             rt::ArrayObject& colors = rt.array(a.h(0), "int[]");
             // Bitmap.createBitmap checked the offset and stride against the array.
             if (offset < 0 || stride < width ||
                 int64_t(offset) + int64_t(stride) * (height - 1) + width > int64_t(colors.length)) {
               rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
             }
             const uint32_t* src = reinterpret_cast<const uint32_t*>(colors.data.data()) + offset;
             write_colors(*state, 0, 0, width, height, src, size_t(stride));
           }

           int64_t handle = img_tables().bitmaps.add(std::move(state));
           return create_bitmap_object(rt, handle, width, height, 0, is_mutable, kNull, kNull);
         });

  // Gainmaps (HDR) are not kept: a bitmap never has one. Canvas(Bitmap)
  // clears it before drawing into the bitmap (RippleDrawable's mask).
  r.bind(bm, "nativeSetGainmap", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    bitmap(rt, a.i64(0));
    return 0;
  }, S::kPartial, "pas de gainmap (HDR) : ignorée");
  r.bind(bm, "nativeHasGainmap", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    bitmap(rt, a.i64(0));
    return rt::ret_bool(false);
  });
  r.bind(bm, "nativeExtractGainmap", "(J)Landroid/graphics/Gainmap;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    bitmap(rt, a.i64(0));
    return rt::ret_obj(kNull);
  });

  r.bind(bm, "nativeRowBytes", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_int(b->width * 4);
  });

  r.bind(bm, "nativeConfig", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_int(b->config);
  });

  r.bind(bm, "nativeHasAlpha", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_bool(b->has_alpha);
  });

  r.bind(bm, "nativeIsPremultiplied", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_bool(b->is_premultiplied);
  });

  r.bind(bm, "nativeSetPremultiplied", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->is_premultiplied = a.z(2);
    return 0;
  });

  r.bind(bm, "nativeSetHasAlpha", "(JZZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->has_alpha = a.z(2);
    return 0;
  });

  r.bind(bm, "nativeHasMipMap", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_bool(b->has_mipmap);
  });

  r.bind(bm, "nativeSetHasMipMap", "(JZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->has_mipmap = a.z(2);
    return 0;
  });

  r.bind(bm, "nativeSameAs", "(JJ)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b1 = bitmap(rt, a.i64(0)), b2 = bitmap(rt, a.i64(2));
    if (b1 == b2) return rt::ret_bool(true);
    std::lock_guard<std::mutex> g1(b1->lock), g2(b2->lock);
    if (b1->width != b2->width || b1->height != b2->height || b1->config != b2->config) {
      return rt::ret_bool(false);
    }
    return rt::ret_bool(b1->pixels->data == b2->pixels->data);
  });

  r.bind(bm, "nativePrepareToDraw", "(J)V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });

  r.bind(bm, "nativeGetPixel", "(JII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    int32_t x = a.i32(2), y = a.i32(3);
    std::lock_guard<std::mutex> guard(b->lock);
    if (x < 0 || x >= b->width || y < 0 || y >= b->height) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "coordinates out of range");
    }
    uint32_t color = 0;
    read_colors(*b, x, y, 1, 1, &color, 1);
    return rt::ret_int(int32_t(color));
  });

  r.bind(bm, "nativeSetPixel", "(JIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    int32_t x = a.i32(2), y = a.i32(3), color = a.i32(4);
    std::lock_guard<std::mutex> guard(b->lock);
    if (x < 0 || x >= b->width || y < 0 || y >= b->height) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "coordinates out of range");
    }
    uint32_t c = uint32_t(color);
    write_colors(*b, x, y, 1, 1, &c, 1);
    ++b->generation_id;
    return 0;
  });

  r.bind(bm, "nativeGetPixels", "(J[IIIIIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    if (a.h(2) == kNull) rt.throw_npe("pixels");
    rt::ArrayObject& dst_arr = rt.array(a.h(2), "int[]");
    int32_t offset = a.i32(3), stride = a.i32(4), x = a.i32(5), y = a.i32(6);
    int32_t width = a.i32(7), height = a.i32(8);

    std::lock_guard<std::mutex> guard(b->lock);
    // Bitmap.getPixels checked the rectangle, the offset and the stride.
    if (x < 0 || y < 0 || width < 0 || height < 0 || int64_t(x) + width > b->width ||
        int64_t(y) + height > b->height || offset < 0 || (height > 0 && stride < width) ||
        (height > 0 && int64_t(offset) + int64_t(stride) * (height - 1) + width > int64_t(dst_arr.length))) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
    }
    read_colors(*b, x, y, width, height, reinterpret_cast<uint32_t*>(dst_arr.data.data()) + offset, size_t(stride));
    return 0;
  });

  r.bind(bm, "nativeSetPixels", "(J[IIIIIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    if (a.h(2) == kNull) rt.throw_npe("pixels");
    rt::ArrayObject& src_arr = rt.array(a.h(2), "int[]");
    int32_t offset = a.i32(3), stride = a.i32(4), x = a.i32(5), y = a.i32(6);
    int32_t width = a.i32(7), height = a.i32(8);

    std::lock_guard<std::mutex> guard(b->lock);
    if (x < 0 || y < 0 || width < 0 || height < 0 || int64_t(x) + width > b->width ||
        int64_t(y) + height > b->height || offset < 0 || (height > 0 && stride < width) ||
        (height > 0 && int64_t(offset) + int64_t(stride) * (height - 1) + width > int64_t(src_arr.length))) {
      rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
    }
    write_colors(*b, x, y, width, height, reinterpret_cast<const uint32_t*>(src_arr.data.data()) + offset,
                 size_t(stride));
    ++b->generation_id;
    return 0;
  });

  r.bind(bm, "nativeGenerationId", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_int(b->generation_id);
  });

  r.bind(bm, "nativeGetAllocationByteCount", "(J)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_int(b->width * b->height * 4);
  });

  r.bind(bm, "nativeIsImmutable", "(J)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    return rt::ret_bool(!b->is_mutable);
  });

  r.bind(bm, "nativeSetImmutable", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->is_mutable = false;
    return 0;
  });

  r.bind(bm, "nativeIsSRGB", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(true); });
  r.bind(bm, "nativeIsSRGBLinear", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(false); });

  r.bind(bm, "nativeComputeColorSpace", "(J)Landroid/graphics/ColorSpace;",
         [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });

  r.bind(bm, "nativeSetColorSpace", "(JJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->color_space_ptr = a.i64(2);
    return 0;
  });

  r.bind(bm, "nativeRecycle", "(J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->pixels = std::make_shared<PixelBuffer>();  // canvases keep the old ones
    b->recycled = true;
    return 0;
  });

  r.bind(bm, "nativeReconfigure", "(JIIIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(b->lock);
    b->width = a.i32(2);
    b->height = a.i32(3);
    b->config = a.i32(4);
    b->is_premultiplied = a.z(5);
    // The memory is kept when it is large enough, as Android's reconfigure.
    size_t needed = size_t(b->width) * size_t(b->height);
    if (needed > b->pixels->data.size()) b->allocate();
    ++b->generation_id;
    return 0;
  });

  r.bind(bm, "nativeErase", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    SkColor color = SkColor(uint32_t(a.i32(2)));
    std::lock_guard<std::mutex> guard(b->lock);
    b->pixmap().erase(color);  // SkBitmap::eraseColor
    ++b->generation_id;
    return 0;
  });

  r.bind(bm, "nativeErase", "(JJJ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = bitmap(rt, a.i64(0));
    SkColor4f color = color4f_of_long(a.i64(4));
    std::lock_guard<std::mutex> guard(b->lock);
    b->pixmap().erase(color);
    ++b->generation_id;
    return 0;
  });

  r.bind(bm, "nativeCopy", "(JIZ)Landroid/graphics/Bitmap;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto src = bitmap(rt, a.i64(0));
    auto copy = std::make_shared<BitmapState>();
    {
      std::lock_guard<std::mutex> guard(src->lock);
      copy->copy_from(*src);
    }
    copy->config = a.i32(2);
    copy->is_mutable = a.z(3);
    int64_t h = img_tables().bitmaps.add(std::move(copy));
    return create_bitmap_object(rt, h, src->width, src->height, src->density, a.z(3), kNull, kNull);
  });

  r.bind(bm, "nativeIsBackedByAshmem", "(J)Z", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(false); });
  r.bind(bm, "nativeGetAshmemFD", "(J)I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(bm, "nativeGetHardwareBuffer", "(J)Landroid/hardware/HardwareBuffer;",
         [](Runtime&, NativeArgs&) -> uint64_t { return kNull; });
}

// -----------------------------------------------------------------------------
// ImageDecoder Natives
// -----------------------------------------------------------------------------

Handle build_decoder_instance(Runtime& rt, std::shared_ptr<ImageDecoderState> dec) {
  int64_t handle = img_tables().decoders.add(dec);

  Handle padding_rect = kNull;
  if (dec->decoded.has_nptc) {
    padding_rect = create_rect(rt, dec->decoded.patch.padding_left, dec->decoded.patch.padding_top,
                               dec->decoded.patch.padding_right, dec->decoded.patch.padding_bottom);
  }

  Handle mime_type = rt.new_string_utf8("image/png");
  Handle instance = construct(rt, "Landroid/graphics/ImageDecoder;",
                              "(JIIZZLjava/lang/String;Landroid/graphics/ColorSpace;Landroid/graphics/Rect;)V",
                              {lo(handle), hi(handle), dec->decoded.width, dec->decoded.height,
                               0, uint32_t(dec->decoded.has_nptc ? 1 : 0),
                               mime_type, kNull, padding_rect});

  rt::Class* cls = rt.find_class("Landroid/graphics/ImageDecoder;");
  rt::Object* obj = rt.object(instance);
  if (obj) {
    if (rt::Field* f = cls->declared_field("mNativePtr", "J")) {
      obj->fields.at(f->slot) = lo(handle);
      obj->fields.at(f->slot + 1) = hi(handle);
    }
    if (dec->decoded.has_nptc) {
      if (rt::Field* f = cls->declared_field("mNinePatchChunk", "[B")) {
        obj->fields.at(f->slot) = build_byte_array(rt, dec->decoded.nptc_chunk);
      }
    }
    if (dec->decoded.has_optical) {
      if (rt::Field* f = cls->declared_field("mOpticalInsets", "Landroid/graphics/Rect;")) {
        obj->fields.at(f->slot) = create_rect(rt, dec->decoded.optical_left, dec->decoded.optical_top,
                                              dec->decoded.optical_right, dec->decoded.optical_bottom);
      }
    }
    if (dec->decoded.has_outline) {
      if (rt::Field* f = cls->declared_field("mOutlineInsets", "Landroid/graphics/Rect;")) {
        obj->fields.at(f->slot) = create_rect(rt, dec->decoded.outline_left, dec->decoded.outline_top,
                                              dec->decoded.outline_right, dec->decoded.outline_bottom);
      }
      if (rt::Field* f = cls->declared_field("mOutlineRadius", "F")) {
        obj->fields.at(f->slot) = float_bits(dec->decoded.outline_radius);
      }
      if (rt::Field* f = cls->declared_field("mOutlineAlpha", "I")) {
        obj->fields.at(f->slot) = uint32_t(dec->decoded.outline_alpha);
      }
    }
  }
  return instance;
}

void register_image_decoder(rt::NativeRegistry& r) {
  const char* id = "Landroid/graphics/ImageDecoder;";

  r.bind(id, "nClose", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    img_tables().decoders.remove(a.i64(0));
    return 0;
  });

  r.bind(id, "nCreate", "(JZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int64_t asset_handle = a.i64(0);
           const auto* bytes = asset_data(asset_handle);
           if (!bytes) {
             rt.throw_new("Ljava/io/IOException;", "nativeAsset invalide ou introuvable");
           }
           auto dec = std::make_shared<ImageDecoderState>();
           if (!decode_png(bytes->data(), bytes->size(), dec->decoded)) {
             rt.throw_new("Ljava/io/IOException;", "décodage PNG échoué depuis nativeAsset");
           }
           return build_decoder_instance(rt, dec);
         });

  r.bind(id, "nCreate", "([BIIZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("data");
           rt::ArrayObject& arr = rt.array(a.h(0), "byte[]");
           int32_t offset = a.i32(1), length = a.i32(2);
           if (offset < 0 || length < 0 || size_t(offset + length) > arr.data.size()) {
             rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "nCreate byte[]");
           }
           auto dec = std::make_shared<ImageDecoderState>();
           if (!decode_png(arr.data.data() + offset, size_t(length), dec->decoded)) {
             rt.throw_new("Ljava/io/IOException;", "décodage PNG échoué depuis byte[]");
           }
           return build_decoder_instance(rt, dec);
         });

  r.bind(id, "nCreate", "(Ljava/nio/ByteBuffer;IIZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("buffer");
           rt::Object& buf = rt.nonnull(a.h(0), "ByteBuffer");
           int32_t pos = a.i32(1), limit = a.i32(2);
           if (limit <= pos) rt.throw_new("Ljava/io/IOException;", "buffer vide");

           // If direct buffer, address is stored in fields
           rt::Field* addr_f = buf.klass->declared_field("address", "J");
           if (addr_f) {
             uint64_t addr = uint64_t(buf.fields.at(addr_f->slot)) |
                             (uint64_t(buf.fields.at(addr_f->slot + 1)) << 32);
             if (addr != 0) {
               auto span = native_span(rt, int64_t(addr) + pos, size_t(limit - pos));
               auto dec = std::make_shared<ImageDecoderState>();
               if (!decode_png(span.data(), span.size(), dec->decoded)) {
                 rt.throw_new("Ljava/io/IOException;", "décodage PNG direct buffer échoué");
               }
               return build_decoder_instance(rt, dec);
             }
           }
           rt.throw_new("Ljava/io/IOException;", "ByteBuffer non supporté pour ImageDecoder");
         });

  r.bind(id, "nCreate", "(Ljava/io/FileDescriptor;JZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = fd_of(rt, a.h(0));
           int64_t len = a.i64(1);
           if (len <= 0) len = int64_t(fstat_fd(fd).size);
           std::vector<uint8_t> data;
           data.resize(size_t(len));
           size_t read_bytes = read_fd(fd, std::span<uint8_t>(data));
           data.resize(read_bytes);
           auto dec = std::make_shared<ImageDecoderState>();
           if (!decode_png(data.data(), data.size(), dec->decoded)) {
             rt.throw_new("Ljava/io/IOException;", "décodage PNG fd échoué");
           }
           return build_decoder_instance(rt, dec);
         });

  r.bind(id, "nCreate", "(Ljava/io/InputStream;[BZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(0) == kNull) rt.throw_npe("is");
           rt::Object& is_obj = rt.nonnull(a.h(0), "InputStream");

           // Fast path: AssetInputStream has native asset handle in mAsset
           if (rt::Field* f_asset = is_obj.klass->declared_field("mAsset", "J")) {
             int64_t h_asset = int64_t(uint64_t(is_obj.fields.at(f_asset->slot)) |
                                       (uint64_t(is_obj.fields.at(f_asset->slot + 1)) << 32));
             if (h_asset != 0) {
               const auto* bytes = asset_data(h_asset);
               if (bytes) {
                 auto dec = std::make_shared<ImageDecoderState>();
                 if (!decode_png(bytes->data(), bytes->size(), dec->decoded)) {
                   rt.throw_new("Ljava/io/IOException;", "décodage PNG AssetInputStream échoué");
                 }
                 return build_decoder_instance(rt, dec);
               }
             }
           }

           // Fallback: read from InputStream using a temporary buffer
           std::vector<uint8_t> all_bytes;
           Handle temp_arr = rt.new_array("[B", 16384);
           while (true) {
             uint64_t res = rt.call_virtual(a.h(0), "read", "([B)I", {temp_arr});
             int32_t count = int32_t(res);
             if (count <= 0) break;
             auto& arr = rt.array(temp_arr, "byte[]");
             all_bytes.insert(all_bytes.end(), arr.data.data(), arr.data.data() + count);
           }
           auto dec = std::make_shared<ImageDecoderState>();
           if (!decode_png(all_bytes.data(), all_bytes.size(), dec->decoded)) {
             rt.throw_new("Ljava/io/IOException;", "décodage PNG InputStream échoué");
           }
           return build_decoder_instance(rt, dec);
         });

  r.bind(id, "nDecodeBitmap",
         "(JLandroid/graphics/ImageDecoder;IILandroid/graphics/Rect;IIJJ)Landroid/graphics/Bitmap;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto dec = decoder(rt, a.i64(0));
           auto bm_state = std::make_shared<BitmapState>();
           bm_state->width = dec->decoded.width;
           bm_state->height = dec->decoded.height;
           bm_state->config = 5;  // ARGB_8888
           bm_state->allocate();
           write_colors(*bm_state, 0, 0, bm_state->width, bm_state->height, dec->decoded.pixels.data(),
                        size_t(bm_state->width));
           bm_state->is_mutable = false;
           bm_state->nine_patch_chunk = dec->decoded.nptc_chunk;

           int64_t bm_handle = img_tables().bitmaps.add(std::move(bm_state));

           Handle chunk_arr = kNull;
           Handle insets_struct = kNull;
           if (dec->decoded.has_nptc) {
             chunk_arr = build_byte_array(rt, dec->decoded.nptc_chunk);
             if (dec->decoded.has_optical || dec->decoded.has_outline) {
               insets_struct = create_nine_patch_insets(
                   rt, dec->decoded.outline_left, dec->decoded.outline_top,
                   dec->decoded.outline_right, dec->decoded.outline_bottom,
                   dec->decoded.optical_left, dec->decoded.optical_top,
                   dec->decoded.optical_right, dec->decoded.optical_bottom,
                   dec->decoded.outline_radius, dec->decoded.outline_alpha);
             }
           }

           return create_bitmap_object(rt, bm_handle, dec->decoded.width, dec->decoded.height, 0, false,
                                       chunk_arr, insets_struct);
         });

  r.bind(id, "nGetSampledSize", "(JI)Landroid/util/Size;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto dec = decoder(rt, a.i64(0));
    int32_t sample = a.i32(2);
    if (sample <= 0) sample = 1;
    return construct(rt, "Landroid/util/Size;", "(II)V",
                     {uint32_t(dec->decoded.width / sample), uint32_t(dec->decoded.height / sample)});
  });
}

}  // namespace

bool decode_png_pixels(const uint8_t* data, size_t size, DecodedPng& out) {
  PngDecoded decoded;
  if (!decode_png(data, size, decoded)) return false;
  out.width = decoded.width;
  out.height = decoded.height;
  out.pixels = std::move(decoded.pixels);
  return true;
}

NinePatchDivs nine_patch_divs(rt::Runtime& rt, int64_t chunk) {
  auto c = img_tables().nine_patch_chunks.get(chunk);
  if (!c) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] NinePatch invalide");
  return NinePatchDivs{c->patch.x_divs, c->patch.y_divs, c->patch.colors};
}

PixelTarget bitmap_pixels(rt::Runtime& rt, int64_t handle) {
  auto b = bitmap(rt, handle);
  std::lock_guard<std::mutex> guard(b->lock);
  PixelTarget t;
  t.pixels = b->pixels;
  t.width = b->width;
  t.height = b->height;
  // SkImageInfo::isOpaque: RGB_565 (3) always, ARGB_8888 and others without alpha.
  t.opaque = b->config == 3 || (b->config != 1 && !b->has_alpha);
  t.premultiplied = b->is_premultiplied;
  t.owner = b;
  return t;
}

void register_framework_images(rt::NativeRegistry& r) {
  register_nine_patch(r);
  register_bitmap(r);
  register_image_decoder(r);
}

}  // namespace aa::bridge
