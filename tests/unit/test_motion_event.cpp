// libinput's MotionEvent (framework-bridge/motion_event.h): packed pointer
// coordinates, ui::Transform, the history, the transformation rules of each
// axis, splitting, the parcel layout and the axis labels; then the
// android.view.MotionEvent natives, called by a "mini framework" declaring
// them as framework.jar does (the signatures of android_view_MotionEvent.cpp).
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/internal.h"
#include "framework-bridge/motion_event.h"
#include "smali.h"
#include "test.h"

using namespace aa;
using bridge::MotionEvent;
using bridge::PointerCoords;
using bridge::PointerProperties;
using bridge::UiTransform;

namespace {

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kPi = 3.14159265358979f;

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

PointerCoords coords(float x, float y) {
  PointerCoords c;
  c.set_axis_value(bridge::kAxisX, x);
  c.set_axis_value(bridge::kAxisY, y);
  return c;
}

UiTransform translate(float tx, float ty) {
  UiTransform t;
  t.set(tx, ty);
  return t;
}

UiTransform matrix(const std::array<float, 9>& m) {
  UiTransform t;
  t.set(m);
  return t;
}

// A touch event with one pointer per coordinate (ids 0, 1, 2...), at 1000 ns,
// pressed at 500 ns.
MotionEvent event(int32_t action, std::vector<PointerCoords> pointers, uint32_t source = bridge::kSourceTouchscreen,
                  uint32_t flags = 0, const UiTransform& transform = {}, const UiTransform& raw_transform = {}) {
  std::vector<PointerProperties> properties;
  for (size_t i = 0; i < pointers.size(); i++) properties.push_back({int32_t(i), bridge::kToolTypeFinger});
  MotionEvent e;
  e.initialize(0x2a, 3, source, 0, {}, action, 0, flags, 0, 0, 0, 0, transform, 1, 1, kNaN, kNaN, raw_transform, 500,
               1000, pointers.size(), properties.data(), pointers.data());
  return e;
}

// libbinder's layout over a byte buffer: 4-byte words, int64 in 8 bytes,
// byte vectors as an int32 length and the bytes padded to 4.
struct Buffer {
  std::vector<uint8_t> data;
  size_t pos = 0;

