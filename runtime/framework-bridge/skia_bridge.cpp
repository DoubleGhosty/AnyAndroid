// The bridge's paths, regions, colours and typefaces as Skia objects
// (skia_bridge.h, ADR-008), with the conversions of HWUI's GraphicsJNI.
#include "framework-bridge/skia_bridge.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/ports/SkFontMgr_empty.h"

namespace aa::bridge {

namespace {

// IEEE 754 binary16 to float (android.util.Half, as HWUI's half_to_float).
float half_to_float(uint16_t h) {
  uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  if (exponent == 0) {
    float v = std::ldexp(float(mantissa), -24);  // zero or subnormal
    return sign ? -v : v;
  }
  if (exponent == 0x1F) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));  // inf, NaN
  return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

}  // namespace

SkColor4f color4f_of_long(int64_t color) {
  uint64_t c = uint64_t(color);
  if ((c & 0x3F) == 0) {  // sRGB: 8-bit channels, as an int colour
    SkColor argb = SkColorSetARGB(uint8_t(c >> 56), uint8_t(c >> 48), uint8_t(c >> 40), uint8_t(c >> 32));
    return SkColor4f::FromColor(argb);
  }
  return SkColor4f{half_to_float(uint16_t(c >> 48)), half_to_float(uint16_t(c >> 32)),
                   half_to_float(uint16_t(c >> 16)), float((c >> 6) & 0x3FF) / 1023.0f};
}

sk_sp<SkColorSpace> skia_color_space(rt::Runtime& rt, int64_t handle) {
  if (handle == 0) return nullptr;
  std::array<float, 7> t;
  std::array<float, 9> m;
  if (!color_space_params(handle, t, m)) {
    rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] ColorSpace invalide");
  }
  skcms_TransferFunction fn;
  fn.a = t[0], fn.b = t[1], fn.c = t[2], fn.d = t[3], fn.e = t[4], fn.f = t[5], fn.g = t[6];
  // GraphicsJNI::getNativeXYZMatrix: Java's matrix is column-major.
  skcms_Matrix3x3 xyz;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) xyz.vals[i][j] = m[size_t(3 * j + i)];
  }
  return SkColorSpace::MakeRGB(fn, xyz);
}

SkPath skia_path_of(rt::Runtime& rt, int64_t path) {
  PathSnapshot s = path_snapshot(rt, path);
  static_assert(sizeof(SkPoint) == 2 * sizeof(float));
  return SkPath::Raw({reinterpret_cast<const SkPoint*>(s.coords.data()), s.coords.size() / 2},
                     {reinterpret_cast<const SkPathVerb*>(s.verbs.data()), s.verbs.size()}, s.weights,
                     SkPathFillType(s.fill_type & 3));
}

sk_sp<SkImage> skia_bitmap_image(rt::Runtime& rt, int64_t bitmap) {
  PixelTarget t = bitmap_pixels(rt, bitmap);
  size_t count = size_t(std::max(t.width, 0)) * size_t(std::max(t.height, 0));
  if (!t.pixels || count == 0 || t.pixels->data.size() < count) return nullptr;
  SkAlphaType at = t.opaque ? kOpaque_SkAlphaType : t.premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
  SkPixmap pixmap(SkImageInfo::Make(t.width, t.height, kN32_SkColorType, at, SkColorSpace::MakeSRGB()),
                  t.pixels->data.data(), size_t(t.width) * 4);
  auto* keep = new std::shared_ptr<PixelBuffer>(t.pixels);
  return SkImages::RasterFromPixmap(
      pixmap, [](const void*, void* ctx) { delete static_cast<std::shared_ptr<PixelBuffer>*>(ctx); }, keep);
}

SkRegion skia_region_of(rt::Runtime& rt, int64_t region) {
  std::vector<std::array<int32_t, 4>> rects = region_rects(rt, region);
  std::vector<SkIRect> r;
  r.reserve(rects.size());
  for (const auto& x : rects) r.push_back(SkIRect::MakeLTRB(x[0], x[1], x[2], x[3]));
  SkRegion out;
  out.setRects(r.data(), int(r.size()));
  return out;
}

sk_sp<SkTypeface> skia_typeface(const std::shared_ptr<text::Face>& face) {
  if (!face) return nullptr;
  // The faces of the engine live as long as their fonts; the cache keeps the
  // most recent ones (and their typefaces) alive.
  struct Entry {
    std::shared_ptr<text::Face> face;
    sk_sp<SkTypeface> typeface;
  };
  static std::mutex lock;
  static auto* entries = new std::list<Entry>;  // most recent first; never destroyed
  static auto* index = new std::map<const text::Face*, std::list<Entry>::iterator>;
  constexpr size_t kMaxEntries = 256;
  std::lock_guard<std::mutex> guard(lock);
  if (auto it = index->find(face.get()); it != index->end()) {
    entries->splice(entries->begin(), *entries, it->second);
    return it->second->typeface;
  }
  static sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Empty();
  const text::FontData& data = face->data();
  auto* owner = new std::shared_ptr<const void>(data.owner);
  sk_sp<SkData> bytes = SkData::MakeWithProc(
      data.bytes, data.size, [](const void*, void* ctx) { delete static_cast<std::shared_ptr<const void>*>(ctx); },
      owner);
  std::vector<SkFontArguments::VariationPosition::Coordinate> coords;
  for (const text::Variation& v : face->variations()) coords.push_back({v.tag, v.value});
  SkFontArguments args;
  args.setCollectionIndex(int(face->index()));
  args.setVariationDesignPosition({coords.data(), int(coords.size())});
  sk_sp<SkTypeface> typeface = manager->makeFromStream(SkMemoryStream::Make(std::move(bytes)), args);
  entries->push_front(Entry{face, typeface});
  (*index)[face.get()] = entries->begin();
  if (entries->size() > kMaxEntries) {
    index->erase(entries->back().face.get());
    entries->pop_back();
  }
  return typeface;
}

}  // namespace aa::bridge
