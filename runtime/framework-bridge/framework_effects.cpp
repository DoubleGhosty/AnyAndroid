// Paint effects (G3, J7 of ADR-006, ADR-008), each signature checked
// against the framework.jar of Android 17: Shader (BitmapShader, gradients,
// ComposeShader), ColorFilter (blend mode, lighting, colour matrix),
// MaskFilter (blur, emboss, table) and PathEffect, as HWUI's jni/Shader.cpp,
// ColorFilter.cpp, MaskFilter.cpp and PathEffect.cpp make them with Skia.
// Paints take a reference when the effect is set (Paint.nSetShader...).
// Not yet: gainmaps.
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/skia_bridge.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkBlurMaskFilter.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkDiscretePathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkTableMaskFilter.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

// uirenderer::ColorFilter: what the Java filter describes, and the Skia
// instance made from it (in the sRGB working space), remade after a change.
struct ColorFilterState {
  enum class Kind { kBlendMode, kLighting, kMatrix, kRuntime } kind = Kind::kBlendMode;
  SkColor color = 0, mul = 0, add = 0;
  SkBlendMode mode = SkBlendMode::kSrcOver;
  std::vector<float> matrix;
  std::shared_ptr<SkRuntimeColorFilterBuilder> runtime_builder;
  std::mutex lock;
  sk_sp<SkColorFilter> instance;

  sk_sp<SkColorFilter> get() {
    std::lock_guard<std::mutex> guard(lock);
    if (!instance) {
      switch (kind) {
        case Kind::kBlendMode:
          instance = SkColorFilters::Blend(color, mode);
          break;
        case Kind::kLighting:
          instance = SkColorFilters::Lighting(mul, add);
          break;
        case Kind::kMatrix:
          instance = SkColorFilters::Matrix(matrix.data(), SkColorFilters::Clamp::kNo);
          break;
        case Kind::kRuntime:
          if (runtime_builder) instance = runtime_builder->makeColorFilter();
          break;
      }
      if (instance) instance = instance->makeWithWorkingColorSpace(SkColorSpace::MakeSRGB());
    }
    return instance;
  }
};

template <typename T>
struct Ref {
  sk_sp<T> value;
};

// Kind tags: unique across the runtime (tests/check_handle_kinds.cmake).
struct Tables {
  HandleTable<Ref<SkShader>> shaders{0x1122};
  HandleTable<ColorFilterState> color_filters{0x1123};
  HandleTable<Ref<SkMaskFilter>> mask_filters{0x1124};
  HandleTable<Ref<SkPathEffect>> path_effects{0x1125};
  HandleTable<SkRuntimeShaderBuilder> shader_builders{0x1128};
};

Tables& tables() {
  static auto* t = new Tables;  // never destroyed: handles may be freed at exit
  return *t;
}

template <typename T>
int64_t add(HandleTable<Ref<T>>& table, sk_sp<T> value) {
  if (!value) return 0;
  return table.add(std::make_shared<Ref<T>>(Ref<T>{std::move(value)}));
}

template <typename T>
sk_sp<T> lookup(Runtime& rt, HandleTable<Ref<T>>& table, int64_t handle, const char* what) {
  if (handle == 0) return nullptr;
  auto r = table.get(handle);
  if (!r) rt.throw_new("Ljava/lang/IllegalStateException;", std::string("[AnyAndroid] ") + what + " invalide");
  return r->value;
}

std::shared_ptr<ColorFilterState> color_filter(Runtime& rt, int64_t handle) {
  auto f = tables().color_filters.get(handle);
  if (!f) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] ColorFilter invalide");
  return f;
}

[[noreturn]] void throw_iae(Runtime& rt, const char* message = "") {
  rt.throw_new("Ljava/lang/IllegalArgumentException;", message);
}

// The local matrix of a shader (a Matrix handle; 0: none).
std::optional<SkMatrix> local_matrix(Runtime& rt, int64_t handle) {
  if (handle == 0) return std::nullopt;
  skmatrix::Values v = skmatrix::of(rt, handle);
  SkMatrix m;
  m.set9(v.data());
  return m;
}

