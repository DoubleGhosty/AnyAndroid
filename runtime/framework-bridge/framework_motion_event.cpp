// android.view.MotionEvent natives (J8 of ADR-006), each signature checked
// against the framework.jar of Android 17, as android_view_MotionEvent.cpp
// does it over libinput (motion_event.h): the same checks and exceptions, the
// PointerCoords and PointerProperties objects read and written through their
// fields, matrices through Matrix.native_instance, parcels through
// Parcel.mNativePtr. libinput's fatal errors (a process abort on Android)
// become a RuntimeException.
#include <array>
#include <bit>
#include <climits>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "framework-bridge/handles.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/motion_event.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;

constexpr int32_t kHistoryCurrent = INT32_MIN;  // MotionEvent.HISTORY_CURRENT

// Kind tag: unique across the runtime (tests/check_handle_kinds.cmake).
HandleTable<MotionEvent>& events() {
  static auto* t = new HandleTable<MotionEvent>(0x1127);  // never destroyed
  return *t;
}

std::shared_ptr<MotionEvent> native_event(Runtime& rt, int64_t h) {
  auto e = events().get(h);
  if (!e) rt.throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] MotionEvent invalide");
  return e;
}

// The event of a native pointer that may be 0 (a new event then).
std::shared_ptr<MotionEvent> event_or_new(Runtime& rt, int64_t h) {
  return h ? native_event(rt, h) : std::make_shared<MotionEvent>();
}

// The handle of an event made or reused by a native: the same, or a new one.
uint64_t handle_of(int64_t h, std::shared_ptr<MotionEvent> event) {
  return rt::ret_long(h ? h : events().add(std::move(event)));
}

// libinput's LOG(FATAL) aborts the process on Android.
template <typename Fn>
rt::NativeFn guarded(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t {
    try {
      return fn(rt, a);
    } catch (const MotionEventError& e) {
      rt.throw_new("Ljava/lang/RuntimeException;", std::string("[AnyAndroid] erreur fatale de libinput : ") + e.what());
    }
  };
}

// --- Fields of Java objects ---------------------------------------------------

rt::Field* field(Runtime& rt, rt::Object& o, const char* name, const char* type) {
  rt::Field* f = find_field(o.klass, name, type);
  if (!f || f->is_static()) rt.throw_new("Ljava/lang/NoSuchFieldError;", name);
  return f;
}

