#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// libinput's MotionEvent (frameworks/native/libs/input/Input.cpp, Android 17)
// over ui::Transform (frameworks/native/libs/ui/Transform.cpp): pointer
// coordinates packed by axis, pointer properties, the samples of the history,
// the window and raw transforms and the transformation rules of each axis,
// splitting, the parcel layout and the axis labels of InputEventLabels.
// Independent of the Java runtime.
namespace aa::bridge {

// AMOTION_EVENT_AXIS_* (android/input.h), MotionEvent.AXIS_* in Java.
enum : int32_t {
  kAxisX = 0,
  kAxisY = 1,
  kAxisPressure = 2,
  kAxisSize = 3,
  kAxisTouchMajor = 4,
  kAxisTouchMinor = 5,
  kAxisToolMajor = 6,
  kAxisToolMinor = 7,
  kAxisOrientation = 8,
  kAxisVScroll = 9,
  kAxisHScroll = 10,
  kAxisRelativeX = 27,
  kAxisRelativeY = 28,
  kAxisGeneric1 = 32,
};

// AMOTION_EVENT_ACTION_*.
enum : int32_t {
  kMotionActionMask = 0xff,
  kMotionActionPointerIndexMask = 0xff00,
  kMotionActionPointerIndexShift = 8,
  kMotionActionDown = 0,
  kMotionActionUp = 1,
  kMotionActionMove = 2,
  kMotionActionCancel = 3,
  kMotionActionOutside = 4,
  kMotionActionPointerDown = 5,
  kMotionActionPointerUp = 6,
  kMotionActionHoverMove = 7,
  kMotionActionScroll = 8,
  kMotionActionHoverEnter = 9,
  kMotionActionHoverExit = 10,
  kMotionActionButtonPress = 11,
  kMotionActionButtonRelease = 12,
};

// AINPUT_SOURCE_* (InputDevice.SOURCE_* in Java).
enum : uint32_t {
  kSourceClassButton = 0x01,
  kSourceClassPointer = 0x02,
  kSourceClassNavigation = 0x04,
  kSourceClassPosition = 0x08,
  kSourceClassJoystick = 0x10,
  kSourceUnknown = 0,
  kSourceKeyboard = 0x100 | kSourceClassButton,
  kSourceDpad = 0x200 | kSourceClassButton,
  kSourceGamepad = 0x400 | kSourceClassButton,
  kSourceTouchscreen = 0x1000 | kSourceClassPointer,
  kSourceMouse = 0x2000 | kSourceClassPointer,
  kSourceStylus = 0x4000 | kSourceClassPointer,
  kSourceBluetoothStylus = 0x8000 | kSourceStylus,
  kSourceTrackball = 0x10000 | kSourceClassNavigation,
  kSourceMouseRelative = 0x20000 | kSourceClassNavigation,
  kSourceTouchpad = 0x100000 | kSourceClassPosition,
  kSourceTouchNavigation = 0x200000,
  kSourceRotaryEncoder = 0x400000,
  kSourceJoystick = 0x1000000 | kSourceClassJoystick,
  kSourceHdmi = 0x2000000 | kSourceClassButton,
  kSourceSensor = 0x4000000,
  kSourceAny = 0xffffff00,
};

// MotionFlag (ftl::Flags<MotionFlag> in libinput), MotionEvent.FLAG_* in Java.
enum : uint32_t {
  kMotionFlagWindowIsObscured = 0x1,
  kMotionFlagCanceled = 0x20,
  kMotionFlagSupportsOrientation = 0x80,
  kMotionFlagSupportsDirectionalOrientation = 0x100,
  // AMOTION_EVENT_PRIVATE_FLAG_MASK: the flags hidden from Java.
  kMotionPrivateFlagMask = kMotionFlagSupportsOrientation | kMotionFlagSupportsDirectionalOrientation,
};

// AMOTION_EVENT_TOOL_TYPE_* (ToolType).
enum : int32_t { kToolTypeUnknown = 0, kToolTypeFinger = 1, kToolTypeStylus = 2, kToolTypeMouse = 3 };

constexpr size_t kMaxPointers = 16;      // MAX_POINTERS
constexpr size_t kMaxSamples = 0xffff;   // MAX_SAMPLES
constexpr int32_t kMaxPointerId = 31;    // MAX_POINTER_ID
// AMOTION_EVENT_INVALID_CURSOR_POSITION (a quiet NaN).
extern const float kInvalidCursorPosition;

// libinput's LOG(FATAL) (the process aborts on Android): broken invariants,
// for example the action index of a POINTER_DOWN beyond the pointers.
class MotionEventError : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

// InputEvent::nextId: ids of the events made by this process (IdGenerator,
// source OTHER in the top two bits).
int32_t next_input_event_id();

struct Vec2 {
  float x = 0, y = 0;
};

// ui::Transform: a 3x3 matrix (columns of 3 floats, the translation in the
// third one) with the cached type of the 2x3 part, which the product and the
// inverse use as it is (stale or not), as in AOSP.
class UiTransform {
 public:
  enum RotationFlags : uint32_t {
    kRot0 = 0,
    kFlipH = 1,
    kFlipV = 2,
    kRot90 = 4,
    kRot180 = kFlipH | kFlipV,
    kRot270 = kRot180 | kRot90,
    kRotInvalid = 0x80
  };
  enum TypeMask : uint32_t { kIdentity = 0, kTranslate = 0x1, kRotate = 0x2, kScale = 0x4, kUnknown = 0x8 };

