// Fonts measured as HWUI's Skia measures them. The scaler follows Skia's
// FreeType port (src/ports/SkFontHost_FreeType.cpp, SkScalerContext.cpp,
// SkStrikeSpec.cpp and SkFont.cpp of Android's external/skia), built with
// SK_BUILD_FOR_ANDROID_FRAMEWORK and SK_USE_FREETYPE_EMBOLDEN: the same
// FreeType load flags, the same size and matrix split, the same metrics
// sources and rounding, so that advances, font metrics and glyph bounds are
// Android's to the bit. Skia computes in float: so does this file.
#include "text/font_face.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include FT_COLOR_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_SIZES_H
#include FT_TRUETYPE_TABLES_H

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace aa::text {

namespace {

std::mutex& engine_lock() {
  static auto* m = new std::mutex;  // never destroyed: faces may outlive static destructors
  return *m;
}

// Under the engine lock.
FT_Library library() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    return FT_Init_FreeType(&l) == 0 ? l : nullptr;
  }();
  return lib;
}

// Skia's fixed-point conversions (SkFDot6.h, SkFixed.h): truncating.
FT_F26Dot6 to_fdot6(float x) { return FT_F26Dot6(x * 64); }
FT_Fixed to_fixed(float x) {
  float v = x * 65536.0f;
  v = std::clamp(v, -2147483520.0f, 2147483520.0f);  // sk_float_saturate2int
  return FT_Fixed(int32_t(v));
}
float fdot6_to_float(FT_Pos x) { return float(x) / 64.0f; }
float fixed_to_float(FT_Fixed x) { return float(x) / 65536.0f; }

// The 2x2 part of an SkMatrix: [sx kx; ky sy] maps (x, y) to
// (sx x + kx y, ky x + sy y), y growing downwards.
struct Mat2 {
  float sx = 1, kx = 0, ky = 0, sy = 1;
  // this * diag(a, b) (SkMatrix::preScale).
  void pre_scale(float a, float b) {
    sx *= a;
    ky *= a;
    kx *= b;
    sy *= b;
  }
  Mat2 operator*(const Mat2& o) const {
    return {sx * o.sx + kx * o.ky, sx * o.kx + kx * o.sy, ky * o.sx + sy * o.ky, ky * o.kx + sy * o.sy};
  }
  Rect map(const Rect& r) const {
    float xs[4], ys[4];
    const float px[4] = {r.left, r.right, r.right, r.left};
    const float py[4] = {r.top, r.top, r.bottom, r.bottom};
    for (int i = 0; i < 4; ++i) {
      xs[i] = sx * px[i] + kx * py[i];
      ys[i] = ky * px[i] + sy * py[i];
    }
    return {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
            *std::max_element(ys, ys + 4)};
  }
};

// SkComputeGivensRotation: the rotation G with (G h).y == 0.
Mat2 givens(float a, float b) {
  float c, s;
  if (b == 0) {
    c = std::copysign(1.0f, a);
    s = 0;
  } else if (a == 0) {
    c = 0;
    s = -std::copysign(1.0f, b);
  } else if (std::fabs(b) > std::fabs(a)) {
    float t = a / b;
    float u = std::copysign(std::sqrt(1.0f + t * t), b);
    s = -1.0f / u;
    c = -s * t;
  } else {
    float t = b / a;
    float u = std::copysign(std::sqrt(1.0f + t * t), a);
    c = 1.0f / u;
    s = -c * t;
  }
  return {c, -s, s, c};  // SkMatrix::setSinCos(s, c)
}

constexpr float kNearlyZero = 1.0f / (1 << 12);  // SK_ScalarNearlyZero