sk_sp<SkShader> with_matrix(sk_sp<SkShader> shader, const std::optional<SkMatrix>& m) {
  return m ? shader->makeWithLocalMatrix(*m) : shader;
}

SkTileMode tile_mode(int32_t mode) { return SkTileMode(std::clamp(mode, 0, int(SkTileMode::kLastTileMode))); }

std::vector<float> floats_of(Runtime& rt, Handle array, size_t min_length) {
  if (array == kNull) rt.throw_npe("float[]");
  rt::ArrayObject& a = rt.array(array, "float[]");
  if (a.width != 4) throw_iae(rt);
  if (a.length < min_length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
  std::vector<float> out(a.length);
  std::memcpy(out.data(), a.data.data(), size_t(a.length) * 4);
  return out;
}

// AutoSkGradient: Color longs, optional positions (at least one per colour)
// and premultiplied interpolation, as gradients before Skia's newer default.
struct Gradient {
  std::vector<SkColor4f> colors;
  std::vector<float> positions;
  sk_sp<SkColorSpace> color_space;
  SkTileMode mode = SkTileMode::kClamp;

  SkGradient get() const {
    static constexpr SkGradient::Interpolation kInterpolation = {
        SkGradient::Interpolation::InPremul::kYes, SkGradient::Interpolation::ColorSpace::kDestination,
        SkGradient::Interpolation::HueMethod::kShorter};
    SkSpan<const float> pos = positions.empty() ? SkSpan<const float>() : SkSpan<const float>(positions.data(), colors.size());
    return SkGradient(SkGradient::Colors(colors, pos, mode, color_space), kInterpolation);
  }
};

Gradient gradient(Runtime& rt, Handle colors, Handle positions, int64_t color_space, int32_t mode) {
  Gradient g;
  if (colors == kNull) rt.throw_npe("colors");
  rt::ArrayObject& c = rt.array(colors, "long[]");
  if (c.width != 8) throw_iae(rt);
  for (uint32_t i = 0; i < c.length; i++) {
    int64_t v;
    std::memcpy(&v, c.data.data() + size_t(i) * 8, 8);
    g.colors.push_back(color4f_of_long(v));
  }
  if (positions != kNull) g.positions = floats_of(rt, positions, g.colors.size());
  g.color_space = skia_color_space(rt, color_space);
  g.mode = tile_mode(mode);
  return g;
}

// MaskFilter.cpp's convertRadiusToSigma.
float radius_to_sigma(float radius) { return radius > 0 ? 0.57735f * radius + 0.5f : 0.0f; }

// ColorMatrixColorFilter: Java biases the translations by 255.
std::vector<float> color_matrix(Runtime& rt, Handle array) {
  std::vector<float> m = floats_of(rt, array, 20);
  m.resize(20);
  for (size_t i : {4, 9, 14, 19}) m[i] *= 1.0f / 255;
  return m;
}

bool is_int_uniform_type(SkRuntimeEffect::Uniform::Type type) {
  switch (type) {
    case SkRuntimeEffect::Uniform::Type::kInt:
    case SkRuntimeEffect::Uniform::Type::kInt2:
    case SkRuntimeEffect::Uniform::Type::kInt3:
    case SkRuntimeEffect::Uniform::Type::kInt4:
      return true;
    default:
      return false;
  }
}

void update_float_uniforms(Runtime& rt, SkRuntimeEffectBuilder& builder, const char* name,
                           const float values[], int count, bool is_color) {
  auto uniform = builder.uniform(name);
  if (!uniform.fVar) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("uniform introuvable : ") + name);
    return;
  }
  if (is_color != ((uniform.fVar->flags & SkRuntimeEffect::Uniform::kColor_Flag) != 0)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("type couleur incorrect pour uniform : ") + name);
    return;
  }
  if (is_int_uniform_type(uniform.fVar->type)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("tentative d'assigner un float à un int uniform : ") + name);
    return;
  }
  if (!uniform.set<float>(values, count)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("taille incorrecte pour uniform : ") + name);
  }
}