  UiTransform() { reset(); }

  uint32_t type_mask() const { return type() & 0xFF; }  // getType
  uint32_t orientation() const { return (type() >> 8) & 0xFF; }  // getOrientation
  bool operator==(const UiTransform& other) const { return m_ == other.m_; }

  float tx() const { return m_[2][0]; }
  float ty() const { return m_[2][1]; }
  float dsdx() const { return m_[0][0]; }
  float dtdx() const { return m_[1][0]; }
  float dtdy() const { return m_[0][1]; }
  float dsdy() const { return m_[1][1]; }
  float det() const { return m_[0][0] * m_[1][1] - m_[0][1] * m_[1][0]; }

  void reset();
  void set(float tx, float ty);
  // SkMatrix's row-major order (Matrix.getValues).
  void set(const std::array<float, 9>& matrix);
  Vec2 transform(float x, float y) const;
  Vec2 transform(Vec2 v) const { return transform(v.x, v.y); }
  UiTransform operator*(const UiTransform& rhs) const;
  UiTransform inverse() const;

 private:
  static constexpr uint32_t kUnknownType = 0x80000000;
  uint32_t type() const;

  std::array<std::array<float, 3>, 3> m_;  // m_[column][row]
  mutable uint32_t type_ = kIdentity;
};

// Transforms an angle of the x-y plane (0: up, clockwise positive), in
// [-pi, pi] when directional, else in [-pi/2, pi/2].
float transform_angle(const UiTransform& transform, float angle_radians, bool is_directional);
// The transform without its translation, rounded as transformed coordinates.
Vec2 transform_without_translation(const UiTransform& transform, Vec2 xy);
// inputEventSourceToString.
std::string input_event_source_to_string(int32_t source);
bool is_from_source(uint32_t source, uint32_t test);

// PointerCoords: the values of the present axes, packed by axis as libutils'
// BitSet64 orders them (axis n is bit 63 - n, also in parcels and in
// PointerCoords.mPackedAxisBits in Java).
struct PointerCoords {
  static constexpr uint32_t kMaxAxes = 30;  // MAX_AXES

  uint64_t bits = 0;
  std::array<float, kMaxAxes> values{};
  bool is_resampled = false;

  static uint64_t bit_of(uint32_t axis) { return 0x8000000000000000ULL >> axis; }

  void clear() {
    bits = 0;
    is_resampled = false;
  }
  bool is_empty() const { return bits == 0; }
  float axis_value(int32_t axis) const;
  // false when the axis is out of range (NAME_NOT_FOUND) or the structure is
  // full (NO_MEMORY); a missing axis set to 0 is not stored.
  bool set_axis_value(int32_t axis, float value);
  // A global scale and a window scale (X/Y and relative axes).
  void scale(float global_scale, float window_x_scale, float window_y_scale);
  float x() const { return axis_value(kAxisX); }
  float y() const { return axis_value(kAxisY); }
  bool operator==(const PointerCoords& other) const;
};

struct PointerProperties {
  int32_t id = -1;
  int32_t tool_type = kToolTypeUnknown;
  bool operator==(const PointerProperties&) const = default;
};

class MotionEvent {
 public:
  using Hmac = std::array<uint8_t, 32>;
  using PointerIds = std::bitset<kMaxPointerId + 1>;

  // --- InputEvent ---
  int32_t id() const { return id_; }
  int32_t device_id() const { return device_id_; }
  uint32_t source() const { return source_; }
  void set_source(uint32_t source) { source_ = source; }
  int32_t display_id() const { return display_id_; }
  void set_display_id(int32_t display_id) { display_id_ = display_id; }
  const Hmac& hmac() const { return hmac_; }