// SkScalerContextRec::computeMatrices(kFull): the scale given to FreeType
// and the remaining matrix. False when the matrix is singular (all zero).
bool compute_matrices(const Mat2& a, float* scale_x, float* scale_y, Mat2* remaining) {
  Mat2 ga = a;
  bool skewed_or_flipped = a.kx != 0 || a.ky != 0 || a.sx < 0 || a.sy < 0;
  if (skewed_or_flipped) ga = givens(a.sx, a.ky) * a;  // h = A (1, 0)
  if (std::fabs(ga.sx) <= kNearlyZero || std::fabs(ga.sy) <= kNearlyZero || !std::isfinite(ga.sx) ||
      !std::isfinite(ga.kx) || !std::isfinite(ga.ky) || !std::isfinite(ga.sy)) {
    *scale_x = *scale_y = 1;
    *remaining = {0, 0, 0, 0};
    return false;
  }
  *scale_x = std::fabs(ga.sx);
  *scale_y = std::fabs(ga.sy);
  if (!skewed_or_flipped) {
    *remaining = Mat2{};
  } else {
    *remaining = a;
    remaining->pre_scale(1.0f / *scale_x, 1.0f / *scale_y);
  }
  return true;
}

// The nearest strike at or above the requested size (chooseBitmapStrike).
FT_Int choose_bitmap_strike(FT_Face face, FT_F26Dot6 scale_y) {
  FT_Pos requested = scale_y;
  FT_Int chosen = -1;
  FT_Pos chosen_ppem = 0;
  for (FT_Int i = 0; i < face->num_fixed_sizes; ++i) {
    FT_Pos ppem = face->available_sizes[i].y_ppem;
    if (ppem == requested) return i;
    if (chosen_ppem < requested) {
      if (chosen_ppem < ppem) {
        chosen_ppem = ppem;
        chosen = i;
      }
    } else if (requested < ppem && ppem < chosen_ppem) {
      chosen_ppem = ppem;
      chosen = i;
    }
  }
  return chosen;
}

int face_upem(FT_Face face) {
  int upem = face->units_per_EM;
  if (upem == 0) {
    if (auto* head = static_cast<TT_Header*>(FT_Get_Sfnt_Table(face, FT_SFNT_HEAD))) upem = head->Units_Per_EM;
  }
  return upem;
}

constexpr int kOutlineEmboldenDivisor = 34;  // SK_OUTLINE_EMBOLDEN_DIVISOR (Android)
constexpr FT_Pos kBitmapEmboldenStrength = 1 << 6;

struct GlyphInfo {
  float advance = 0;
  Rect bounds;  // integers (SkGlyph keeps its rounded-out bounds)
};

}  // namespace

// The face's FreeType and HarfBuzz objects, used under the engine lock.
struct Face::Impl {
  FT_Face face = nullptr;
  hb_face_t* hb_face = nullptr;
  std::map<uint32_t, hb_blob_t*> tables;
  std::vector<std::unique_ptr<Strike>> strikes;  // most recently used last
  ~Impl();
};

// A scaler context: an SkStrike for one FontSpec.
struct Face::Strike {
  FontSpec spec;            // as asked (the cache key)
  float source_scale = 1;   // strikeToSourceScale (canonical path size)
  FT_Size size = nullptr;
  FT_Int32 load_flags = 0;
  FT_Matrix matrix22{};
  Mat2 m22;                 // fMatrix22Scalar
  float scale_x = 1, scale_y = 1;  // fScale
  FT_Int strike_index = -1;
  bool embolden = false;
  bool linear = false;
  bool valid = false;
  std::unordered_map<uint16_t, GlyphInfo> glyphs;
  std::optional<FontMetrics> metrics;
  ~Strike() {
    if (size) FT_Done_Size(size);
  }
};

Face::Impl::~Impl() {
  strikes.clear();  // FT_Done_Size before FT_Done_Face
  for (auto& [t, blob] : tables) hb_blob_destroy(blob);
  if (hb_face) hb_face_destroy(hb_face);
  if (face) FT_Done_Face(face);
}