  void put(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    data.insert(data.end(), b, b + n);
    while (data.size() % 4) data.push_back(0);
  }
  template <typename T>
  std::optional<T> get() {
    if (pos + sizeof(T) > data.size()) return std::nullopt;
    T v;
    std::memcpy(&v, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    return v;
  }
  MotionEvent::ParcelWriter writer() {
    return {[this](int32_t v) { put(&v, 4); }, [this](int64_t v) { put(&v, 8); },
            [this](const std::vector<uint8_t>& v) {
              int32_t n = int32_t(v.size());
              put(&n, 4);
              put(v.data(), v.size());
            }};
  }
  MotionEvent::ParcelReader reader() {
    return {[this] { return get<int32_t>(); }, [this] { return get<int64_t>(); },
            [this]() -> std::optional<std::vector<uint8_t>> {
              auto n = get<int32_t>();
              size_t padded = n ? (size_t(*n) + 3) & ~size_t(3) : 0;
              if (!n || *n < 0 || pos + padded > data.size()) return std::nullopt;
              std::vector<uint8_t> v(data.begin() + std::ptrdiff_t(pos), data.begin() + std::ptrdiff_t(pos) + *n);
              pos += padded;
              return v;
            }};
  }
};

}  // namespace

TEST(motion_event_pointer_coords_pack_axes_as_bitset64) {
  PointerCoords c;
  CHECK(c.set_axis_value(bridge::kAxisY, 2));
  CHECK(c.set_axis_value(bridge::kAxisGeneric1, 7));
  CHECK(c.set_axis_value(bridge::kAxisX, 1));
  // Axis n is bit 63 - n, the values in the order of the axes.
  CHECK_EQ(PointerCoords::bit_of(0), uint64_t(0x8000000000000000ULL));
  CHECK_EQ(c.bits, uint64_t(0xC000000080000000ULL));
  CHECK_EQ(c.values[0], 1.f);
  CHECK_EQ(c.values[1], 2.f);
  CHECK_EQ(c.values[2], 7.f);
  CHECK_EQ(c.axis_value(bridge::kAxisGeneric1), 7.f);
  // A missing axis set to 0 is not stored; a present one keeps its place.
  CHECK(c.set_axis_value(bridge::kAxisPressure, 0));
  CHECK(c.set_axis_value(bridge::kAxisY, 0));
  CHECK_EQ(c.bits, uint64_t(0xC000000080000000ULL));
  CHECK_EQ(c.axis_value(bridge::kAxisY), 0.f);
  CHECK(!c.set_axis_value(64, 1));
  CHECK(!c.set_axis_value(-1, 1));
  CHECK_EQ(c.axis_value(64), 0.f);
  CHECK_EQ(c.axis_value(bridge::kAxisPressure), 0.f);

  // At most 30 axes.
  PointerCoords full;
  for (int32_t axis = 0; axis < 30; axis++) CHECK(full.set_axis_value(axis, float(axis + 1)));
  CHECK(!full.set_axis_value(30, 1));
  CHECK_EQ(full.axis_value(29), 30.f);
  CHECK_EQ(full.axis_value(30), 0.f);

  // The window scale for X/Y and the relative axes, the global one for the
  // sizes, nothing for pressure.
  PointerCoords s = coords(10, 20);
  s.set_axis_value(bridge::kAxisTouchMajor, 4);
  s.set_axis_value(bridge::kAxisPressure, 0.5f);
  s.set_axis_value(bridge::kAxisRelativeX, 2);
  s.scale(2, 3, 4);
  CHECK_EQ(s.x(), 30.f);
  CHECK_EQ(s.y(), 80.f);
  CHECK_EQ(s.axis_value(bridge::kAxisTouchMajor), 8.f);
  CHECK_EQ(s.axis_value(bridge::kAxisPressure), 0.5f);
  CHECK_EQ(s.axis_value(bridge::kAxisRelativeX), 6.f);

  PointerCoords copy = s;
  CHECK(copy == s);
  copy.is_resampled = true;
  CHECK(!(copy == s));
}

TEST(motion_event_ui_transform_types_and_inverse) {
  UiTransform t;
  CHECK_EQ(t.type_mask(), uint32_t(UiTransform::kIdentity));
  t.set(3, 4);
  CHECK_EQ(t.type_mask(), uint32_t(UiTransform::kTranslate));
  CHECK_EQ(t.transform(1, 1).x, 4.f);
  CHECK_EQ(t.inverse().transform(4, 5).y, 1.f);

  // A rotation of 90 degrees and a translation (SkMatrix's row-major order).
  UiTransform r = matrix({0, -1, 10, 1, 0, 20, 0, 0, 1});
  CHECK_EQ(r.orientation(), uint32_t(UiTransform::kRot90));
  CHECK_EQ(r.type_mask(), uint32_t(UiTransform::kRotate | UiTransform::kTranslate));
  bridge::Vec2 p = r.transform(1, 0);
  CHECK_EQ(p.x, 10.f);
  CHECK_EQ(p.y, 21.f);
  bridge::Vec2 back = r.inverse().transform(10, 21);
  CHECK_EQ(back.x, 1.f);
  CHECK_EQ(back.y, 0.f);
  CHECK_EQ(r.inverse().orientation(), uint32_t(UiTransform::kRot270));
  bridge::Vec2 q = (r * t).transform(1, 1);  // t first
  CHECK_EQ(q.x, 5.f);
  CHECK_EQ(q.y, 24.f);

  CHECK_EQ(matrix({2, 0, 0, 0, 2, 0, 0, 0, 1}).type_mask(), uint32_t(UiTransform::kScale));
  CHECK_EQ(matrix({-1, 0, 0, 0, -1, 0, 0, 0, 1}).orientation(), uint32_t(UiTransform::kRot180));
  UiTransform skew = matrix({1, 0.5f, 0, 0, 1, 0, 0, 0, 1});
  CHECK_EQ(skew.orientation(), uint32_t(UiTransform::kRotInvalid));
  CHECK_EQ(skew.type_mask(), uint32_t(UiTransform::kUnknown));
}

TEST(motion_event_history_samples_and_copies) {
  MotionEvent e = event(bridge::kMotionActionMove, {coords(1, 2), coords(3, 4)});
  std::vector<PointerCoords> next = {coords(5, 6), coords(7, 8)};
  e.add_sample(2000, next.data(), 0x2b);
  CHECK_EQ(e.history_size(), size_t(1));
  CHECK_EQ(e.event_time(), int64_t(2000));
  CHECK_EQ(e.historical_event_time(0), int64_t(1000));
  CHECK_EQ(e.down_time(), int64_t(500));
  CHECK_EQ(e.x(1), 7.f);
  CHECK_EQ(e.historical_axis_value(bridge::kAxisX, 1, 0), 3.f);
  CHECK_EQ(e.id(), 0x2b);  // the id of the last sample
  CHECK_EQ(e.find_pointer_index(1), int64_t(1));
  CHECK_EQ(e.find_pointer_index(5), int64_t(-1));
  CHECK_THROWS(e.historical_raw_pointer_coords(2, 0), bridge::MotionEventError);
  CHECK_THROWS(e.historical_raw_pointer_coords(0, 2), bridge::MotionEventError);

  MotionEvent latest;
  latest.copy_from(e, false);
  CHECK_EQ(latest.history_size(), size_t(0));
  CHECK_EQ(latest.event_time(), int64_t(2000));
  CHECK_EQ(latest.x(0), 5.f);
  CHECK_EQ(latest.id(), 0x2b);
  MotionEvent all;
  all.copy_from(e, true);
  CHECK(all == e);
  all.copy_from(all, false);  // the same event: its last sample
  CHECK_EQ(all.history_size(), size_t(0));
  CHECK_EQ(all.y(1), 8.f);
}

TEST(motion_event_window_transform_versus_raw_coordinates) {
  PointerCoords c = coords(10, 20);
  c.set_axis_value(bridge::kAxisRelativeX, 3);
  c.set_axis_value(bridge::kAxisRelativeY, 4);
  MotionEvent e = event(bridge::kMotionActionDown, {c}, bridge::kSourceTouchscreen, 0, translate(100, 200));
  CHECK_EQ(e.x(0), 110.f);
  CHECK_EQ(e.y(0), 220.f);
  CHECK_EQ(e.raw_axis_value(bridge::kAxisX, 0), 10.f);
  CHECK_EQ(e.raw_axis_value(bridge::kAxisY, 0), 20.f);
  CHECK_EQ(e.axis_value(bridge::kAxisRelativeX, 0), 3.f);  // never translated
  CHECK_EQ(e.raw_x_offset(), 100.f);
  CHECK_EQ(e.raw_y_offset(), 200.f);

  e.offset_location(5, 6);
  CHECK_EQ(e.x(0), 115.f);
  CHECK_EQ(e.y(0), 226.f);
  CHECK_EQ(e.raw_axis_value(bridge::kAxisX, 0), 10.f);
  CHECK_EQ(e.raw_x_offset(), 105.f);

  // scale: the translations, the precisions and the samples.
  e.scale(2);
  CHECK_EQ(e.transform().tx(), 210.f);
  CHECK_EQ(e.transform().ty(), 412.f);
  CHECK_EQ(e.raw_axis_value(bridge::kAxisX, 0), 20.f);
  CHECK_EQ(e.x(0), 230.f);
  CHECK_EQ(e.y(0), 452.f);
  CHECK_EQ(e.axis_value(bridge::kAxisRelativeY, 0), 8.f);
  CHECK_EQ(e.x_precision(), 2.f);

  // transform: a matrix before the window's transform, the raw values kept.
  e.transform({0, -1, 500, 1, 0, 0, 0, 0, 1});
  CHECK_EQ(e.x(0), 48.f);
  CHECK_EQ(e.y(0), 230.f);
  CHECK_EQ(e.raw_axis_value(bridge::kAxisX, 0), 20.f);
  CHECK_EQ(e.axis_value(bridge::kAxisRelativeX, 0), -8.f);
  CHECK_EQ(e.axis_value(bridge::kAxisRelativeY, 0), 6.f);
  CHECK(e.surface_rotation() == 3);  // ROTATION_270
  CHECK(event(0, {c}).surface_rotation() == 0);
  CHECK(!event(0, {c}, bridge::kSourceTouchscreen, 0, matrix({1, 0.5f, 0, 0, 1, 0, 0, 0, 1})).surface_rotation());

  // Touchpads (class POSITION) are never transformed, navigation sources
  // (a trackball) are not translated.
  CHECK_EQ(event(0, {c}, bridge::kSourceTouchpad, 0, translate(100, 200)).x(0), 10.f);
  MotionEvent ball = event(0, {c}, bridge::kSourceTrackball, 0, matrix({2, 0, 100, 0, 2, 200, 0, 0, 1}));
  CHECK_EQ(ball.x(0), 20.f);
  CHECK_EQ(ball.y(0), 40.f);
  CHECK(!MotionEvent::is_touch_event(bridge::kSourceTrackball, bridge::kMotionActionDown));
  CHECK(MotionEvent::is_touch_event(bridge::kSourceTouchscreen, bridge::kMotionActionPointerUp | 0x100));
  CHECK(!MotionEvent::is_touch_event(bridge::kSourceMouse, bridge::kMotionActionHoverMove));

  // applyTransform: the samples and the cursor, not the transform.
  MotionEvent a = event(0, {coords(10, 20)}, bridge::kSourceTouchscreen, 0, translate(100, 200));
  a.set_cursor_position(101, 202);
  CHECK_EQ(a.raw_x_cursor_position(), 1.f);
  a.apply_transform({2, 0, 0, 0, 2, 0, 0, 0, 1});
  CHECK_EQ(a.raw_axis_value(bridge::kAxisX, 0), 20.f);
  CHECK_EQ(a.x(0), 120.f);
  CHECK_EQ(a.x_cursor_position(), 102.f);
  CHECK_EQ(a.y_cursor_position(), 204.f);
  CHECK(std::isnan(event(0, {c}).x_cursor_position()));
}

TEST(motion_event_orientation_follows_the_flags) {
  PointerCoords c = coords(1, 1);
  c.set_axis_value(bridge::kAxisOrientation, 3.0f);  // nearly down
  using bridge::kMotionFlagSupportsDirectionalOrientation;
  using bridge::kMotionFlagSupportsOrientation;
  // Without SUPPORTS_ORIENTATION the orientation reads 0; not directional,
  // it is folded into [-pi/2, pi/2].
  CHECK_EQ(event(0, {c}).axis_value(bridge::kAxisOrientation, 0), 0.f);
  MotionEvent folded = event(0, {c}, bridge::kSourceTouchscreen, kMotionFlagSupportsOrientation);
  CHECK(near(folded.axis_value(bridge::kAxisOrientation, 0), 3.0f - kPi));
  CHECK(near(folded.raw_axis_value(bridge::kAxisOrientation, 0), 3.0f - kPi));
  MotionEvent directional = event(0, {c}, bridge::kSourceTouchscreen,
                                  kMotionFlagSupportsOrientation | kMotionFlagSupportsDirectionalOrientation);
  CHECK(near(directional.axis_value(bridge::kAxisOrientation, 0), 3.0f));
  // Up, through a rotation of 90 degrees: right.
  PointerCoords up = coords(1, 1);
  MotionEvent rotated = event(0, {up}, bridge::kSourceTouchscreen, kMotionFlagSupportsOrientation,
                              matrix({0, -1, 0, 1, 0, 0, 0, 0, 1}));
  CHECK(near(rotated.axis_value(bridge::kAxisOrientation, 0), kPi / 2));
}

TEST(motion_event_split_keeps_the_requested_pointers) {
  const int32_t pointer_down_1 = bridge::kMotionActionPointerDown | (1 << 8);
  MotionEvent e = event(pointer_down_1, {coords(1, 1), coords(2, 2), coords(3, 3)});
  std::vector<PointerCoords> next = {coords(10, 10), coords(20, 20), coords(30, 30)};
  e.add_sample(2000, next.data(), 0x2a);

  MotionEvent s;
  s.split_from(e, 0b010, 99);  // only the pointer going down: DOWN
  CHECK_EQ(s.action(), bridge::kMotionActionDown);
  CHECK_EQ(s.pointer_count(), size_t(1));
  CHECK_EQ(s.pointer_id(0), 1);
  CHECK_EQ(s.x(0), 20.f);
  CHECK_EQ(s.history_size(), size_t(1));
  CHECK_EQ(s.historical_axis_value(bridge::kAxisX, 0, 0), 2.f);
  CHECK_EQ(s.id(), 99);
  CHECK_EQ(s.down_time(), int64_t(500));
  s.split_from(e, 0b011, 99);  // the pointer is the second one of the split event
  CHECK_EQ(s.action(), pointer_down_1);
  s.split_from(e, 0b101, 99);  // without the pointer going down: MOVE
  CHECK_EQ(s.action(), bridge::kMotionActionMove);
  CHECK_EQ(s.y(1), 30.f);

  const int32_t pointer_up_1 = bridge::kMotionActionPointerUp | (1 << 8);
  s.split_from(event(pointer_up_1, {coords(1, 1), coords(2, 2)}), 0b10, 1);
  CHECK_EQ(s.action(), bridge::kMotionActionUp);
  s.split_from(
      event(pointer_up_1, {coords(1, 1), coords(2, 2)}, bridge::kSourceTouchscreen, bridge::kMotionFlagCanceled), 0b10,
      1);
  CHECK_EQ(s.action(), bridge::kMotionActionCancel);

  // A missing id: the other event as it is (libinput logs the error).
  s.split_from(e, 0b100010, 7);
  CHECK_EQ(s.pointer_count(), size_t(3));
  CHECK_EQ(s.action(), pointer_down_1);
  CHECK_EQ(s.id(), 7);
  // An action index beyond the pointers, or an id out of the bitset: fatal.
  MotionEvent bad = event(bridge::kMotionActionPointerDown | (5 << 8), {coords(1, 1)});
  CHECK_THROWS(s.split_from(bad, 0b1, 1), bridge::MotionEventError);
  std::vector<PointerProperties> far = {{40, bridge::kToolTypeFinger}};
  PointerCoords one = coords(1, 1);
  MotionEvent far_id;
  far_id.initialize(1, 0, bridge::kSourceTouchscreen, 0, {}, 0, 0, 0, 0, 0, 0, 0, {}, 1, 1, kNaN, kNaN, {}, 0, 0, 1,
                    far.data(), &one);
  CHECK_THROWS(s.split_from(far_id, 1u << 8, 1), bridge::MotionEventError);
}

TEST(motion_event_parcel_round_trip) {
  PointerCoords a = coords(1.5f, 2.5f);
  a.set_axis_value(bridge::kAxisGeneric1, 9);
  PointerCoords b = coords(3, 4);
  b.is_resampled = true;
  std::vector<PointerProperties> properties = {{3, bridge::kToolTypeFinger}, {7, bridge::kToolTypeStylus}};
  std::vector<PointerCoords> first = {a, b}, second = {coords(5, 6), coords(7, 8)};
  MotionEvent e;
  e.initialize(0x1234, 6, bridge::kSourceTouchscreen, 5, {}, bridge::kMotionActionMove, 0, 0x81, 4, 1, 2, 2,
               matrix({0, -1, 10, 1, 0, 20, 0, 0, 1}), 0.5f, 0.25f, kNaN, kNaN, translate(-3, -4), 111, 222, 2,
               properties.data(), first.data());
  e.add_sample(333, second.data(), 0x1235);

  Buffer out;
  e.write_to_parcel(out.writer());
  int32_t words[4];
  std::memcpy(words, out.data.data(), sizeof words);
  CHECK_EQ(words[0], 2);  // pointers
  CHECK_EQ(words[1], 2);  // samples
  CHECK_EQ(words[2], 0x1235);
  CHECK_EQ(words[3], 6);
  MotionEvent back;
  CHECK(back.read_from_parcel(out.reader()));
  CHECK(back == e);
  CHECK_EQ(back.historical_axis_value(bridge::kAxisGeneric1, 0, 0), 9.f);
  CHECK(back.is_resampled(1, 0));

  // libbinder's value reads give 0 past the end: only the counts, the hmac
  // and the transforms are checked.
  Buffer truncated;
  truncated.data.assign(out.data.begin(), out.data.end() - 4);
  CHECK(MotionEvent().read_from_parcel(truncated.reader()));
  truncated.data.resize(40);  // in the hmac
  truncated.pos = 0;
  CHECK(!MotionEvent().read_from_parcel(truncated.reader()));
  truncated.data.assign(out.data.begin(), out.data.begin() + 100);  // in the transform
  truncated.pos = 0;
  CHECK(!MotionEvent().read_from_parcel(truncated.reader()));
  // Counts: no pointer, more than MAX_POINTERS.
  for (int32_t count : {0, 17, -1}) {
    Buffer bad = out;
    bad.pos = 0;
    std::memcpy(bad.data.data(), &count, 4);
    CHECK(!MotionEvent().read_from_parcel(bad.reader()));
  }
  // An hmac of 31 bytes.
  Buffer short_hmac = out;
  int32_t n = 31;
  std::memcpy(short_hmac.data.data() + 24, &n, 4);
  CHECK(!MotionEvent().read_from_parcel(short_hmac.reader()));
  // More than 30 axes in the first sample (its bits after 160 bytes of
  // header, 16 of pointer properties and the sample's time).
  Buffer axes = out;
  std::memset(axes.data.data() + 184, 0xff, 8);
  CHECK(!MotionEvent().read_from_parcel(axes.reader()));
}

TEST(motion_event_labels_and_descriptions) {
  CHECK(MotionEvent::axis_label(0) == "X");
  CHECK(MotionEvent::axis_label(bridge::kAxisRelativeY) == "RELATIVE_Y");
  CHECK(MotionEvent::axis_label(29) == "RESERVED_29");
  CHECK(MotionEvent::axis_label(32) == "GENERIC_1");
  CHECK(MotionEvent::axis_label(53) == "GESTURE_SWIPE_FINGER_COUNT");
  CHECK(!MotionEvent::axis_label(54));
  CHECK(!MotionEvent::axis_label(-1));
  CHECK(MotionEvent::axis_from_label("GENERIC_16") == 47);
  CHECK(MotionEvent::axis_from_label("RESERVED_31") == 31);
  CHECK(!MotionEvent::axis_from_label("x"));
  CHECK_EQ(MotionEvent::action_to_string(bridge::kMotionActionPointerDown | (2 << 8)), std::string("POINTER_DOWN(2)"));
  CHECK_EQ(MotionEvent::action_to_string(99), std::string("99"));
  CHECK_EQ(bridge::input_event_source_to_string(int32_t(bridge::kSourceTouchscreen)), std::string("TOUCHSCREEN"));
  CHECK_EQ(bridge::input_event_source_to_string(int32_t(bridge::kSourceMouse | bridge::kSourceStylus)),
           std::string("MOUSE | STYLUS"));
  CHECK_EQ(bridge::input_event_source_to_string(0), std::string("UNKNOWN"));
  CHECK_EQ(bridge::input_event_source_to_string(int32_t(bridge::kSourceAny)), std::string("ANY"));
  CHECK_EQ(bridge::input_event_source_to_string(0x40), std::string("0x00000040"));

  CHECK_EQ(event(bridge::kMotionActionDown, {coords(10, 20.5f)}).to_string(),
           std::string("MotionEvent { action=DOWN, id[0]=0, x[0]=10, y[0]=20.5, eventTime=1000ns, downTime=500ns, "
                       "deviceId=3, source=TOUCHSCREEN, displayId=0, eventId=0x2a}"));
  CHECK_EQ(event(bridge::kMotionActionMove, {coords(0, 0), coords(1, 2)}, bridge::kSourceTouchscreen, 0x91).to_string(),
           std::string("MotionEvent { action=MOVE, id[0]=0, id[1]=1, x[1]=1, y[1]=2, "
                       "flags=WINDOW_IS_OBSCURED | SUPPORTS_ORIENTATION | 0x10, pointerCount=2, eventTime=1000ns, "
                       "downTime=500ns, deviceId=3, source=TOUCHSCREEN, displayId=0, eventId=0x2a}"));
  std::vector<PointerCoords> many(17, coords(1, 1));
  CHECK_THROWS(event(0, many).to_string(), bridge::MotionEventError);  // LOG_ALWAYS_FATAL
}

// --- The natives ----------------------------------------------------------------

namespace {

const char* const kMotionEvent = "Landroid/view/MotionEvent;";

// gMotionEventMethods of android_view_MotionEvent.cpp (Android 17).
const char* const kNatives[][2] = {
    {"nativeInitialize",
     "(JIIIIIIIIIFFFFJJI[Landroid/view/MotionEvent$PointerProperties;[Landroid/view/MotionEvent$PointerCoords;)J"},
    {"nativeDispose", "(J)V"},
    {"nativeAddBatch", "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V"},
    {"nativeReadFromParcel", "(JLandroid/os/Parcel;)J"},
    {"nativeWriteToParcel", "(JLandroid/os/Parcel;)V"},
    {"nativeAxisToString", "(I)Ljava/lang/String;"},
    {"nativeAxisFromString", "(Ljava/lang/String;)I"},
    {"nativeGetPointerProperties", "(JILandroid/view/MotionEvent$PointerProperties;)V"},
    {"nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V"},
    {"nativeGetPointerId", "(JI)I"},
    {"nativeGetToolType", "(JI)I"},
    {"nativeGetEventTimeNanos", "(JI)J"},
    {"nativeGetRawAxisValue", "(JIII)F"},
    {"nativeGetAxisValue", "(JIII)F"},
    {"nativeTransform", "(JLandroid/graphics/Matrix;)V"},
    {"nativeApplyTransform", "(JLandroid/graphics/Matrix;)V"},
    {"nativeCopy", "(JJZ)J"},
    {"nativeSplit", "(JJI)J"},
    {"nativeGetId", "(J)I"},
    {"nativeGetDeviceId", "(J)I"},
    {"nativeGetSource", "(J)I"},
    {"nativeSetSource", "(JI)V"},
    {"nativeGetDisplayId", "(J)I"},
    {"nativeSetDisplayId", "(JI)V"},
    {"nativeGetAction", "(J)I"},
    {"nativeSetAction", "(JI)V"},
    {"nativeGetActionButton", "(J)I"},
    {"nativeSetActionButton", "(JI)V"},
    {"nativeIsTouchEvent", "(J)Z"},
    {"nativeGetFlags", "(J)I"},
    {"nativeSetFlags", "(JI)V"},
    {"nativeGetEdgeFlags", "(J)I"},
    {"nativeSetEdgeFlags", "(JI)V"},
    {"nativeGetMetaState", "(J)I"},
    {"nativeGetButtonState", "(J)I"},
    {"nativeSetButtonState", "(JI)V"},
    {"nativeGetClassification", "(J)I"},
    {"nativeOffsetLocation", "(JFF)V"},
    {"nativeGetRawXOffset", "(J)F"},
    {"nativeGetRawYOffset", "(J)F"},
    {"nativeGetXPrecision", "(J)F"},
    {"nativeGetYPrecision", "(J)F"},
    {"nativeGetXCursorPosition", "(J)F"},
    {"nativeGetYCursorPosition", "(J)F"},
    {"nativeSetCursorPosition", "(JFF)V"},
    {"nativeGetDownTimeNanos", "(J)J"},
    {"nativeSetDownTimeNanos", "(JJ)V"},
    {"nativeGetPointerCount", "(J)I"},
    {"nativeFindPointerIndex", "(JI)I"},
    {"nativeGetHistorySize", "(J)I"},
    {"nativeScale", "(JF)V"},
    {"nativeGetSurfaceRotation", "(J)I"},
};

const char* kFramework[] = {
    R"(
.class public final Landroid/view/MotionEvent$PointerCoords;
.super Ljava/lang/Object;
.field private mPackedAxisBits:J
.field private mPackedAxisValues:[F
.field public x:F
.field public y:F
.field public pressure:F
.field public size:F
.field public touchMajor:F
.field public touchMinor:F
.field public toolMajor:F
.field public toolMinor:F
.field public orientation:F
.field public relativeX:F
.field public relativeY:F
.field public isResampled:Z
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
)",
    R"(
.class public final Landroid/view/MotionEvent$PointerProperties;
.super Ljava/lang/Object;
.field public id:I
.field public toolType:I
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
)",
    R"(
.class public Landroid/graphics/Matrix;
.super Ljava/lang/Object;
.field public final native_instance:J
)",
    R"(
.class public final Landroid/os/Parcel;
.super Ljava/lang/Object;
.field private mNativePtr:J
)",
    R"(
