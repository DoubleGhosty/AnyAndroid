// android.graphics.Interpolator natives (libs/hwui/jni/Interpolator.cpp of
// android-17.0.0_r1), on a port of HWUI's SkiaInterpolator: key frames of
// values, each with a cubic blend towards the next (2.14 fixed point, as
// SkUnitCubicInterp), repeated and mirrored over time. View's scroll bars
// fade with it (ScrollabilityCache).
#include "framework-bridge/internal.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "framework-bridge/handles.h"

namespace aa::bridge {

namespace {

using rt::NativeArgs;
using rt::Runtime;

using Dot14 = int32_t;
constexpr Dot14 kDot14One = 1 << 14, kDot14Half = 1 << 13;

Dot14 dot14_mul(Dot14 a, Dot14 b) { return (a * b + kDot14Half) >> 14; }

Dot14 eval_cubic(Dot14 t, Dot14 a, Dot14 b, Dot14 c) { return dot14_mul(dot14_mul(dot14_mul(c, t) + b, t) + a, t); }

Dot14 pin_and_convert(float x) {
  if (x <= 0) return 0;
  if (x >= 1.0f) return kDot14One;
  return static_cast<Dot14>(x * kDot14One);
}

// SkUnitCubicInterp: y of the unit cubic (0,0) (bx,by) (cx,cy) (1,1) at x.
float unit_cubic_interp(float value, float bx, float by, float cx, float cy) {
  Dot14 x = pin_and_convert(value);
  if (x == 0) return 0.0f;
  if (x == kDot14One) return 1.0f;
  Dot14 b = pin_and_convert(bx), c = pin_and_convert(cx);
  Dot14 a3 = 3 * b, b3 = 3 * (c - 2 * b), c3 = 3 * (b - c) + kDot14One;
  Dot14 t = kDot14Half, dt = kDot14Half;
  for (int i = 0; i < 13; i++) {
    dt >>= 1;
    if (x < eval_cubic(t, a3, b3, c3)) {
      t -= dt;
    } else {
      t += dt;
    }
  }
  b = pin_and_convert(by), c = pin_and_convert(cy);
  a3 = 3 * b, b3 = 3 * (c - 2 * b), c3 = 3 * (b - c) + kDot14One;
  return float(eval_cubic(t, a3, b3, c3)) / 16384.f;
}

constexpr std::array<float, 4> kIdentityBlend = {0.33333333f, 0.33333333f, 0.66666667f, 0.66666667f};

// SkiaInterpolator, its storage in vectors.
struct Interpolator {
  enum Result { kNormal = 0, kFreezeStart = 1, kFreezeEnd = 2 };
  enum Flags { kMirror = 1, kReset = 2 };
  struct TimeCode {
    uint32_t time = 0;
    std::array<float, 4> blend{};
  };

  int16_t frame_count = 0;
  uint8_t elem_count = 0;
  uint8_t flags = 0;
  float repeat = 1.0f;
  std::vector<TimeCode> times;
  std::vector<float> values;

  void reset(int elems, int frames) {
    flags = 0;
    elem_count = static_cast<uint8_t>(elems);
    frame_count = static_cast<int16_t>(frames);
    repeat = 1.0f;
    times.assign(size_t(std::max<int>(frame_count, 0)), TimeCode{});
    values.assign(size_t(std::max<int>(frame_count, 0)) * elem_count, 0.0f);
  }