std::shared_ptr<Face> Face::open(FontData data, uint32_t index, std::vector<Variation> axes, std::string* error) {
  auto fail = [&](std::string why) -> std::shared_ptr<Face> {
    if (error) *error = std::move(why);
    return nullptr;
  };
  if (!data.bytes || data.size == 0) return fail("police vide");
  if (data.size > size_t(std::numeric_limits<FT_Long>::max()) || data.size > std::numeric_limits<unsigned>::max()) {
    return fail("police trop grande");
  }
  // Declared before the lock: on failure, the face is destroyed (which
  // takes the lock) after the lock is released.
  std::shared_ptr<Face> f(new Face);
  std::lock_guard<std::mutex> guard(engine_lock());
  FT_Library lib = library();
  if (!lib) return fail("FreeType n'a pas pu être initialisé");

  f->data_ = std::move(data);
  f->index_ = index;
  f->impl_ = std::make_unique<Impl>();
  FT_Face face = nullptr;
  if (FT_Error err = FT_New_Memory_Face(lib, f->data_.bytes, FT_Long(f->data_.size), FT_Long(index), &face)) {
    return fail("police refusée par FreeType (erreur " + std::to_string(err) + ")");
  }
  f->impl_->face = face;
  f->upem_ = uint32_t(std::max(0, face_upem(face)));
  f->scalable_ = FT_IS_SCALABLE(face);

  // Variation coordinates (FaceRec::setupAxes with computeAxisValues).
  if (face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) {
    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(face, &mm) == 0) {
      std::vector<FT_Fixed> coords(mm->num_axis);
      for (FT_UInt i = 0; i < mm->num_axis; ++i) {
        const FT_Var_Axis& a = mm->axis[i];
        Axis axis{uint32_t(a.tag), fixed_to_float(a.minimum), fixed_to_float(a.def), fixed_to_float(a.maximum)};
        f->axes_.push_back(axis);
        coords[i] = to_fixed(axis.def);
        for (size_t j = axes.size(); j-- > 0;) {
          if (axes[j].tag == axis.tag) {
            coords[i] = to_fixed(std::clamp(axes[j].value, axis.min, axis.max));
            break;
          }
        }
      }
      if (!coords.empty()) FT_Set_Var_Design_Coordinates(face, FT_UInt(coords.size()), coords.data());
      FT_Done_MM_Var(lib, mm);
    }
  }
  f->variations_ = std::move(axes);

  // minikin's Font::prepareFont.
  hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(f->data_.bytes), unsigned(f->data_.size),
                                   HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  f->impl_->hb_face = hb_face_create(blob, index);
  hb_blob_destroy(blob);
  hb_font_t* parent = hb_font_create(f->impl_->hb_face);
  hb_ot_font_set_funcs(parent);
  unsigned upem = hb_face_get_upem(f->impl_->hb_face);
  hb_font_set_scale(parent, int(upem), int(upem));
  f->hb_font_ = hb_font_create_sub_font(parent);
  hb_font_destroy(parent);
  std::vector<hb_variation_t> hb_vars;
  for (const Variation& v : f->variations_) hb_vars.push_back({v.tag, v.value});
  hb_font_set_variations(f->hb_font_, hb_vars.data(), unsigned(hb_vars.size()));

  hb_blob_t* cbdt = hb_face_reference_table(f->impl_->hb_face, tag('C', 'B', 'D', 'T'));
  f->color_bitmaps_ = hb_blob_get_length(cbdt) > 0;
  hb_blob_destroy(cbdt);
  return f;
}

Face::~Face() {
  std::lock_guard<std::mutex> guard(engine_lock());
  if (hb_font_) hb_font_destroy(hb_font_);
  impl_.reset();
}

std::shared_ptr<Face> Face::with_variations(const std::vector<Variation>& axes) const {
  std::vector<Variation> merged = variations_;
  merged.insert(merged.end(), axes.begin(), axes.end());
  std::string error;
  return open(data_, index_, std::move(merged), &error);
}