void update_int_uniforms(Runtime& rt, SkRuntimeEffectBuilder& builder, const char* name,
                         const int32_t values[], int count) {
  auto uniform = builder.uniform(name);
  if (!uniform.fVar) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("uniform introuvable : ") + name);
    return;
  }
  if (!is_int_uniform_type(uniform.fVar->type)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("tentative d'assigner un int à un float uniform : ") + name);
    return;
  }
  if (!uniform.set<int32_t>(values, count)) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", std::string("taille incorrecte pour uniform : ") + name);
  }
}

void register_shaders(rt::NativeRegistry& r) {
  static const int64_t shader_free = register_free_function([](int64_t h) { tables().shaders.remove(h); });
  r.bind("Landroid/graphics/Shader;", "nativeGetFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(shader_free); });
  r.bind("Landroid/graphics/BitmapShader;", "nativeCreate", "(JJIIIZZJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(0));
    int32_t aniso = a.i32(6);
    SkSamplingOptions sampling =
        aniso > 0 ? SkSamplingOptions::Aniso(aniso)
                  : SkSamplingOptions(a.z(7) ? SkFilterMode::kLinear : SkFilterMode::kNearest, SkMipmapMode::kNone);
    sk_sp<SkImage> image = a.i64(2) != 0 ? skia_bitmap_image(rt, a.i64(2)) : nullptr;
    if (!image) return rt::ret_long(add(tables().shaders, SkShaders::Empty()));
    sk_sp<SkShader> shader = a.z(8) ? image->makeRawShader(tile_mode(a.i32(4)), tile_mode(a.i32(5)), sampling)
                                    : image->makeShader(tile_mode(a.i32(4)), tile_mode(a.i32(5)), sampling);
    if (!shader) throw_iae(rt);
    return rt::ret_long(add(tables().shaders, with_matrix(std::move(shader), m)));
  }, rt::ApiStatus::kPartial, "sans gainmap");
  r.bind("Landroid/graphics/LinearGradient;", "nativeCreate", "(JFFFF[J[FIJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(0));
    SkPoint pts[2] = {{a.f32(2), a.f32(3)}, {a.f32(4), a.f32(5)}};
    Gradient g = gradient(rt, a.h(6), a.h(7), a.i64(9), a.i32(8));
    sk_sp<SkShader> shader = SkShaders::LinearGradient(pts, g.get());
    if (!shader) throw_iae(rt);
    return rt::ret_long(add(tables().shaders, with_matrix(std::move(shader), m)));
  });
  r.bind("Landroid/graphics/RadialGradient;", "nativeCreate", "(JFFFFFF[J[FIJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(0));
    Gradient g = gradient(rt, a.h(8), a.h(9), a.i64(11), a.i32(10));
    sk_sp<SkShader> shader =
        SkShaders::TwoPointConicalGradient({a.f32(2), a.f32(3)}, a.f32(4), {a.f32(5), a.f32(6)}, a.f32(7), g.get());
    if (!shader) throw_iae(rt);
    return rt::ret_long(add(tables().shaders, with_matrix(std::move(shader), m)));
  });
  r.bind("Landroid/graphics/SweepGradient;", "nativeCreate", "(JFF[J[FJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(0));
    Gradient g = gradient(rt, a.h(4), a.h(5), a.i64(6), int32_t(SkTileMode::kClamp));
    sk_sp<SkShader> shader = SkShaders::SweepGradient({a.f32(2), a.f32(3)}, g.get());
    if (!shader) throw_iae(rt);
    return rt::ret_long(add(tables().shaders, with_matrix(std::move(shader), m)));
  });
  r.bind("Landroid/graphics/ComposeShader;", "nativeCreate", "(JJJI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(0));
    sk_sp<SkShader> dst = lookup(rt, tables().shaders, a.i64(2), "Shader");
    sk_sp<SkShader> src = lookup(rt, tables().shaders, a.i64(4), "Shader");
    SkBlendMode mode = SkBlendMode(std::clamp(a.i32(6), 0, int(SkBlendMode::kLastMode)));
    return rt::ret_long(add(tables().shaders, with_matrix(SkShaders::Blend(mode, dst, src), m)));
  });

  // RuntimeShader (AGSL)
  const char* rts = "Landroid/graphics/RuntimeShader;";
  static const int64_t rts_builder_free =
      register_free_function([](int64_t h) { tables().shader_builders.remove(h); });
  r.bind(rts, "nativeGetFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(rts_builder_free); });
  r.bind(rts, "nativeCreateBuilder", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("sksl");
    std::string sksl = rt.string_utf8(a.h(0));
    auto result = SkRuntimeEffect::MakeForShader(SkString(sksl.c_str()), SkRuntimeEffect::Options{});
    if (!result.effect) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", result.errorText.c_str());
    }
    auto builder = std::make_shared<SkRuntimeShaderBuilder>(std::move(result.effect));
    return uint64_t(tables().shader_builders.add(std::move(builder)));
  });
  r.bind(rts, "nativeCreateShader", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    std::optional<SkMatrix> m = local_matrix(rt, a.i64(2));
    sk_sp<SkShader> shader = b->makeShader(m ? &*m : nullptr);
    if (!shader) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", "impossible de construire le shader d'exécution");
    }
    return rt::ret_long(add(tables().shaders, std::move(shader)));
  });
  r.bind(rts, "nativeUpdateUniforms", "(JLjava/lang/String;FFFFI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    float values[4] = {a.f32(3), a.f32(4), a.f32(5), a.f32(6)};
    update_float_uniforms(rt, *b, name.c_str(), values, a.i32(7), false);
    return 0;
  });
  r.bind(rts, "nativeUpdateUniforms", "(JLjava/lang/String;[FZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    if (a.h(3) == kNull) rt.throw_npe("values");
    std::string name = rt.string_utf8(a.h(2));
    rt::ArrayObject& arr = rt.array(a.h(3), "float[]");
    const float* ptr = reinterpret_cast<const float*>(arr.data.data());
    update_float_uniforms(rt, *b, name.c_str(), ptr, arr.length, a.z(4));
    return 0;
  });
  r.bind(rts, "nativeUpdateUniforms", "(JLjava/lang/String;IIIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    int32_t values[4] = {a.i32(3), a.i32(4), a.i32(5), a.i32(6)};
    update_int_uniforms(rt, *b, name.c_str(), values, a.i32(7));
    return 0;
  });
  r.bind(rts, "nativeUpdateUniforms", "(JLjava/lang/String;[I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    if (a.h(3) == kNull) rt.throw_npe("values");
    std::string name = rt.string_utf8(a.h(2));
    rt::ArrayObject& arr = rt.array(a.h(3), "int[]");
    const int32_t* ptr = reinterpret_cast<const int32_t*>(arr.data.data());
    update_int_uniforms(rt, *b, name.c_str(), ptr, arr.length);
    return 0;
  });
  r.bind(rts, "nativeUpdateShader", "(JLjava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    sk_sp<SkShader> child = lookup(rt, tables().shaders, a.i64(3), "Shader");
    auto c = b->child(name.c_str());
    if (!c.fChild) rt.throw_new("Ljava/lang/IllegalArgumentException;", "child introuvable : " + name);
    c = std::move(child);
    return 0;
  });
  r.bind(rts, "nativeUpdateColorFilter", "(JLjava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    sk_sp<SkColorFilter> child = skia_color_filter(rt, a.i64(3));
    auto c = b->child(name.c_str());
    if (!c.fChild) rt.throw_new("Ljava/lang/IllegalArgumentException;", "child introuvable : " + name);
    c = std::move(child);
    return 0;
  });
  r.bind(rts, "nativeUpdateChild", "(JLjava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto b = tables().shader_builders.get(a.i64(0));
    if (!b) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeShader builder invalide");
    if (a.h(2) == kNull) rt.throw_npe("childName");
    std::string name = rt.string_utf8(a.h(2));
    auto c = b->child(name.c_str());
    if (!c.fChild) rt.throw_new("Ljava/lang/IllegalArgumentException;", "child introuvable : " + name);
    sk_sp<SkShader> s = tables().shaders.get(a.i64(3)) ? lookup(rt, tables().shaders, a.i64(3), "Shader") : nullptr;
    if (s) {
      c = std::move(s);
    } else {
      sk_sp<SkColorFilter> cf = skia_color_filter(rt, a.i64(3));
      c = std::move(cf);
    }
    return 0;
  });
}