float get_float(Runtime& rt, rt::Object& o, const char* name) {
  uint32_t bits = o.fields.at(field(rt, o, name, "F")->slot);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

void set_float(Runtime& rt, rt::Object& o, const char* name, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  o.fields.at(field(rt, o, name, "F")->slot) = bits;
}

int32_t get_int(Runtime& rt, rt::Object& o, const char* name) {
  return int32_t(o.fields.at(field(rt, o, name, "I")->slot));
}
void set_int(Runtime& rt, rt::Object& o, const char* name, int32_t v) {
  o.fields.at(field(rt, o, name, "I")->slot) = uint32_t(v);
}

int64_t get_long(Runtime& rt, rt::Object& o, const char* name) {
  rt::Field* f = field(rt, o, name, "J");
  return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
}

void set_long(Runtime& rt, rt::Object& o, const char* name, int64_t v) {
  rt::Field* f = field(rt, o, name, "J");
  o.fields.at(f->slot) = uint32_t(uint64_t(v));
  o.fields.at(f->slot + 1) = uint32_t(uint64_t(v) >> 32);
}

// An element of a Java object array (GetObjectArrayElement).
Handle element(Runtime& rt, Handle array, size_t i) {
  rt::ArrayObject& a = rt.array(array, "Object[]");
  if (a.width != 4 || i >= a.length) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", std::to_string(i));
  Handle h;
  std::memcpy(&h, a.data.data() + i * 4, 4);
  return h;
}

// gPointerCoordsClassInfo: the axes kept in fields of MotionEvent.PointerCoords,
// in the order of pointerCoordsToNative.
struct AxisField {
  int32_t axis;
  const char* name;
};
constexpr AxisField kAxesFromMotionEvent[] = {
    {kAxisX, "x"},
    {kAxisY, "y"},
    {kAxisPressure, "pressure"},
    {kAxisSize, "size"},
    {kAxisTouchMajor, "touchMajor"},
    {kAxisTouchMinor, "touchMinor"},
    {kAxisToolMajor, "toolMajor"},
    {kAxisToolMinor, "toolMinor"},
    {kAxisOrientation, "orientation"},
    {kAxisRelativeX, "relativeX"},
    {kAxisRelativeY, "relativeY"},
};

PointerCoords pointer_coords_to_native(Runtime& rt, Handle obj) {
  rt::Object& o = rt.nonnull(obj, "pointerCoords");
  PointerCoords out;
  for (const auto& [axis, name] : kAxesFromMotionEvent) out.set_axis_value(axis, get_float(rt, o, name));
  out.is_resampled = o.fields.at(field(rt, o, "isResampled", "Z")->slot) != 0;
  uint64_t bits = uint64_t(get_long(rt, o, "mPackedAxisBits"));
  if (bits != 0) {
    Handle values = Handle(o.fields.at(field(rt, o, "mPackedAxisValues", "[F")->slot));
    if (values != kNull) {
      rt::ArrayObject& a = rt.array(values, "float[]");
      uint32_t index = 0;
      do {
        uint32_t axis = uint32_t(std::countl_zero(bits));  // clearFirstMarkedBit
        bits &= ~PointerCoords::bit_of(axis);
        if (a.width != 4 || index >= a.length) break;  // libinput would read past the array
        float v;
        std::memcpy(&v, a.data.data() + size_t(index++) * 4, 4);
        out.set_axis_value(int32_t(axis), v);
      } while (bits != 0);
    }
  }
  return out;
}

// obtainPackedAxisValuesArray: the object's array when it holds min_size
// values, else a new one of 8, 16, 32... values.
Handle obtain_packed_axis_values_array(Runtime& rt, uint32_t min_size, rt::Object& o) {
  rt::Field* f = field(rt, o, "mPackedAxisValues", "[F");
  Handle values = Handle(o.fields.at(f->slot));
  if (values != kNull && rt.array(values, "float[]").length >= min_size) return values;
  uint32_t size = 8;
  while (size < min_size) size *= 2;
  values = rt.new_array("[F", int32_t(size));
  o.fields.at(f->slot) = values;
  return values;
}

void pointer_coords_from_native(Runtime& rt, const PointerCoords& raw, uint64_t bits, rt::Object& o) {
  uint64_t out_bits = 0;
  if (bits != 0) {
    Handle values = obtain_packed_axis_values_array(rt, uint32_t(std::popcount(bits)), o);
    rt::ArrayObject& a = rt.array(values, "float[]");
    uint32_t index = 0;
    do {
      uint32_t axis = uint32_t(std::countl_zero(bits));
      bits &= ~PointerCoords::bit_of(axis);
      out_bits |= PointerCoords::bit_of(axis);
      float v = raw.axis_value(int32_t(axis));
      std::memcpy(a.data.data() + size_t(index++) * 4, &v, 4);
    } while (bits != 0);
  }
  set_long(rt, o, "mPackedAxisBits", int64_t(out_bits));
}

PointerProperties pointer_properties_to_native(Runtime& rt, Handle obj) {
  rt::Object& o = rt.nonnull(obj, "pointerProperties");
  return {get_int(rt, o, "id"), get_int(rt, o, "toolType")};
}

// AMatrix_getContents: Matrix.getValues's order, SkMatrix's.
std::array<float, 9> matrix_contents(Runtime& rt, Handle matrix) {
  rt::Object& o = rt.nonnull(matrix, "matrix");
  return skmatrix::of(rt, get_long(rt, o, "native_instance"));
}

// --- The checks of android_view_MotionEvent.cpp -------------------------------

void throw_iae(Runtime& rt, const std::string& message) {
  rt.throw_new("Ljava/lang/IllegalArgumentException;", message);
}

void validate_pointer_count(Runtime& rt, int32_t pointer_count) {
  if (pointer_count < 1) throw_iae(rt, "pointerCount must be at least 1");
}

void validate_pointer_properties_array(Runtime& rt, Handle array, size_t pointer_count) {
  if (array == kNull) throw_iae(rt, "pointerProperties array must not be null");
  if (size_t(rt.array(array, "PointerProperties[]").length) < pointer_count) {
    throw_iae(rt, "pointerProperties array must be large enough to hold all pointers");
  }
}

void validate_pointer_coords_array(Runtime& rt, Handle array, size_t pointer_count) {
  if (array == kNull) throw_iae(rt, "pointerCoords array must not be null");
  if (size_t(rt.array(array, "PointerCoords[]").length) < pointer_count) {
    throw_iae(rt, "pointerCoords array must be large enough to hold all pointers");
  }
}

void validate_pointer_index(Runtime& rt, int32_t pointer_index, const MotionEvent& event) {
  if (pointer_index < 0 || size_t(pointer_index) >= event.pointer_count()) {
    throw_iae(rt, "invalid pointerIndex " + std::to_string(pointer_index) + " for " + event.to_string());
  }
}

void validate_history_pos(Runtime& rt, int32_t history_pos, const MotionEvent& event) {
  if (history_pos < 0 || size_t(history_pos) >= event.history_size()) {
    throw_iae(rt, "historyPos " + std::to_string(history_pos) + " out of range for " + event.to_string());
  }
}

// --- Natives ------------------------------------------------------------------

const char* const kMotionEvent = "Landroid/view/MotionEvent;";

void bind(rt::NativeRegistry& r, const char* name, const char* sig, rt::NativeFn fn) {
  r.bind(kMotionEvent, name, sig, guarded(std::move(fn)));
}

// The natives that read or write one value of the event (@CriticalNative).
template <typename Fn>
rt::NativeFn with_event(Fn fn) {
  return [fn](Runtime& rt, NativeArgs& a) -> uint64_t { return fn(*native_event(rt, a.i64(0)), a); };
}

void register_lifecycle(rt::NativeRegistry& r) {
  bind(r, "nativeInitialize",
       "(JIIIIIIIIIFFFFJJI[Landroid/view/MotionEvent$PointerProperties;[Landroid/view/MotionEvent$PointerCoords;)J",
       [](Runtime& rt, NativeArgs& a) -> uint64_t {
         const int32_t pointer_count = a.i32(19);
         const Handle properties_array = a.h(20), coords_array = a.h(21);
         validate_pointer_count(rt, pointer_count);
         validate_pointer_properties_array(rt, properties_array, size_t(pointer_count));
         validate_pointer_coords_array(rt, coords_array, size_t(pointer_count));

         const int64_t ptr = a.i64(0);
         std::shared_ptr<MotionEvent> event = event_or_new(rt, ptr);
         const uint32_t source = uint32_t(a.i32(3));
         uint32_t flags = uint32_t(a.i32(6));
         UiTransform transform;
         transform.set(a.f32(11), a.f32(12));
         const UiTransform inverse_transform = transform.inverse();

         // libinput frees the event it was given when an element is null.
         auto drop = [&] {
           if (ptr) events().remove(ptr);
         };
         std::vector<PointerProperties> pointer_properties;
         std::vector<PointerCoords> raw_pointer_coords;
         pointer_properties.reserve(size_t(pointer_count));
         raw_pointer_coords.reserve(size_t(pointer_count));
         for (int32_t i = 0; i < pointer_count; i++) {
           Handle properties = element(rt, properties_array, size_t(i));
           if (properties == kNull) {
             drop();
             return 0;
           }
           pointer_properties.push_back(pointer_properties_to_native(rt, properties));
           Handle coords_object = element(rt, coords_array, size_t(i));
           if (coords_object == kNull) {
             drop();
             rt.throw_npe("pointerCoords");
           }
           PointerCoords& coords = raw_pointer_coords.emplace_back(pointer_coords_to_native(rt, coords_object));
           if (coords.axis_value(kAxisOrientation) != 0.f) {
             flags |= kMotionFlagSupportsOrientation | kMotionFlagSupportsDirectionalOrientation;
           }
           MotionEvent::calculate_transformed_coords_in_place(coords, source, flags, inverse_transform);
         }
         event->initialize(next_input_event_id(), a.i32(2), source, a.i32(4), MotionEvent::Hmac{} /* INVALID_HMAC */,
                           a.i32(5), 0, flags, a.i32(7), a.i32(8), a.i32(9), uint8_t(a.i32(10)), transform, a.f32(13),
                           a.f32(14), kInvalidCursorPosition, kInvalidCursorPosition, UiTransform(), a.i64(15),
                           a.i64(17), size_t(pointer_count), pointer_properties.data(), raw_pointer_coords.data());
         return handle_of(ptr, std::move(event));
       });
  bind(r, "nativeDispose", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    events().remove(a.i64(0));
    return 0;
  });
  bind(r, "nativeAddBatch", "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V",
       [](Runtime& rt, NativeArgs& a) -> uint64_t {
         auto event = native_event(rt, a.i64(0));
         const size_t pointer_count = event->pointer_count();
         validate_pointer_coords_array(rt, a.h(4), pointer_count);
         const UiTransform inverse_transform = event->transform().inverse();
         std::vector<PointerCoords> raw_pointer_coords;
         raw_pointer_coords.reserve(pointer_count);
         for (size_t i = 0; i < pointer_count; i++) {
           Handle coords_object = element(rt, a.h(4), i);
           if (coords_object == kNull) rt.throw_npe("pointerCoords");
           raw_pointer_coords.push_back(pointer_coords_to_native(rt, coords_object));
           MotionEvent::calculate_transformed_coords_in_place(raw_pointer_coords.back(), event->source(),
                                                              event->flags(), inverse_transform);
         }
         event->add_sample(a.i64(2), raw_pointer_coords.data(), event->id());
         event->set_meta_state(event->meta_state() | a.i32(5));
         return 0;
       });
  bind(r, "nativeCopy", "(JJZ)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const int64_t dest = a.i64(0);
    std::shared_ptr<MotionEvent> source = native_event(rt, a.i64(2));
    std::shared_ptr<MotionEvent> event = event_or_new(rt, dest);
    event->copy_from(*source, a.z(4));
    return handle_of(dest, std::move(event));
  });
  bind(r, "nativeSplit", "(JJI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const int64_t dest = a.i64(0);
    std::shared_ptr<MotionEvent> source = native_event(rt, a.i64(2));
    std::shared_ptr<MotionEvent> event = event_or_new(rt, dest);
    event->split_from(*source, MotionEvent::PointerIds(uint32_t(a.i32(4))), next_input_event_id());
    return handle_of(dest, std::move(event));
  });
}