bool Face::os2_style(int* weight, bool* italic) const {
  auto os2 = table(tag('O', 'S', '/', '2'));
  constexpr size_t kWeightClass = 4, kFsSelection = 62;
  if (os2.size() < kFsSelection + 2) return false;
  *weight = os2[kWeightClass] << 8 | os2[kWeightClass + 1];
  *italic = (os2[kFsSelection + 1] & 1) != 0;
  return true;
}

uint32_t Face::glyph(char32_t c) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  return FT_Get_Char_Index(impl_->face, FT_ULong(c));
}

uint32_t Face::glyph(char32_t c, char32_t selector) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  return FT_Face_GetCharVariantIndex(impl_->face, FT_ULong(c), FT_ULong(selector));
}

std::span<const uint8_t> Face::table(uint32_t t) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  auto it = impl_->tables.find(t);
  if (it == impl_->tables.end()) it = impl_->tables.emplace(t, hb_face_reference_table(impl_->hb_face, t)).first;
  unsigned length = 0;
  const char* bytes = hb_blob_get_data(it->second, &length);
  return {reinterpret_cast<const uint8_t*>(bytes), length};
}

// SkStrikeSpec::MakeCanonicalized, MakeRecAndEffects, onFilterRec and the
// SkScalerContext_FreeType constructor. Under the engine lock.
Face::Strike& Face::strike(const FontSpec& spec) const {
  auto& strikes = impl_->strikes;
  for (size_t i = strikes.size(); i-- > 0;) {
    if (strikes[i]->spec == spec) {
      if (i + 1 != strikes.size()) std::rotate(strikes.begin() + long(i), strikes.begin() + long(i) + 1, strikes.end());
      return *strikes.back();
    }
  }
  constexpr size_t kMaxStrikes = 16;
  if (strikes.size() >= kMaxStrikes) strikes.erase(strikes.begin());
  auto s = std::make_unique<Strike>();
  s->spec = spec;
  FT_Face face = impl_->face;

  // Beyond 256 pixels, Skia measures the glyphs as paths at 64 pixels.
  FontSpec f = spec;
  {
    float msx = f.size * f.scale_x, mkx = f.skew_x * f.size, msy = f.size;
    constexpr float kMaxSizeSquared = 256.0f * 256.0f;
    if (msx * msx > kMaxSizeSquared || mkx * mkx + msy * msy > kMaxSizeSquared) {
      f.embedded_bitmaps = false;
      f.force_autohint = false;
      f.subpixel = true;
      f.hinting = Hinting::kNone;
      if (f.edging == Edging::kSubpixelAntiAlias) f.edging = Edging::kAntiAlias;
      s->source_scale = f.size / 64.0f;
      f.size = 64;
    }
  }

  // The scaler context record (no device matrix, no LCD: the default
  // surface has an unknown pixel geometry).
  float text_size = std::min(f.size, 16384.0f);
  bool bw = f.edging == Edging::kAlias;
  Hinting hinting = f.hinting == Hinting::kFull ? Hinting::kNormal : f.hinting;
  if (f.skew_x != 0) hinting = Hinting::kNone;  // not axis aligned
  s->embolden = f.embolden;

  bool linear = f.linear_metrics;
  FT_Int32 flags = FT_LOAD_DEFAULT;
  if (bw) {
    flags = FT_LOAD_TARGET_MONO;
    if (hinting == Hinting::kNone) {
      flags |= FT_LOAD_NO_HINTING;
      linear = true;
    }
  } else {
    switch (hinting) {
      case Hinting::kNone:
        flags = FT_LOAD_NO_HINTING;
        linear = true;
        break;
      case Hinting::kSlight:
        flags = FT_LOAD_TARGET_LIGHT;
        linear = true;
        break;
      case Hinting::kNormal:
      case Hinting::kFull:
        flags = FT_LOAD_TARGET_NORMAL;
        break;
    }
  }
  flags |= f.force_autohint ? FT_LOAD_FORCE_AUTOHINT : FT_LOAD_NO_AUTOHINT;
  if (!f.embedded_bitmaps) flags |= FT_LOAD_NO_BITMAP;
  flags |= FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;
  s->load_flags = flags;

  if (FT_New_Size(face, &s->size) == 0 && FT_Activate_Size(s->size) == 0) {
    // MakeTextMatrix: Scale(size * scaleX, size), then postSkew(skewX, 0).
    Mat2 a{text_size * f.scale_x, f.skew_x * text_size, 0, text_size};
    compute_matrices(a, &s->scale_x, &s->scale_y, &s->m22);
    FT_F26Dot6 sx = to_fdot6(s->scale_x), sy = to_fdot6(s->scale_y);
    if (FT_IS_SCALABLE(face)) {
      if (FT_Set_Char_Size(face, sx, sy, 72, 72) == 0) {
        s->valid = true;
        if (s->scale_x < 1 || s->scale_y < 1) {
          float upem = float(face->units_per_EM);
          const FT_Size_Metrics& m = face->size->metrics;
          float x_ppem = upem * fixed_to_float(m.x_scale) / 64.0f;
          float y_ppem = upem * fixed_to_float(m.y_scale) / 64.0f;
          s->m22.pre_scale(s->scale_x / x_ppem, s->scale_y / y_ppem);
        }
      }
    } else if (FT_HAS_FIXED_SIZES(face)) {
      s->strike_index = choose_bitmap_strike(face, sy);
      if (s->strike_index != -1 && FT_Select_Size(face, s->strike_index) == 0) {
        s->valid = true;
        s->m22.pre_scale(s->scale_x / float(face->size->metrics.x_ppem),
                         s->scale_y / float(face->size->metrics.y_ppem));
        linear = false;  // FreeType has no linear metrics for bitmap fonts
        s->load_flags &= ~FT_LOAD_NO_BITMAP;
        s->load_flags |= FT_LOAD_COLOR;
      } else {
        s->strike_index = -1;
      }
    }
  }
  s->matrix22.xx = to_fixed(s->m22.sx);
  s->matrix22.xy = to_fixed(-s->m22.kx);
  s->matrix22.yx = to_fixed(-s->m22.ky);
  s->matrix22.yy = to_fixed(s->m22.sy);
  s->linear = linear;
  strikes.push_back(std::move(s));
  return *strikes.back();
}