  // Index of target, or ~ the index where it would go.
  static int binary_search(const TimeCode* arr, int count, uint32_t target) {
    if (count <= 0) return ~0;
    int lo = 0, hi = count - 1;
    while (lo < hi) {
      int mid = (hi + lo) / 2;
      uint32_t elem = arr[mid].time;
      if (elem == target) return mid;
      if (elem < target) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (target < arr[lo].time) return ~lo;
    return ~(lo + 1);
  }

  bool set_key_frame(int index, uint32_t time, const float* src, const float* blend) {
    if (!blend) blend = kIdentityBlend.data();
    bool success = ~index == binary_search(times.data(), index, time);
    if (success) {
      times[size_t(index)].time = time;
      std::memcpy(times[size_t(index)].blend.data(), blend, sizeof(float) * 4);
      std::memcpy(&values[size_t(elem_count) * size_t(index)], src, elem_count * sizeof(float));
    }
    return success;
  }

  Result time_to_t(uint32_t time, float* t, int* index_out, bool* exact_out) const {
    Result result = kNormal;
    if (repeat != 1.0f) {
      uint32_t start = times[0].time, end = times[size_t(frame_count) - 1].time;
      uint32_t total = end - start, offset = time - start;
      end = uint32_t(int32_t(std::floor(repeat * float(total))));
      if (offset >= end) {
        float fraction = repeat - std::floor(repeat);  // SkScalarFraction
        offset = fraction == 0 && repeat > 0 ? total : uint32_t(int32_t(std::floor(fraction * float(total))));
        result = kFreezeEnd;
      } else if (total == 0) {
        offset = 0;  // a single key time: SkiaInterpolator would divide by zero
      } else {
        int mirror = flags & kMirror;
        offset = offset % (total << mirror);
        if (offset > total) offset = (total << 1) - offset;
      }
      time = offset + start;
    }
    int index = binary_search(times.data(), frame_count, time);
    bool exact = true;
    if (index < 0) {
      index = ~index;
      if (index == 0) {
        result = kFreezeStart;
      } else if (index == frame_count) {
        index = (flags & kReset) ? 0 : index - 1;
        result = kFreezeEnd;
      } else {
        exact = false;
      }
    }
    uint32_t next = times[size_t(index)].time;
    if (exact) {
      *t = 0;
    } else {
      const TimeCode& prev = times[size_t(index) - 1];
      float r = float(time - prev.time) / float(next - prev.time);
      *t = unit_cubic_interp(r, prev.blend[0], prev.blend[1], prev.blend[2], prev.blend[3]);
    }
    *index_out = index;
    *exact_out = exact;
    return result;
  }

  Result time_to_values(uint32_t time, float* out) const {
    if (frame_count <= 0) return kFreezeStart;  // SkiaInterpolator asserts key frames
    float t;
    int index;
    bool exact;
    Result result = time_to_t(time, &t, &index, &exact);
    if (out) {
      const float* next = &values[size_t(index) * elem_count];
      if (exact) {
        std::memcpy(out, next, elem_count * sizeof(float));
      } else {
        const float* prev = next - elem_count;
        for (int i = elem_count - 1; i >= 0; --i) out[i] = prev[i] + (next[i] - prev[i]) * t;  // SkScalarInterp
      }
    }
    return result;
  }
};

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<Interpolator>& interpolators() {
  static auto* t = new HandleTable<Interpolator>(0x112A);  // never destroyed
  return *t;
}

std::shared_ptr<Interpolator> interpolator(Runtime& rt, int64_t h) {
  auto i = interpolators().get(h);
  if (!i) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] Interpolator invalide");
  return i;
}

// The elements of a float[] (count of them at least, else AIOOBE as
// AutoJavaFloatArray), or nothing for null.
std::vector<float> floats(Runtime& rt, rt::Handle array, size_t at_least) {
  if (array == rt::kNull) return {};
  rt::ArrayObject& a = rt.array(array, "float[]");
  if (a.length < at_least) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "");
  std::vector<float> v(a.length);
  if (a.length) std::memcpy(v.data(), a.data.data(), a.length * sizeof(float));
  return v;
}

}  // namespace

void register_framework_interpolator(rt::NativeRegistry& r) {
  const char* cls = "Landroid/graphics/Interpolator;";
  r.bind(cls, "nativeConstructor", "(II)J", [](Runtime&, NativeArgs& a) -> uint64_t {
    auto i = std::make_shared<Interpolator>();
    i->reset(a.i32(0), a.i32(1));
    return rt::ret_long(interpolators().add(std::move(i)));
  });
  r.bind(cls, "nativeDestructor", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    interpolators().remove(a.i64(0));
    return 0;
  });
  r.bind(cls, "nativeReset", "(JII)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    interpolator(rt, a.i64(0))->reset(a.i32(2), a.i32(3));
    return 0;
  });
  r.bind(cls, "nativeSetKeyFrame", "(JII[F[F)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto i = interpolator(rt, a.i64(0));
    int32_t index = a.i32(2);
    if (a.h(4) == rt::kNull) rt.throw_npe("values");
    std::vector<float> values = floats(rt, a.h(4), i->elem_count);
    std::vector<float> blend = floats(rt, a.h(5), 4);
    if (index < 0 || index >= i->frame_count) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "");
    i->set_key_frame(index, uint32_t(a.i32(3)), values.data(), blend.empty() ? nullptr : blend.data());
    return 0;
  });
  r.bind(cls, "nativeSetRepeatMirror", "(JFZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto i = interpolator(rt, a.i64(0));
    float repeat = a.f32(2);
    if (repeat > 32000) repeat = 32000;
    i->repeat = repeat;
    i->flags = static_cast<uint8_t>((i->flags & ~Interpolator::kMirror) | (a.i32(3) != 0 ? 1 : 0));
    return 0;
  });
  r.bind(cls, "nativeTimeToValues", "(JI[F)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto i = interpolator(rt, a.i64(0));
    rt::Handle out = a.h(3);
    std::vector<float> values;
    if (out != rt::kNull) {
      values = floats(rt, out, i->elem_count);
    }
    Interpolator::Result result = i->time_to_values(uint32_t(a.i32(2)), values.empty() ? nullptr : values.data());
    if (out != rt::kNull && !values.empty()) {
      rt::ArrayObject& arr = rt.array(out, "float[]");
      std::memcpy(arr.data.data(), values.data(), values.size() * sizeof(float));
    }
    return rt::ret_int(int32_t(result));
  });
}

}  // namespace aa::bridge