void register_parcel(rt::NativeRegistry& r) {
  bind(r, "nativeReadFromParcel", "(JLandroid/os/Parcel;)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    const int64_t ptr = a.i64(0);
    std::shared_ptr<MotionEvent> event = event_or_new(rt, ptr);
    const Handle parcel = a.h(2);
    rt.nonnull(parcel, "parcel");
    MotionEvent::ParcelReader reader{
        [&]() -> std::optional<int32_t> {
          bool ok = true;
          int32_t v = parcel_read_i32(rt, parcel, &ok);
          return ok ? std::optional<int32_t>(v) : std::nullopt;
        },
        [&]() -> std::optional<int64_t> {
          bool ok = true;
          int64_t v = parcel_read_i64(rt, parcel, &ok);
          return ok ? std::optional<int64_t>(v) : std::nullopt;
        },
        [&]() -> std::optional<std::vector<uint8_t>> {
          bool ok = true;
          std::vector<uint8_t> v = parcel_read_byte_vector(rt, parcel, &ok);
          return ok ? std::optional<std::vector<uint8_t>>(std::move(v)) : std::nullopt;
        }};
    if (!event->read_from_parcel(reader)) {
      rt.throw_new("Ljava/lang/RuntimeException;", "Failed to read MotionEvent parcel.");
    }
    return handle_of(ptr, std::move(event));
  });
  bind(r, "nativeWriteToParcel", "(JLandroid/os/Parcel;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    const Handle parcel = a.h(2);
    rt.nonnull(parcel, "parcel");
    event->write_to_parcel({[&](int32_t v) { parcel_write_i32(rt, parcel, v); },
                            [&](int64_t v) { parcel_write_i64(rt, parcel, v); },
                            [&](const std::vector<uint8_t>& v) { parcel_write_byte_vector(rt, parcel, v); }});
    return 0;
  });
}