namespace {

// SkScalerContext_FreeType::setupSize.
bool setup_size(FT_Face face, FT_Size size, FT_Matrix* matrix) {
  if (!size || FT_Activate_Size(size) != 0) return false;
  FT_Set_Transform(face, matrix, nullptr);
  return true;
}

// emboldenIfNeeded.
void embolden_if_needed(FT_Face face, FT_Int32 load_flags, bool embolden, FT_UInt gid) {
  if (!embolden) return;
  FT_GlyphSlot glyph = face->glyph;
  if (glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
    FT_Pos strength = FT_MulFix(face->units_per_EM, face->size->metrics.y_scale) / kOutlineEmboldenDivisor;
    FT_Outline_Embolden(&glyph->outline, strength);
  } else if (glyph->format == FT_GLYPH_FORMAT_BITMAP) {
    if (!glyph->bitmap.buffer) FT_Load_Glyph(face, gid, load_flags);
    FT_GlyphSlot_Own_Bitmap(glyph);
    FT_Bitmap_Embolden(glyph->library, &glyph->bitmap, kBitmapEmboldenStrength, 0);
  }
}

bool outline_bounds(FT_GlyphSlot glyph, Rect* out) {
  if (glyph->format != FT_GLYPH_FORMAT_OUTLINE || glyph->outline.n_contours == 0) return false;
  FT_BBox box;
  FT_Outline_Get_CBox(&glyph->outline, &box);
  *out = {fdot6_to_float(box.xMin), -fdot6_to_float(box.yMax), fdot6_to_float(box.xMax), -fdot6_to_float(box.yMin)};
  return true;
}

Rect join_rects(const Rect& a, const Rect& b) {
  if (b.empty()) return a;
  if (a.empty()) return b;
  return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

// SkScalerContext::SaturateGlyphBounds: rounded out; empty when either
// dimension is.
Rect round_out(const Rect& r) {
  Rect o{std::floor(r.left), std::floor(r.top), std::ceil(r.right), std::ceil(r.bottom)};
  if (!(o.right - o.left > 0) || !(o.bottom - o.top > 0)) return Rect{};
  return o;
}

}  // namespace

// SkScalerContext::internalMakeGlyph over SkScalerContext_FreeType::
// generateMetrics, for a fill paint (no path effect, no frame): the advance
// and the rounded-out bounds of a glyph. Under the engine lock.
static GlyphInfo make_glyph(FT_Face face, FT_Size size, FT_Matrix* matrix, const Mat2& m22, FT_Int32 load_flags,
                            bool embolden, bool linear, FT_UInt gid) {
  GlyphInfo g;
  if (!setup_size(face, size, matrix)) return g;
  Rect bounds;
  bool layers = false;
  if (FT_IS_SCALABLE(face)) {
    // COLRv1: the clip box when the font gives one.
    FT_OpaquePaint paint{nullptr, 1};
    if (FT_Get_Color_Glyph_Paint(face, gid, FT_COLOR_INCLUDE_ROOT_TRANSFORM, &paint)) {
      layers = true;
      FT_ClipBox clip;
      if (FT_Get_Color_Glyph_ClipBox(face, gid, &clip)) {
        FT_BBox box{clip.bottom_left.x, clip.bottom_left.y, clip.bottom_left.x, clip.bottom_left.y};
        for (const FT_Vector& c : {clip.top_left, clip.top_right, clip.bottom_right}) {
          box.xMin = std::min(box.xMin, c.x);
          box.yMin = std::min(box.yMin, c.y);
          box.xMax = std::max(box.xMax, c.x);
          box.yMax = std::max(box.yMax, c.y);
        }
        bounds = {fdot6_to_float(box.xMin), -fdot6_to_float(box.yMax), fdot6_to_float(box.xMax),
                  -fdot6_to_float(box.yMin)};
      } else {
        // Skia walks the paint graph; the base glyph's outline stands in.
        layers = false;
      }
    }
    if (!layers) {
      // COLRv0: the union of the layers.
      FT_LayerIterator it{0, 0, nullptr};
      FT_UInt layer_glyph, layer_color;
      FT_Int32 layer_flags = (load_flags | FT_LOAD_BITMAP_METRICS_ONLY | FT_LOAD_NO_BITMAP) & ~(FT_LOAD_RENDER | FT_LOAD_COLOR);
      while (FT_Get_Color_Glyph_Layer(face, gid, &layer_glyph, &layer_color, &it)) {
        layers = true;
        if (FT_Load_Glyph(face, layer_glyph, layer_flags)) return g;
        Rect r;
        if (outline_bounds(face->glyph, &r)) bounds = join_rects(bounds, r);
      }
    }
  }
  if (FT_Load_Glyph(face, gid, load_flags | FT_LOAD_BITMAP_METRICS_ONLY)) return g;
  if (!layers) {
    embolden_if_needed(face, load_flags, embolden, gid);
    FT_GlyphSlot slot = face->glyph;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
      outline_bounds(slot, &bounds);
    } else if (slot->format == FT_GLYPH_FORMAT_BITMAP) {
      Rect r{float(slot->bitmap_left), -float(slot->bitmap_top), float(slot->bitmap_left) + float(slot->bitmap.width),
             -float(slot->bitmap_top) + float(slot->bitmap.rows)};
      bounds = m22.map(r);
    } else {
      return g;
    }
  }
  if (linear) {
    g.advance = m22.sx * fixed_to_float(face->glyph->linearHoriAdvance);
  } else {
    g.advance = fdot6_to_float(face->glyph->advance.x);
  }
  g.bounds = round_out(bounds);
  return g;
}