.class public final Landroid/view/MotionEventProbe;
.super Ljava/lang/Object;
# What MotionEvent.obtain(downTime, eventTime, action, 1, properties, coords,
# 0, 0, 1f, 1f, 0, 0, SOURCE_TOUCHSCREEN, 0, 0, CLASSIFICATION_NONE) does: a
# finger (id 0) at (x, y), the times in nanoseconds, then nativeInitialize.
.method public static obtain(JJIFF)J
    .registers 36
    const/4 v0, 0x1
    const/4 v1, 0x0
    new-array v2, v0, [Landroid/view/MotionEvent$PointerProperties;
    new-instance v3, Landroid/view/MotionEvent$PointerProperties;
    invoke-direct {v3}, Landroid/view/MotionEvent$PointerProperties;-><init>()V
    iput v1, v3, Landroid/view/MotionEvent$PointerProperties;->id:I
    iput v0, v3, Landroid/view/MotionEvent$PointerProperties;->toolType:I
    aput-object v3, v2, v1
    new-array v4, v0, [Landroid/view/MotionEvent$PointerCoords;
    new-instance v3, Landroid/view/MotionEvent$PointerCoords;
    invoke-direct {v3}, Landroid/view/MotionEvent$PointerCoords;-><init>()V
    move/from16 v5, p5
    iput v5, v3, Landroid/view/MotionEvent$PointerCoords;->x:F
    move/from16 v5, p6
    iput v5, v3, Landroid/view/MotionEvent$PointerCoords;->y:F
    aput-object v3, v4, v1
    const-wide/16 v7, 0x0
    const/16 v9, 0x0
    const/16 v10, 0x1002
    const/16 v11, 0x0
    move/from16 v12, p4
    const/16 v13, 0x0
    const/16 v14, 0x0
    const/16 v15, 0x0
    const/16 v16, 0x0
    const/16 v17, 0x0
    const/16 v18, 0x0
    const/16 v19, 0x0
    const v20, 0x3f800000
    const v21, 0x3f800000
    const-wide/32 v0, 0xf4240
    mul-long v22, p0, v0
    mul-long v24, p2, v0
    const/16 v26, 0x1
    move-object/from16 v27, v2
    move-object/from16 v28, v4
    invoke-static/range {v7 .. v28}, Landroid/view/MotionEvent;->nativeInitialize(JIIIIIIIIIFFFFJJI[Landroid/view/MotionEvent$PointerProperties;[Landroid/view/MotionEvent$PointerCoords;)J
    move-result-wide v0
    return-wide v0
.end method
)",
};

// MotionEvent declaring every native of the table, as framework.jar does,
// and obtain() (without its pool of recycled events).
std::string motion_event_class() {
  std::string s = R"(
.class public Landroid/view/MotionEvent;
.super Ljava/lang/Object;
.field private mNativePtr:J
.method private constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method static obtain()Landroid/view/MotionEvent;
    .registers 1
    new-instance v0, Landroid/view/MotionEvent;
    invoke-direct {v0}, Landroid/view/MotionEvent;-><init>()V
    return-object v0
.end method
)";
  for (const auto& n : kNatives) s += std::string(".method static native ") + n[0] + n[1] + "\n.end method\n";
  return s;
}

uint32_t lo(int64_t v) { return uint32_t(uint64_t(v)); }
uint32_t hi(int64_t v) { return uint32_t(uint64_t(v) >> 32); }
uint32_t fbits(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  return b;
}
float bfloat(uint64_t b) {
  float v;
  uint32_t w = uint32_t(b);
  std::memcpy(&v, &w, 4);
  return v;
}

constexpr uint32_t kCurrent = 0x80000000u;  // HISTORY_CURRENT

struct MotionVm {
  std::unique_ptr<dex::DexFile> boot;
  bridge::NullHost host;
  std::unique_ptr<rt::Runtime> rt;
  std::unique_ptr<rt::Runtime::Scope> scope;
  rt::Class* me = nullptr;

  MotionVm() {
    std::vector<tool::ClassDef> b;
    b.push_back(tool::assemble_smali(motion_event_class(), "MotionEvent.smali"));
    for (const char* s : kFramework) b.push_back(tool::assemble_smali(s, "framework.smali"));
    boot = dex::DexFile::open(tool::write_dex(b), "framework-test.jar!classes.dex");
    rt = std::make_unique<rt::Runtime>();
    rt->add_boot_dex(boot.get());
    bridge::install(rt->registry(), host);
    scope = std::make_unique<rt::Runtime::Scope>(*rt, "test");
    me = rt->find_class(kMotionEvent);
  }
  ~MotionVm() {
    scope.reset();
    rt.reset();
  }

  // A native of another class, called through the registry.
  uint64_t native(const char* cls, const char* name, const char* sig, std::vector<uint32_t> words) {
    const rt::NativeBinding* b = rt->registry().binding(cls, name, sig);
    if (!b) throw test::Failure(std::string("native absente : ") + name);
    rt::NativeArgs a(*rt, words.data(), uint32_t(words.size()));
    return b->fn(*rt, a);
  }
  // A native of MotionEvent, called through its declaration.
  uint64_t call(const char* name, const char* sig, std::vector<uint32_t> args = {}) {
    rt::Method* m = me->declared_method(name, sig);
    if (!m) throw test::Failure(std::string("méthode absente : ") + name);
    return rt->invoke(m, args.data(), uint32_t(args.size()));
  }
  // "java.lang.X: message" of what f throws, "" if it returns.
  template <typename F>
  std::string thrown(F&& f) {
    try {
      f();
    } catch (const rt::JavaThrow& t) {
      std::string d = rt->describe_throwable(t.exception);
      return d.substr(0, d.find('\n'));
    }
    return "";
  }

  rt::Field* field(rt::Handle h, const char* name, const char* type) {
    rt::Field* f = bridge::find_field(rt->object(h)->klass, name, type);
    if (!f) throw test::Failure(std::string("champ absent : ") + name);
    return f;
  }
  void set(rt::Handle h, const char* name, const char* type, uint32_t v) {
    rt->object(h)->fields.at(field(h, name, type)->slot) = v;
  }
  void set_long(rt::Handle h, const char* name, int64_t v) {
    rt::Field* f = field(h, name, "J");
    rt->object(h)->fields.at(f->slot) = lo(v);
    rt->object(h)->fields.at(f->slot + 1) = hi(v);
  }
  uint32_t get(rt::Handle h, const char* name, const char* type) {
    return rt->object(h)->fields.at(field(h, name, type)->slot);
  }
  int64_t get_long(rt::Handle h, const char* name) {
    rt::Field* f = field(h, name, "J");
    return int64_t(uint64_t(rt->object(h)->fields.at(f->slot)) |
                   (uint64_t(rt->object(h)->fields.at(f->slot + 1)) << 32));
  }

  rt::Handle coords(float x, float y) {
    rt::Handle h = rt->alloc(rt->find_class("Landroid/view/MotionEvent$PointerCoords;"));
    set(h, "x", "F", fbits(x));
    set(h, "y", "F", fbits(y));
    return h;
  }
  rt::Handle properties(int32_t id, int32_t tool_type) {
    rt::Handle h = rt->alloc(rt->find_class("Landroid/view/MotionEvent$PointerProperties;"));
    set(h, "id", "I", uint32_t(id));
    set(h, "toolType", "I", uint32_t(tool_type));
    return h;
  }
  rt::Handle array(const char* descriptor, const std::vector<rt::Handle>& items) {
    rt::Handle a = rt->new_array(descriptor, int32_t(items.size()));
    for (size_t i = 0; i < items.size(); i++) std::memcpy(rt->array(a, "Object[]").data.data() + i * 4, &items[i], 4);
    return a;
  }
  rt::Handle coords_array(const std::vector<rt::Handle>& items) {
    return array("[Landroid/view/MotionEvent$PointerCoords;", items);
  }
  rt::Handle properties_array(const std::vector<rt::Handle>& items) {
    return array("[Landroid/view/MotionEvent$PointerProperties;", items);
  }

  // nativeInitialize(ptr, deviceId 0, source, displayId 0, action, flags, 0, 0,
  // 0, CLASSIFICATION_NONE, xOffset, yOffset, 1, 1, 1 ms, 2 ms, count, ...).
  int64_t initialize(int64_t ptr, int32_t action, int32_t flags, int32_t count, rt::Handle props, rt::Handle coords,
                     float x_offset = 0, float y_offset = 0) {
    int64_t down = 1000000, when = 2000000;
    return int64_t(call(kNatives[0][0], kNatives[0][1],
                        {lo(ptr),
                         hi(ptr),
                         0,
                         uint32_t(bridge::kSourceTouchscreen),
                         0,
                         uint32_t(action),
                         uint32_t(flags),
                         0,
                         0,
                         0,
                         0,
                         fbits(x_offset),
                         fbits(y_offset),
                         fbits(1),
                         fbits(1),
                         lo(down),
                         hi(down),
                         lo(when),
                         hi(when),
                         uint32_t(count),
                         props,
                         coords}));
  }
  int32_t get_i32(const char* name, int64_t h) { return int32_t(call(name, "(J)I", {lo(h), hi(h)})); }
  float axis(int64_t h, int32_t axis_id, int32_t pointer, uint32_t pos = kCurrent, bool raw = false) {
    return bfloat(call(raw ? "nativeGetRawAxisValue" : "nativeGetAxisValue", "(JIII)F",
                       {lo(h), hi(h), uint32_t(axis_id), uint32_t(pointer), pos}));
  }
};

}  // namespace