  int32_t action() const { return action_; }
  static int32_t action_masked(int32_t action) { return action & kMotionActionMask; }
  int32_t action_masked() const { return action_masked(action_); }
  static uint8_t action_index(int32_t action) {
    return uint8_t((action & kMotionActionPointerIndexMask) >> kMotionActionPointerIndexShift);
  }
  int32_t action_index() const { return action_index(action_); }
  void set_action(int32_t action) { action_ = action; }
  uint32_t flags() const { return flags_; }
  void set_flags(uint32_t flags) { flags_ = flags; }
  int32_t edge_flags() const { return edge_flags_; }
  void set_edge_flags(int32_t edge_flags) { edge_flags_ = edge_flags; }
  int32_t meta_state() const { return meta_state_; }
  void set_meta_state(int32_t meta_state) { meta_state_ = meta_state; }
  int32_t button_state() const { return button_state_; }
  void set_button_state(int32_t button_state) { button_state_ = button_state; }
  uint8_t classification() const { return classification_; }
  int32_t action_button() const { return action_button_; }
  void set_action_button(int32_t button) { action_button_ = button; }
  const UiTransform& transform() const { return transform_; }
  const UiTransform& raw_transform() const { return raw_transform_; }
  // ui::Rotation (0 to 3) of the window's surface, none for other angles.
  std::optional<int32_t> surface_rotation() const;
  float x_precision() const { return x_precision_; }
  float y_precision() const { return y_precision_; }
  float raw_x_cursor_position() const { return raw_x_cursor_position_; }
  float raw_y_cursor_position() const { return raw_y_cursor_position_; }
  float x_cursor_position() const;
  float y_cursor_position() const;
  void set_cursor_position(float x, float y);
  int64_t down_time() const { return down_time_; }
  void set_down_time(int64_t down_time) { down_time_ = down_time; }

  size_t pointer_count() const { return pointer_properties_.size(); }
  const PointerProperties& pointer_properties(size_t pointer_index) const;
  int32_t pointer_id(size_t pointer_index) const { return pointer_properties(pointer_index).id; }
  int32_t tool_type(size_t pointer_index) const { return pointer_properties(pointer_index).tool_type; }
  int64_t event_time() const;
  size_t history_size() const { return sample_event_times_.size() - 1; }
  int64_t historical_event_time(size_t historical_index) const;

  // The coordinates as the device reported them, no transform applied.
  const PointerCoords& raw_pointer_coords(size_t pointer_index) const;
  const PointerCoords& historical_raw_pointer_coords(size_t pointer_index, size_t historical_index) const;
  // Through the raw transform ("compat raw": the screen's rotation applied).
  float raw_axis_value(int32_t axis, size_t pointer_index) const {
    return historical_raw_axis_value(axis, pointer_index, history_size());
  }
  float historical_raw_axis_value(int32_t axis, size_t pointer_index, size_t historical_index) const;
  // Through the window's transform.
  float axis_value(int32_t axis, size_t pointer_index) const {
    return historical_axis_value(axis, pointer_index, history_size());
  }
  float historical_axis_value(int32_t axis, size_t pointer_index, size_t historical_index) const;
  float x(size_t pointer_index) const { return axis_value(kAxisX, pointer_index); }
  float y(size_t pointer_index) const { return axis_value(kAxisY, pointer_index); }
  bool is_resampled(size_t pointer_index, size_t historical_index) const {
    return historical_raw_pointer_coords(pointer_index, historical_index).is_resampled;
  }
  // -1 when no pointer has this id.
  int64_t find_pointer_index(int32_t pointer_id) const;

  void initialize(int32_t id, int32_t device_id, uint32_t source, int32_t display_id, const Hmac& hmac, int32_t action,
                  int32_t action_button, uint32_t flags, int32_t edge_flags, int32_t meta_state, int32_t button_state,
                  uint8_t classification, const UiTransform& transform, float x_precision, float y_precision,
                  float raw_x_cursor_position, float raw_y_cursor_position, const UiTransform& raw_transform,
                  int64_t down_time, int64_t event_time, size_t pointer_count,
                  const PointerProperties* pointer_properties, const PointerCoords* pointer_coords);
  void copy_from(const MotionEvent& other, bool keep_history);
  // Keeps the pointers of `other` whose ids are in split_pointer_ids.
  void split_from(const MotionEvent& other, PointerIds split_pointer_ids, int32_t new_event_id);
  void add_sample(int64_t event_time, const PointerCoords* pointer_coords, int32_t event_id);
  void offset_location(float x_offset, float y_offset);
  // The offset of the window's coordinates from the raw ones.
  float raw_x_offset() const;
  float raw_y_offset() const;
  void scale(float global_scale_factor);
  // Prepends a matrix (SkMatrix's order) to the window's transform.
  void transform(const std::array<float, 9>& matrix);
  // Applies a matrix to the coordinates of every sample (not to the transform).
  void apply_transform(const std::array<float, 9>& matrix);