// getCBoxForLetter. Under the engine lock, with the strike's size active.
static bool letter_box(FT_Face face, char letter, FT_Int32 load_flags, bool embolden, FT_BBox* box) {
  FT_UInt gid = FT_Get_Char_Index(face, FT_ULong(letter));
  if (!gid || FT_Load_Glyph(face, gid, load_flags) || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;
  embolden_if_needed(face, load_flags, embolden, gid);
  FT_Outline_Get_CBox(&face->glyph->outline, box);
  return true;
}

// SkScalerContext_FreeType::generateFontMetrics. Under the engine lock.
static FontMetrics make_font_metrics(FT_Face face, FT_Size size, FT_Matrix* matrix, float scale_y,
                                     FT_Int strike_index, FT_Int32 load_flags, bool embolden) {
  FontMetrics m;
  if (!setup_size(face, size, matrix)) return m;
  float upem = float(face_upem(face));
  float x_height = 0, avg_char_width = 0, cap_height = 0, strikeout_thickness = 0, strikeout_position = 0;
  auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
  if (os2) {
    x_height = float(os2->sxHeight) / upem * scale_y;
    avg_char_width = float(os2->xAvgCharWidth) / upem;
    strikeout_thickness = float(os2->yStrikeoutSize) / upem;
    strikeout_position = -float(os2->yStrikeoutPosition) / upem;
    m.flags |= FontMetrics::kStrikeoutThicknessValid | FontMetrics::kStrikeoutPositionValid;
    if (os2->version != 0xFFFF && os2->version >= 2) cap_height = float(os2->sCapHeight) / upem * scale_y;
  }
  float ascent, descent, leading, xmin, xmax, ymin, ymax, underline_thickness, underline_position;
  if (face->face_flags & FT_FACE_FLAG_SCALABLE) {
    constexpr int kUseTypoMetrics = 1 << 7;
    if (os2 && os2->version != 0xFFFF && (os2->fsSelection & kUseTypoMetrics)) {
      ascent = -float(os2->sTypoAscender) / upem;
      descent = -float(os2->sTypoDescender) / upem;
      leading = float(os2->sTypoLineGap) / upem;
    } else {
      ascent = -float(face->ascender) / upem;
      descent = -float(face->descender) / upem;
      leading = float(face->height + (face->descender - face->ascender)) / upem;
    }
    xmin = float(face->bbox.xMin) / upem;
    xmax = float(face->bbox.xMax) / upem;
    ymin = -float(face->bbox.yMin) / upem;
    ymax = -float(face->bbox.yMax) / upem;
    underline_thickness = float(face->underline_thickness) / upem;
    underline_position = -float(face->underline_position + face->underline_thickness / 2) / upem;
    m.flags |= FontMetrics::kUnderlineThicknessValid | FontMetrics::kUnderlinePositionValid;
    FT_BBox box;
    if (!x_height && letter_box(face, 'x', load_flags, embolden, &box)) x_height = float(box.yMax) / 64.0f;
    if (!cap_height && letter_box(face, 'H', load_flags, embolden, &box)) cap_height = float(box.yMax) / 64.0f;
  } else if (strike_index != -1) {
    float xppem = float(face->size->metrics.x_ppem);
    float yppem = float(face->size->metrics.y_ppem);
    ascent = -float(face->size->metrics.ascender) / (yppem * 64.0f);
    descent = -float(face->size->metrics.descender) / (yppem * 64.0f);
    leading = (float(face->size->metrics.height) / (yppem * 64.0f)) + ascent - descent;
    xmin = 0.0f;
    xmax = float(face->available_sizes[strike_index].width) / xppem;
    ymin = descent;
    ymax = ascent;
    m.flags |= FontMetrics::kBoundsInvalid;
    underline_thickness = 0;
    underline_position = 0;
    m.flags &= ~(FontMetrics::kUnderlineThicknessValid | FontMetrics::kUnderlinePositionValid);
    if (auto* post = static_cast<TT_Postscript*>(FT_Get_Sfnt_Table(face, FT_SFNT_POST))) {
      underline_thickness = float(post->underlineThickness) / upem;
      underline_position = -float(post->underlinePosition) / upem;
      m.flags |= FontMetrics::kUnderlineThicknessValid | FontMetrics::kUnderlinePositionValid;
    }
  } else {
    return FontMetrics{};
  }
  if (!x_height) x_height = -ascent * scale_y;
  if (!avg_char_width) avg_char_width = xmax - xmin;
  if (!cap_height) cap_height = -ascent * scale_y;
  if (leading < 0.0f) leading = 0.0f;
  m.top = ymax * scale_y;
  m.ascent = ascent * scale_y;
  m.descent = descent * scale_y;
  m.bottom = ymin * scale_y;
  m.leading = leading * scale_y;
  m.avg_char_width = avg_char_width * scale_y;
  m.x_min = xmin * scale_y;
  m.x_max = xmax * scale_y;
  m.max_char_width = m.x_max - m.x_min;
  m.x_height = x_height;
  m.cap_height = cap_height;
  m.underline_thickness = underline_thickness * scale_y;
  m.underline_position = underline_position * scale_y;
  m.strikeout_thickness = strikeout_thickness * scale_y;
  m.strikeout_position = strikeout_position * scale_y;
  if (face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) m.flags |= FontMetrics::kBoundsInvalid;
  return m;
}

float Face::metrics(const FontSpec& spec, FontMetrics* out) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  Strike& s = strike(spec);
  if (!s.metrics) {
    s.metrics = s.valid ? make_font_metrics(impl_->face, s.size, &s.matrix22, s.scale_y, s.strike_index,
                                            s.load_flags, s.embolden)
                        : FontMetrics{};
  }
  FontMetrics m = *s.metrics;
  if (s.source_scale != 1) {  // SkFontPriv::ScaleFontMetrics
    float k = s.source_scale;
    for (float* v : {&m.top, &m.ascent, &m.descent, &m.bottom, &m.leading, &m.avg_char_width, &m.max_char_width,
                     &m.x_min, &m.x_max, &m.x_height, &m.cap_height, &m.underline_thickness,
                     &m.underline_position, &m.strikeout_thickness, &m.strikeout_position}) {
      *v *= k;
    }
  }
  if (out) *out = m;
  return m.descent - m.ascent + m.leading;
}