void register_pointers(rt::NativeRegistry& r) {
  bind(r, "nativeAxisToString", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto label = MotionEvent::axis_label(a.i32(0));
    return rt::ret_obj(label ? rt.new_string_utf8(*label) : kNull);
  });
  bind(r, "nativeAxisFromString", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe("label");  // ScopedUtfChars
    return rt::ret_int(MotionEvent::axis_from_label(rt.string_utf8(a.h(0))).value_or(-1));
  });
  bind(r, "nativeGetPointerProperties", "(JILandroid/view/MotionEvent$PointerProperties;)V",
       [](Runtime& rt, NativeArgs& a) -> uint64_t {
         auto event = native_event(rt, a.i64(0));
         validate_pointer_index(rt, a.i32(2), *event);
         if (a.h(3) == kNull) throw_iae(rt, "pointerProperties must not be null");
         const PointerProperties& properties = event->pointer_properties(size_t(a.i32(2)));
         rt::Object& o = rt.nonnull(a.h(3), "pointerProperties");
         set_int(rt, o, "id", properties.id);
         set_int(rt, o, "toolType", properties.tool_type);
         return 0;
       });
  bind(r, "nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V",
       [](Runtime& rt, NativeArgs& a) -> uint64_t {
         auto event = native_event(rt, a.i64(0));
         const int32_t pointer_index = a.i32(2), history_pos = a.i32(3);
         validate_pointer_index(rt, pointer_index, *event);
         if (a.h(4) == kNull) throw_iae(rt, "pointerCoords must not be null");
         if (history_pos != kHistoryCurrent) validate_history_pos(rt, history_pos, *event);
         const bool current = history_pos == kHistoryCurrent;
         const size_t index = size_t(pointer_index);
         rt::Object& o = rt.nonnull(a.h(4), "pointerCoords");
         // These axes come from the event (transformed), not from the raw coordinates.
         for (const auto& [axis, name] : kAxesFromMotionEvent) {
           set_float(rt, o, name,
                     current ? event->axis_value(axis, index)
                             : event->historical_axis_value(axis, index, size_t(history_pos)));
         }
         const PointerCoords& raw = current ? event->raw_pointer_coords(index)
                                            : event->historical_raw_pointer_coords(index, size_t(history_pos));
         uint64_t bits = raw.bits;
         for (const auto& [axis, name] : kAxesFromMotionEvent) bits &= ~PointerCoords::bit_of(uint32_t(axis));
         pointer_coords_from_native(rt, raw, bits, o);
         const bool resampled = current ? event->is_resampled(index, event->history_size())
                                        : event->is_resampled(index, size_t(history_pos));
         o.fields.at(field(rt, o, "isResampled", "Z")->slot) = resampled ? 1 : 0;
         return 0;
       });
  bind(r, "nativeGetPointerId", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    validate_pointer_index(rt, a.i32(2), *event);
    return rt::ret_int(event->pointer_id(size_t(a.i32(2))));
  });
  bind(r, "nativeGetToolType", "(JI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    validate_pointer_index(rt, a.i32(2), *event);
    return rt::ret_int(event->tool_type(size_t(a.i32(2))));
  });
  bind(r, "nativeGetEventTimeNanos", "(JI)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    const int32_t history_pos = a.i32(2);
    if (history_pos == kHistoryCurrent) return rt::ret_long(event->event_time());
    validate_history_pos(rt, history_pos, *event);
    return rt::ret_long(event->historical_event_time(size_t(history_pos)));
  });
  // getRawAxisValue and getAxisValue: through the raw transform or the window's.
  for (bool raw : {true, false}) {
    bind(r, raw ? "nativeGetRawAxisValue" : "nativeGetAxisValue", "(JIII)F",
         [raw](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto event = native_event(rt, a.i64(0));
           const int32_t axis = a.i32(2), pointer_index = a.i32(3), history_pos = a.i32(4);
           validate_pointer_index(rt, pointer_index, *event);
           const size_t index = size_t(pointer_index);
           if (history_pos == kHistoryCurrent) {
             return rt::ret_float(raw ? event->raw_axis_value(axis, index) : event->axis_value(axis, index));
           }
           validate_history_pos(rt, history_pos, *event);
           const size_t pos = size_t(history_pos);
           return rt::ret_float(raw ? event->historical_raw_axis_value(axis, index, pos)
                                    : event->historical_axis_value(axis, index, pos));
         });
  }
  bind(r, "nativeTransform", "(JLandroid/graphics/Matrix;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    event->transform(matrix_contents(rt, a.h(2)));
    return 0;
  });
  bind(r, "nativeApplyTransform", "(JLandroid/graphics/Matrix;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto event = native_event(rt, a.i64(0));
    event->apply_transform(matrix_contents(rt, a.h(2)));
    return 0;
  });
}