TEST(motion_event_natives_match_the_aosp_table) {
  MotionVm vm;
  for (const auto& n : kNatives) {
    if (!vm.rt->registry().binding(kMotionEvent, n[0], n[1]))
      throw test::Failure(std::string("native absente : ") + n[0] + n[1]);
  }
  CHECK_EQ(std::size(kNatives), size_t(52));
}

TEST(motion_event_natives_obtain_batch_and_read_back) {
  MotionVm vm;
  rt::Runtime& rt = *vm.rt;
  // MotionEvent.obtain's path, through the interpreter.
  rt::Method* obtain = rt.find_class("Landroid/view/MotionEventProbe;")->declared_method("obtain", "(JJIFF)J");
  std::vector<uint32_t> args = {1, 0, 2, 0, uint32_t(bridge::kMotionActionDown), fbits(10.5f), fbits(20.25f)};
  int64_t h = int64_t(rt.invoke(obtain, args.data(), uint32_t(args.size())));
  CHECK(h != 0);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0), 10.5f);
  CHECK_EQ(vm.axis(h, bridge::kAxisY, 0), 20.25f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 10.5f);
  CHECK_EQ(vm.get_i32("nativeGetAction", h), bridge::kMotionActionDown);
  CHECK_EQ(vm.get_i32("nativeGetPointerCount", h), 1);
  CHECK_EQ(vm.get_i32("nativeGetSource", h), int32_t(bridge::kSourceTouchscreen));
  CHECK_EQ(vm.get_i32("nativeGetHistorySize", h), 0);
  CHECK_EQ(int64_t(vm.call("nativeGetDownTimeNanos", "(J)J", {lo(h), hi(h)})), int64_t(1000000));
  CHECK_EQ(int64_t(vm.call("nativeGetEventTimeNanos", "(JI)J", {lo(h), hi(h), kCurrent})), int64_t(2000000));
  CHECK_EQ(int32_t(vm.call("nativeGetToolType", "(JI)I", {lo(h), hi(h), 0})), bridge::kToolTypeFinger);
  CHECK_EQ(bfloat(vm.call("nativeGetXPrecision", "(J)F", {lo(h), hi(h)})), 1.f);
  CHECK(std::isnan(bfloat(vm.call("nativeGetXCursorPosition", "(J)F", {lo(h), hi(h)}))));
  CHECK_EQ(uint32_t(vm.get_i32("nativeGetId", h)) >> 30, 3u);  // IdGenerator::Source::OTHER
  CHECK(vm.call("nativeIsTouchEvent", "(J)Z", {lo(h), hi(h)}) == 1u);

  // offsetLocation, then addBatch in window coordinates (with a packed axis).
  vm.call("nativeOffsetLocation", "(JFF)V", {lo(h), hi(h), fbits(5), fbits(-5)});
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0), 15.5f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 10.5f);
  CHECK_EQ(bfloat(vm.call("nativeGetRawXOffset", "(J)F", {lo(h), hi(h)})), 5.f);
  rt::GlobalRef next(rt, vm.coords(30, 40));
  rt::GlobalRef packed(rt, rt.new_array("[F", 8));
  float seven = 7;
  std::memcpy(rt.array(packed.get(), "float[]").data.data(), &seven, 4);
  vm.set_long(next.get(), "mPackedAxisBits", int64_t(PointerCoords::bit_of(bridge::kAxisGeneric1)));
  vm.set(next.get(), "mPackedAxisValues", "[F", packed.get());
  rt::GlobalRef batch(rt, vm.coords_array({next.get()}));
  int64_t t3 = 3000000;
  vm.call("nativeAddBatch", "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V",
          {lo(h), hi(h), lo(t3), hi(t3), batch.get(), 1});
  CHECK_EQ(vm.get_i32("nativeGetHistorySize", h), 1);
  CHECK_EQ(vm.get_i32("nativeGetMetaState", h), 1);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0), 30.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 25.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, 0), 15.5f);
  CHECK_EQ(vm.axis(h, bridge::kAxisGeneric1, 0), 7.f);
  CHECK_EQ(int64_t(vm.call("nativeGetEventTimeNanos", "(JI)J", {lo(h), hi(h), 0})), int64_t(2000000));

  // getPointerCoords: the named axes from the event, the others packed.
  rt::GlobalRef out(rt, vm.coords(0, 0));
  vm.call("nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V",
          {lo(h), hi(h), 0, kCurrent, out.get()});
  CHECK_EQ(bfloat(vm.get(out.get(), "x", "F")), 30.f);
  CHECK_EQ(bfloat(vm.get(out.get(), "y", "F")), 40.f);
  CHECK_EQ(vm.get_long(out.get(), "mPackedAxisBits"), int64_t(PointerCoords::bit_of(bridge::kAxisGeneric1)));
  rt::Handle values = vm.get(out.get(), "mPackedAxisValues", "[F");
  CHECK(values != rt::kNull);
  CHECK_EQ(rt.array(values, "float[]").length, 8u);
  float got;
  std::memcpy(&got, rt.array(values, "float[]").data.data(), 4);
  CHECK_EQ(got, 7.f);
  vm.call("nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V", {lo(h), hi(h), 0, 0, out.get()});
  CHECK_EQ(bfloat(vm.get(out.get(), "x", "F")), 15.5f);
  CHECK_EQ(vm.get_long(out.get(), "mPackedAxisBits"), int64_t(0));
  rt::GlobalRef props(rt, vm.properties(-1, 0));
  vm.call("nativeGetPointerProperties", "(JILandroid/view/MotionEvent$PointerProperties;)V",
          {lo(h), hi(h), 0, props.get()});
  CHECK_EQ(int32_t(vm.get(props.get(), "id", "I")), 0);
  CHECK_EQ(int32_t(vm.get(props.get(), "toolType", "I")), bridge::kToolTypeFinger);

  // The JNI's checks.
  std::string message = vm.thrown([&] { vm.axis(h, bridge::kAxisX, 1); });
  CHECK_EQ(
      message.substr(0, message.find(", eventId")),
      std::string("java.lang.IllegalArgumentException: invalid pointerIndex 1 for MotionEvent { action=DOWN, "
                  "id[0]=0, x[0]=30, y[0]=40, metaState=1, historySize=1, eventTime=3000000ns, downTime=1000000ns, "
                  "deviceId=0, source=TOUCHSCREEN, displayId=0"));
  CHECK(
      vm.thrown([&] {
          vm.axis(h, bridge::kAxisX, 0, 1);
        }).starts_with("java.lang.IllegalArgumentException: historyPos 1 out of range for MotionEvent { action=DOWN"));
  CHECK_EQ(vm.thrown([&] {
    vm.call("nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V", {lo(h), hi(h), 0, kCurrent, 0});
  }),
           std::string("java.lang.IllegalArgumentException: pointerCoords must not be null"));
  rt::GlobalRef with_null(rt, vm.coords_array({rt::kNull}));
  CHECK_EQ(vm.thrown([&] {
             vm.call("nativeAddBatch", "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V",
                     {lo(h), hi(h), lo(t3), hi(t3), with_null.get(), 0});
           }),
           std::string("java.lang.NullPointerException: pointerCoords"));

  // Labels.
  CHECK_EQ(rt.string_utf8(rt::Handle(vm.call("nativeAxisToString", "(I)Ljava/lang/String;", {8}))),
           std::string("ORIENTATION"));
  CHECK(vm.call("nativeAxisToString", "(I)Ljava/lang/String;", {54}) == rt::kNull);
  CHECK_EQ(int32_t(vm.call("nativeAxisFromString", "(Ljava/lang/String;)I", {rt.new_string_utf8("RELATIVE_X")})), 27);
  CHECK_EQ(int32_t(vm.call("nativeAxisFromString", "(Ljava/lang/String;)I", {rt.new_string_utf8("FOO")})), -1);

  vm.call("nativeDispose", "(J)V", {lo(h), hi(h)});
  CHECK_EQ(vm.thrown([&] { vm.get_i32("nativeGetAction", h); }),
           std::string("java.lang.IllegalStateException: [AnyAndroid] MotionEvent invalide"));
}