void register_color_filters(rt::NativeRegistry& r) {
  static const int64_t filter_free = register_free_function([](int64_t h) { tables().color_filters.remove(h); });
  r.bind("Landroid/graphics/ColorFilter;", "nativeGetFinalizer", "()J",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_long(filter_free); });
  auto blend_mode = [](Runtime&, NativeArgs& a) -> uint64_t {
    auto f = std::make_shared<ColorFilterState>();
    f->kind = ColorFilterState::Kind::kBlendMode;
    f->color = SkColor(uint32_t(a.i32(0)));
    f->mode = SkBlendMode(std::clamp(a.i32(1), 0, int(SkBlendMode::kLastMode)));
    return rt::ret_long(tables().color_filters.add(std::move(f)));
  };
  r.bind("Landroid/graphics/PorterDuffColorFilter;", "native_CreateBlendModeFilter", "(II)J", blend_mode);
  r.bind("Landroid/graphics/BlendModeColorFilter;", "native_CreateBlendModeFilter", "(II)J", blend_mode);
  const char* lighting = "Landroid/graphics/LightingColorFilter;";
  r.bind(lighting, "native_CreateLightingFilter", "(II)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto f = std::make_shared<ColorFilterState>();
    f->kind = ColorFilterState::Kind::kLighting;
    f->mul = SkColor(uint32_t(a.i32(0)));
    f->add = SkColor(uint32_t(a.i32(1)));
    return rt::ret_long(tables().color_filters.add(std::move(f)));
  });
  r.bind(lighting, "native_SetLightingFilterAdd", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(f->lock);
    f->add = SkColor(uint32_t(a.i32(2)));
    f->instance = nullptr;
    return 0;
  });
  r.bind(lighting, "native_SetLightingFilterMul", "(JI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(f->lock);
    f->mul = SkColor(uint32_t(a.i32(2)));
    f->instance = nullptr;
    return 0;
  });
  const char* matrix = "Landroid/graphics/ColorMatrixColorFilter;";
  r.bind(matrix, "nativeColorMatrixFilter", "([F)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = std::make_shared<ColorFilterState>();
    f->kind = ColorFilterState::Kind::kMatrix;
    f->matrix = color_matrix(rt, a.h(0));
    return rt::ret_long(tables().color_filters.add(std::move(f)));
  });
  r.bind(matrix, "nativeSetColorMatrix", "(J[F)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<float> m = color_matrix(rt, a.h(2));
    auto f = color_filter(rt, a.i64(0));
    std::lock_guard<std::mutex> guard(f->lock);
    f->matrix = std::move(m);
    f->instance = nullptr;
    return 0;
  });

  // RuntimeColorFilter (AGSL)
  const char* rcf = "Landroid/graphics/RuntimeColorFilter;";
  r.bind(rcf, "nativeCreateRuntimeColorFilter", "(Ljava/lang/String;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("agsl");
    std::string agsl = rt.string_utf8(a.h(0));
    auto result = SkRuntimeEffect::MakeForColorFilter(SkString(agsl.c_str()), SkRuntimeEffect::Options{});
    if (!result.effect) {
      rt.throw_new("Ljava/lang/IllegalArgumentException;", result.errorText.c_str());
    }
    auto f = std::make_shared<ColorFilterState>();
    f->kind = ColorFilterState::Kind::kRuntime;
    f->runtime_builder = std::make_shared<SkRuntimeColorFilterBuilder>(std::move(result.effect));
    return rt::ret_long(tables().color_filters.add(std::move(f)));
  });
  r.bind(rcf, "nativeUpdateUniforms", "(JLjava/lang/String;FFFFI)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    float values[4] = {a.f32(3), a.f32(4), a.f32(5), a.f32(6)};
    std::lock_guard<std::mutex> guard(f->lock);
    update_float_uniforms(rt, *f->runtime_builder, name.c_str(), values, a.i32(7), false);
    f->instance = nullptr;
    return 0;
  });
  r.bind(rcf, "nativeUpdateUniforms", "(JLjava/lang/String;[FZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    if (a.h(3) == kNull) rt.throw_npe("values");
    std::string name = rt.string_utf8(a.h(2));
    rt::ArrayObject& arr = rt.array(a.h(3), "float[]");
    const float* ptr = reinterpret_cast<const float*>(arr.data.data());
    std::lock_guard<std::mutex> guard(f->lock);
    update_float_uniforms(rt, *f->runtime_builder, name.c_str(), ptr, arr.length, a.z(4));
    f->instance = nullptr;
    return 0;
  });
  r.bind(rcf, "nativeUpdateUniforms", "(JLjava/lang/String;IIIII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    std::string name = rt.string_utf8(a.h(2));
    int32_t values[4] = {a.i32(3), a.i32(4), a.i32(5), a.i32(6)};
    std::lock_guard<std::mutex> guard(f->lock);
    update_int_uniforms(rt, *f->runtime_builder, name.c_str(), values, a.i32(7));
    f->instance = nullptr;
    return 0;
  });
  r.bind(rcf, "nativeUpdateUniforms", "(JLjava/lang/String;[I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("uniformName");
    if (a.h(3) == kNull) rt.throw_npe("values");
    std::string name = rt.string_utf8(a.h(2));
    rt::ArrayObject& arr = rt.array(a.h(3), "int[]");
    const int32_t* ptr = reinterpret_cast<const int32_t*>(arr.data.data());
    std::lock_guard<std::mutex> guard(f->lock);
    update_int_uniforms(rt, *f->runtime_builder, name.c_str(), ptr, arr.length);
    f->instance = nullptr;
    return 0;
  });
  r.bind(rcf, "nativeUpdateChild", "(JLjava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("childName");
    std::string name = rt.string_utf8(a.h(2));
    std::lock_guard<std::mutex> guard(f->lock);
    auto c = f->runtime_builder->child(name.c_str());
    if (!c.fChild) rt.throw_new("Ljava/lang/IllegalArgumentException;", "child introuvable : " + name);
    sk_sp<SkShader> s = tables().shaders.get(a.i64(3)) ? lookup(rt, tables().shaders, a.i64(3), "Shader") : nullptr;
    if (s) {
      c = std::move(s);
    } else {
      sk_sp<SkColorFilter> cf = skia_color_filter(rt, a.i64(3));
      c = std::move(cf);
    }
    f->instance = nullptr;
    return 0;
  });
  r.bind(rcf, "nativeUpdateInputColorFilter", "(JLjava/lang/String;J)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto f = color_filter(rt, a.i64(0));
    if (!f->runtime_builder) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] RuntimeColorFilter invalide");
    if (a.h(2) == kNull) rt.throw_npe("childName");
    std::string name = rt.string_utf8(a.h(2));
    std::lock_guard<std::mutex> guard(f->lock);
    auto c = f->runtime_builder->child(name.c_str());
    if (!c.fChild) rt.throw_new("Ljava/lang/IllegalArgumentException;", "child introuvable : " + name);
    sk_sp<SkColorFilter> cf = skia_color_filter(rt, a.i64(3));
    c = std::move(cf);
    f->instance = nullptr;
    return 0;
  });
}