  // Parcel layout of writeToParcel/readFromParcel, over libbinder's
  // primitives (a float travels as the bits of an int32).
  struct ParcelWriter {
    std::function<void(int32_t)> write_i32;
    std::function<void(int64_t)> write_i64;
    std::function<void(const std::vector<uint8_t>&)> write_byte_vector;
  };
  struct ParcelReader {
    // nullopt when the parcel has no more data (NOT_ENOUGH_DATA).
    std::function<std::optional<int32_t>()> read_i32;
    std::function<std::optional<int64_t>()> read_i64;
    // nullopt for a null or truncated vector.
    std::function<std::optional<std::vector<uint8_t>>()> read_byte_vector;
  };
  // false on malformed data (BAD_VALUE and the like); the fields read so far
  // stay replaced, as in libinput.
  bool read_from_parcel(const ParcelReader& r);
  void write_to_parcel(const ParcelWriter& w) const;

  static bool is_touch_event(uint32_t source, int32_t action);
  bool is_touch_event() const { return is_touch_event(source_, action_); }

  // InputEventLookup's axis labels ("X", "RELATIVE_Y", "GENERIC_1"...).
  static std::optional<std::string_view> axis_label(int32_t axis);
  static std::optional<int32_t> axis_from_label(std::string_view label);
  static std::string action_to_string(int32_t action);

  struct Split {
    int32_t action = 0;
    std::vector<PointerProperties> pointer_properties;
    std::vector<PointerCoords> pointer_coords;
  };
  // nullopt, with the reason in *error, when the event lacks some of the ids.
  static std::optional<Split> split(int32_t action, uint32_t flags, size_t history_size,
                                    const std::vector<PointerProperties>& pointer_properties,
                                    const std::vector<PointerCoords>& pointer_coords, PointerIds split_pointer_ids,
                                    std::string* error);

  // The transformation of each axis for a source: none for joysticks,
  // touchpads and relative mice, no translation for non-pointer sources.
  static Vec2 calculate_transformed_xy(uint32_t source, const UiTransform& transform, Vec2 xy);
  static float calculate_transformed_axis_value(int32_t axis, uint32_t source, uint32_t flags,
                                                const UiTransform& transform, const PointerCoords& coords);
  static void calculate_transformed_coords_in_place(PointerCoords& coords, uint32_t source, uint32_t flags,
                                                    const UiTransform& transform);

  bool operator==(const MotionEvent& o) const;
  // operator<< of libinput (the messages of the JNI's checks).
  std::string to_string() const;

  const std::vector<PointerProperties>& all_pointer_properties() const { return pointer_properties_; }
  const std::vector<int64_t>& sample_event_times() const { return sample_event_times_; }
  const std::vector<PointerCoords>& sample_pointer_coords() const { return sample_pointer_coords_; }

 private:
  // safeDump: no accessor, no invariant assumed.
  std::string safe_dump() const;

  int32_t id_ = 0;
  int32_t device_id_ = 0;
  uint32_t source_ = 0;
  int32_t display_id_ = -1;  // ui::LogicalDisplayId::INVALID
  Hmac hmac_{};
  int32_t action_ = 0;
  int32_t action_button_ = 0;
  uint32_t flags_ = 0;
  int32_t edge_flags_ = 0;
  int32_t meta_state_ = 0;
  int32_t button_state_ = 0;
  uint8_t classification_ = 0;
  UiTransform transform_;
  float x_precision_ = 0;
  float y_precision_ = 0;
  float raw_x_cursor_position_ = 0;
  float raw_y_cursor_position_ = 0;
  UiTransform raw_transform_;
  int64_t down_time_ = 0;
  std::vector<PointerProperties> pointer_properties_;
  std::vector<int64_t> sample_event_times_;
  std::vector<PointerCoords> sample_pointer_coords_;
};

}  // namespace aa::bridge