TEST(motion_event_natives_initialize_checks_and_flags) {
  MotionVm vm;
  rt::Runtime& rt = *vm.rt;
  rt::GlobalRef p0(rt, vm.properties(0, bridge::kToolTypeFinger));
  rt::GlobalRef p1(rt, vm.properties(1, bridge::kToolTypeFinger));
  rt::GlobalRef c0(rt, vm.coords(1, 2));
  rt::GlobalRef c1(rt, vm.coords(3, 4));
  rt::GlobalRef props(rt, vm.properties_array({p0.get(), p1.get()}));
  rt::GlobalRef coords(rt, vm.coords_array({c0.get(), c1.get()}));
  rt::GlobalRef one_prop(rt, vm.properties_array({p0.get()}));

  CHECK_EQ(vm.thrown([&] { vm.initialize(0, 0, 0, 0, props.get(), coords.get()); }),
           std::string("java.lang.IllegalArgumentException: pointerCount must be at least 1"));
  CHECK_EQ(vm.thrown([&] { vm.initialize(0, 0, 0, 2, rt::kNull, coords.get()); }),
           std::string("java.lang.IllegalArgumentException: pointerProperties array must not be null"));
  CHECK_EQ(
      vm.thrown([&] { vm.initialize(0, 0, 0, 2, one_prop.get(), coords.get()); }),
      std::string(
          "java.lang.IllegalArgumentException: pointerProperties array must be large enough to hold all pointers"));
  CHECK_EQ(vm.thrown([&] { vm.initialize(0, 0, 0, 2, props.get(), rt::kNull); }),
           std::string("java.lang.IllegalArgumentException: pointerCoords array must not be null"));

  // An offset: the coordinates given in the window, kept raw.
  int64_t h = vm.initialize(0, bridge::kMotionActionPointerDown | (1 << 8), 0, 2, props.get(), coords.get(), 100, 0);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 1), 3.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 1, kCurrent, true), -97.f);
  // Reinitialized in place: the same handle.
  CHECK_EQ(vm.initialize(h, bridge::kMotionActionPointerDown | (1 << 8), 0, 2, props.get(), coords.get()), h);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 1, kCurrent, true), 3.f);

  // An orientation sets the private flags, which Java never sees.
  vm.set(c1.get(), "orientation", "F", fbits(0.5f));
  int64_t oriented = vm.initialize(0, 0, 0x1, 2, props.get(), coords.get());
  CHECK_EQ(vm.get_i32("nativeGetFlags", oriented), 0x1);
  CHECK(near(vm.axis(oriented, bridge::kAxisOrientation, 1), 0.5f));
  vm.call("nativeSetFlags", "(JI)V", {lo(oriented), hi(oriented), 0x182});
  CHECK_EQ(vm.get_i32("nativeGetFlags", oriented), 0x2);
  CHECK(near(vm.axis(oriented, bridge::kAxisOrientation, 1), 0.5f));  // still supported
  vm.call("nativeSetCursorPosition", "(JFF)V", {lo(oriented), hi(oriented), fbits(7), fbits(8)});
  CHECK_EQ(bfloat(vm.call("nativeGetYCursorPosition", "(J)F", {lo(oriented), hi(oriented)})), 8.f);

  // split: the pointer going down alone is a DOWN, with a new id.
  int64_t split = int64_t(vm.call("nativeSplit", "(JJI)J", {0, 0, lo(h), hi(h), 0b10}));
  CHECK(split != 0 && split != h);
  CHECK_EQ(vm.get_i32("nativeGetAction", split), bridge::kMotionActionDown);
  CHECK_EQ(vm.get_i32("nativeGetPointerCount", split), 1);
  CHECK_EQ(int32_t(vm.call("nativeGetPointerId", "(JI)I", {lo(split), hi(split), 0})), 1);
  CHECK(vm.get_i32("nativeGetId", split) != vm.get_i32("nativeGetId", h));
  CHECK_EQ(int32_t(vm.call("nativeFindPointerIndex", "(JI)I", {lo(h), hi(h), 1})), 1);
  // libinput's fatal error: an action index beyond the pointers.
  vm.call("nativeSetAction", "(JI)V", {lo(h), hi(h), uint32_t(bridge::kMotionActionPointerDown | (5 << 8))});
  CHECK_EQ(vm.thrown([&] { vm.call("nativeSplit", "(JJI)J", {0, 0, lo(h), hi(h), 0b1}); }),
           std::string("java.lang.RuntimeException: [AnyAndroid] erreur fatale de libinput : Action index is out of "
                       "bounds, index: 5"));

  // copy: a new event, or the destination reused.
  int64_t copy = int64_t(vm.call("nativeCopy", "(JJZ)J", {0, 0, lo(h), hi(h), 0}));
  CHECK(copy != h);
  CHECK_EQ(vm.axis(copy, bridge::kAxisY, 1), 4.f);
  CHECK_EQ(int64_t(vm.call("nativeCopy", "(JJZ)J", {lo(copy), hi(copy), lo(oriented), hi(oriented), 1})), copy);
  CHECK_EQ(vm.get_i32("nativeGetFlags", copy), 0x2);

  // A null PointerProperties: 0 and the event given is freed; a null
  // PointerCoords: NullPointerException, the event freed too.
  rt::GlobalRef null_prop(rt, vm.properties_array({p0.get(), rt::kNull}));
  CHECK_EQ(vm.initialize(copy, 0, 0, 2, null_prop.get(), coords.get()), int64_t(0));
  CHECK_EQ(vm.thrown([&] { vm.get_i32("nativeGetAction", copy); }),
           std::string("java.lang.IllegalStateException: [AnyAndroid] MotionEvent invalide"));
  rt::GlobalRef null_coords(rt, vm.coords_array({c0.get(), rt::kNull}));
  CHECK_EQ(vm.thrown([&] { vm.initialize(split, 0, 0, 2, props.get(), null_coords.get()); }),
           std::string("java.lang.NullPointerException: pointerCoords"));
  CHECK(vm.thrown([&] { vm.get_i32("nativeGetAction", split); }) != "");
}