void register_mask_filters(rt::NativeRegistry& r) {
  r.bind("Landroid/graphics/MaskFilter;", "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().mask_filters.remove(a.i64(0));
    return 0;
  });
  r.bind("Landroid/graphics/BlurMaskFilter;", "nativeConstructor", "(FI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    sk_sp<SkMaskFilter> f = SkMaskFilter::MakeBlur(SkBlurStyle(std::clamp(a.i32(1), 0, int(kLastEnum_SkBlurStyle))),
                                                   radius_to_sigma(a.f32(0)));
    if (!f) throw_iae(rt);
    return rt::ret_long(add(tables().mask_filters, std::move(f)));
  });
  r.bind("Landroid/graphics/EmbossMaskFilter;", "nativeConstructor", "([FFFF)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<float> direction = floats_of(rt, a.h(0), 3);
    sk_sp<SkMaskFilter> f = SkBlurMaskFilter::MakeEmboss(radius_to_sigma(a.f32(3)), direction.data(), a.f32(1), a.f32(2));
    if (!f) throw_iae(rt);
    return rt::ret_long(add(tables().mask_filters, std::move(f)));
  });
  const char* table = "Landroid/graphics/TableMaskFilter;";
  r.bind(table, "nativeNewTable", "([B)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("table");
    rt::ArrayObject& t = rt.array(a.h(0), "byte[]");
    if (t.width != 1 || t.length < 256) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
    return rt::ret_long(add(tables().mask_filters, sk_sp<SkMaskFilter>(SkTableMaskFilter::Create(t.data.data()))));
  });
  r.bind(table, "nativeNewClip", "(II)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_long(add(tables().mask_filters,
                            sk_sp<SkMaskFilter>(SkTableMaskFilter::CreateClip(uint8_t(a.i32(0)), uint8_t(a.i32(1))))));
  });
  r.bind(table, "nativeNewGamma", "(F)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_long(add(tables().mask_filters, sk_sp<SkMaskFilter>(SkTableMaskFilter::CreateGamma(a.f32(0)))));
  });
}