void Face::advances(const FontSpec& spec, std::span<const uint16_t> glyphs, std::span<float> out) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  Strike& s = strike(spec);
  for (size_t i = 0; i < glyphs.size() && i < out.size(); ++i) {
    auto it = s.glyphs.find(glyphs[i]);
    if (it == s.glyphs.end()) {
      GlyphInfo g = s.valid ? make_glyph(impl_->face, s.size, &s.matrix22, s.m22, s.load_flags, s.embolden, s.linear,
                                         glyphs[i])
                            : GlyphInfo{};
      it = s.glyphs.emplace(glyphs[i], g).first;
    }
    out[i] = it->second.advance * s.source_scale;
  }
}

void Face::bounds(const FontSpec& spec, std::span<const uint16_t> glyphs, std::span<Rect> out) const {
  std::lock_guard<std::mutex> guard(engine_lock());
  Strike& s = strike(spec);
  for (size_t i = 0; i < glyphs.size() && i < out.size(); ++i) {
    auto it = s.glyphs.find(glyphs[i]);
    if (it == s.glyphs.end()) {
      GlyphInfo g = s.valid ? make_glyph(impl_->face, s.size, &s.matrix22, s.m22, s.load_flags, s.embolden, s.linear,
                                         glyphs[i])
                            : GlyphInfo{};
      it = s.glyphs.emplace(glyphs[i], g).first;
    }
    const Rect& r = it->second.bounds;
    float k = s.source_scale;
    out[i] = r.empty() ? Rect{} : Rect{r.left * k, r.top * k, r.right * k, r.bottom * k};
  }
}

}  // namespace aa::text