TEST(motion_event_natives_obtain_as_copy_for_input_receivers) {
  MotionVm vm;
  rt::Runtime& rt = *vm.rt;
  MotionEvent e = event(bridge::kMotionActionMove, {coords(1, 2), coords(3, 4)});
  std::vector<PointerCoords> next = {coords(5, 6), coords(7, 8)};
  e.add_sample(2000, next.data(), 0x2b);
  rt::GlobalRef obj(rt, bridge::obtain_motion_event(rt, e));
  auto native = bridge::motion_event_of(rt, obj.get());
  CHECK(native != nullptr);
  CHECK(*native == e);
  int64_t h = vm.get_long(obj.get(), "mNativePtr");
  CHECK_EQ(vm.axis(h, bridge::kAxisY, 1), 8.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisY, 1, 0), 4.f);
  CHECK(bridge::motion_event_of(rt, rt::kNull) == nullptr);
  vm.call("nativeDispose", "(J)V", {lo(h), hi(h)});
  CHECK(bridge::motion_event_of(rt, obj.get()) == nullptr);
}

TEST(motion_event_natives_matrix_and_parcel) {
  MotionVm vm;
  rt::Runtime& rt = *vm.rt;
  rt::Method* obtain = rt.find_class("Landroid/view/MotionEventProbe;")->declared_method("obtain", "(JJIFF)J");
  std::vector<uint32_t> args = {1, 0, 2, 0, uint32_t(bridge::kMotionActionMove), fbits(30), fbits(40)};
  int64_t h = int64_t(rt.invoke(obtain, args.data(), uint32_t(args.size())));

  // transform(Matrix): a rotation of 90 degrees before the window's transform.
  int64_t mh = int64_t(vm.native("Landroid/graphics/Matrix$ExtraNatives;", "nCreate", "(J)J", {0, 0}));
  bridge::skmatrix::assign(rt, mh, {0, -1, 500, 1, 0, 0, 0, 0, 1});
  rt::GlobalRef m(rt, rt.alloc(rt.find_class("Landroid/graphics/Matrix;")));
  vm.set_long(m.get(), "native_instance", mh);
  vm.call("nativeTransform", "(JLandroid/graphics/Matrix;)V", {lo(h), hi(h), m.get()});
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0), 460.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisY, 0), 30.f);
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 30.f);
  CHECK_EQ(vm.get_i32("nativeGetSurfaceRotation", h), 3);
  CHECK_EQ(vm.thrown([&] { vm.call("nativeTransform", "(JLandroid/graphics/Matrix;)V", {lo(h), hi(h), 0}); }),
           std::string("java.lang.NullPointerException: matrix"));
  // applyTransform: the raw coordinates themselves.
  bridge::skmatrix::assign(rt, mh, {2, 0, 0, 0, 2, 0, 0, 0, 1});
  vm.call("nativeApplyTransform", "(JLandroid/graphics/Matrix;)V", {lo(h), hi(h), m.get()});
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 60.f);
  vm.call("nativeScale", "(JF)V", {lo(h), hi(h), fbits(0.5f)});
  CHECK_EQ(vm.axis(h, bridge::kAxisX, 0, kCurrent, true), 30.f);

  // writeToParcel, then readFromParcel into a new event.
  auto parcel_native = [&](const char* name, const char* sig, std::vector<uint32_t> words) {
    return vm.native("Landroid/os/Parcel;", name, sig, std::move(words));
  };
  int64_t ptr = int64_t(parcel_native("nativeCreate", "()J", {}));
  rt::GlobalRef parcel(rt, rt.alloc(rt.find_class("Landroid/os/Parcel;")));
  vm.set_long(parcel.get(), "mNativePtr", ptr);
  vm.call("nativeWriteToParcel", "(JLandroid/os/Parcel;)V", {lo(h), hi(h), parcel.get()});
  CHECK_EQ(int32_t(parcel_native("nativeDataSize", "(J)I", {lo(ptr), hi(ptr)})), 196);
  parcel_native("nativeSetDataPosition", "(JI)V", {lo(ptr), hi(ptr), 0});
  int64_t back = int64_t(vm.call("nativeReadFromParcel", "(JLandroid/os/Parcel;)J", {0, 0, parcel.get()}));
  CHECK(back != 0 && back != h);
  CHECK_EQ(vm.axis(back, bridge::kAxisX, 0), vm.axis(h, bridge::kAxisX, 0));
  CHECK_EQ(vm.axis(back, bridge::kAxisY, 0), vm.axis(h, bridge::kAxisY, 0));
  CHECK_EQ(vm.get_i32("nativeGetId", back), vm.get_i32("nativeGetId", h));
  CHECK_EQ(vm.get_i32("nativeGetSurfaceRotation", back), 3);
  CHECK_EQ(int64_t(vm.call("nativeGetEventTimeNanos", "(JI)J", {lo(back), hi(back), kCurrent})), int64_t(2000000));
  // Into an existing event: the same handle.
  parcel_native("nativeSetDataPosition", "(JI)V", {lo(ptr), hi(ptr), 0});
  CHECK_EQ(int64_t(vm.call("nativeReadFromParcel", "(JLandroid/os/Parcel;)J", {lo(back), hi(back), parcel.get()})),
           back);
  // Nothing left to read: no pointer.
  CHECK_EQ(vm.thrown([&] { vm.call("nativeReadFromParcel", "(JLandroid/os/Parcel;)J", {0, 0, parcel.get()}); }),
           std::string("java.lang.RuntimeException: Failed to read MotionEvent parcel."));
}