void register_path_effects(rt::NativeRegistry& r) {
  r.bind("Landroid/graphics/PathEffect;", "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    tables().path_effects.remove(a.i64(0));
    return 0;
  });
  r.bind("Landroid/graphics/ComposePathEffect;", "nativeCreate", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    sk_sp<SkPathEffect> outer = lookup(rt, tables().path_effects, a.i64(0), "PathEffect");
    sk_sp<SkPathEffect> inner = lookup(rt, tables().path_effects, a.i64(2), "PathEffect");
    return rt::ret_long(add(tables().path_effects, SkPathEffect::MakeCompose(outer, inner)));
  });
  r.bind("Landroid/graphics/SumPathEffect;", "nativeCreate", "(JJ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    sk_sp<SkPathEffect> first = lookup(rt, tables().path_effects, a.i64(0), "PathEffect");
    sk_sp<SkPathEffect> second = lookup(rt, tables().path_effects, a.i64(2), "PathEffect");
    return rt::ret_long(add(tables().path_effects, SkPathEffect::MakeSum(first, second)));
  });
  r.bind("Landroid/graphics/DashPathEffect;", "nativeCreate", "([FF)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<float> intervals = floats_of(rt, a.h(0), 0);
    intervals.resize(intervals.size() & ~size_t(1));  // an even number
    return rt::ret_long(add(tables().path_effects, SkDashPathEffect::Make(intervals, a.f32(1))));
  });
  r.bind("Landroid/graphics/PathDashPathEffect;", "nativeCreate", "(JFFI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    SkPath shape = skia_path_of(rt, a.i64(0));
    auto style = SkPath1DPathEffect::Style(std::clamp(a.i32(4), 0, int(SkPath1DPathEffect::kLastEnum_Style)));
    return rt::ret_long(add(tables().path_effects, SkPath1DPathEffect::Make(shape, a.f32(2), a.f32(3), style)));
  });
  r.bind("Landroid/graphics/CornerPathEffect;", "nativeCreate", "(F)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_long(add(tables().path_effects, SkCornerPathEffect::Make(a.f32(0))));
  });
  r.bind("Landroid/graphics/DiscretePathEffect;", "nativeCreate", "(FF)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    return rt::ret_long(add(tables().path_effects, SkDiscretePathEffect::Make(a.f32(0), a.f32(1))));
  });
}

}  // namespace

sk_sp<SkShader> skia_shader(Runtime& rt, int64_t handle) { return lookup(rt, tables().shaders, handle, "Shader"); }

sk_sp<SkColorFilter> skia_color_filter(Runtime& rt, int64_t handle) {
  if (handle == 0) return nullptr;
  return color_filter(rt, handle)->get();
}

sk_sp<SkMaskFilter> skia_mask_filter(Runtime& rt, int64_t handle) {
  return lookup(rt, tables().mask_filters, handle, "MaskFilter");
}

sk_sp<SkPathEffect> skia_path_effect(Runtime& rt, int64_t handle) {
  return lookup(rt, tables().path_effects, handle, "PathEffect");
}

void register_framework_effects(rt::NativeRegistry& r) {
  register_shaders(r);
  register_color_filters(r);
  register_mask_filters(r);
  register_path_effects(r);
}

}  // namespace aa::bridge