void register_accessors(rt::NativeRegistry& r) {
  bind(r, "nativeGetId", "(J)I", with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.id()); }));
  bind(r, "nativeGetDeviceId", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.device_id()); }));
  bind(r, "nativeGetSource", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(int32_t(e.source())); }));
  bind(r, "nativeSetSource", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_source(uint32_t(a.i32(2)));
         return 0;
       }));
  bind(r, "nativeGetDisplayId", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.display_id()); }));
  bind(r, "nativeSetDisplayId", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_display_id(a.i32(2));
         return 0;
       }));
  bind(r, "nativeGetAction", "(J)I", with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.action()); }));
  bind(r, "nativeSetAction", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_action(a.i32(2));
         return 0;
       }));
  bind(r, "nativeGetActionButton", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.action_button()); }));
  bind(r, "nativeSetActionButton", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_action_button(a.i32(2));
         return 0;
       }));
  bind(r, "nativeIsTouchEvent", "(J)Z",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_bool(e.is_touch_event()); }));
  // The private flags stay hidden from Java, and kept when Java sets the others.
  bind(r, "nativeGetFlags", "(J)I", with_event([](MotionEvent& e, NativeArgs&) {
         return rt::ret_int(int32_t(e.flags() & ~kMotionPrivateFlagMask));
       }));
  bind(r, "nativeSetFlags", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         const uint32_t private_flags = e.flags() & kMotionPrivateFlagMask;
         e.set_flags((uint32_t(a.i32(2)) & ~kMotionPrivateFlagMask) | private_flags);
         return 0;
       }));
  bind(r, "nativeGetEdgeFlags", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.edge_flags()); }));
  bind(r, "nativeSetEdgeFlags", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_edge_flags(a.i32(2));
         return 0;
       }));
  bind(r, "nativeGetMetaState", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.meta_state()); }));
  bind(r, "nativeGetButtonState", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.button_state()); }));
  bind(r, "nativeSetButtonState", "(JI)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_button_state(a.i32(2));
         return 0;
       }));
  bind(r, "nativeGetClassification", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(e.classification()); }));
  bind(r, "nativeOffsetLocation", "(JFF)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.offset_location(a.f32(2), a.f32(3));
         return 0;
       }));
  bind(r, "nativeGetRawXOffset", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.raw_x_offset()); }));
  bind(r, "nativeGetRawYOffset", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.raw_y_offset()); }));
  bind(r, "nativeGetXPrecision", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.x_precision()); }));
  bind(r, "nativeGetYPrecision", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.y_precision()); }));
  bind(r, "nativeGetXCursorPosition", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.x_cursor_position()); }));
  bind(r, "nativeGetYCursorPosition", "(J)F",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_float(e.y_cursor_position()); }));
  bind(r, "nativeSetCursorPosition", "(JFF)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_cursor_position(a.f32(2), a.f32(3));
         return 0;
       }));
  bind(r, "nativeGetDownTimeNanos", "(J)J",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_long(e.down_time()); }));
  bind(r, "nativeSetDownTimeNanos", "(JJ)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.set_down_time(a.i64(2));
         return 0;
       }));
  bind(r, "nativeGetPointerCount", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(int32_t(e.pointer_count())); }));
  bind(r, "nativeFindPointerIndex", "(JI)I", with_event([](MotionEvent& e, NativeArgs& a) {
         return rt::ret_int(int32_t(e.find_pointer_index(a.i32(2))));
       }));
  bind(r, "nativeGetHistorySize", "(J)I",
       with_event([](MotionEvent& e, NativeArgs&) { return rt::ret_int(int32_t(e.history_size())); }));
  bind(r, "nativeScale", "(JF)V", with_event([](MotionEvent& e, NativeArgs& a) -> uint64_t {
         e.scale(a.f32(2));
         return 0;
       }));
  bind(r, "nativeGetSurfaceRotation", "(J)I", with_event([](MotionEvent& e, NativeArgs&) {
         return rt::ret_int(e.surface_rotation().value_or(-1));
       }));
}

}  // namespace

rt::Handle obtain_motion_event(Runtime& rt, const MotionEvent& event) {
  auto copy = std::make_shared<MotionEvent>();
  copy->copy_from(event, true);
  Handle obj = Handle(rt.call_static(kMotionEvent, "obtain", "()Landroid/view/MotionEvent;"));
  rt::Object& o = rt.nonnull(obj, "MotionEvent.obtain()");
  if (int64_t old = get_long(rt, o, "mNativePtr")) events().remove(old);  // a recycled event's
  set_long(rt, o, "mNativePtr", events().add(std::move(copy)));
  return obj;
}

std::shared_ptr<MotionEvent> motion_event_of(Runtime& rt, Handle event) {
  if (event == kNull) return nullptr;
  return events().get(get_long(rt, rt.nonnull(event, "MotionEvent"), "mNativePtr"));
}

void register_framework_motion_event(rt::NativeRegistry& r) {
  register_lifecycle(r);
  register_parcel(r);
  register_pointers(r);
  register_accessors(r);
}

}  // namespace aa::bridge
