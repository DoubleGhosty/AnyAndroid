#include "framework-bridge/motion_event.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

#include "common/log.h"

namespace aa::bridge {

const float kInvalidCursorPosition = std::numeric_limits<float>::quiet_NaN();

namespace {

// libutils' BitSet64: axis n is the bit 63 - n.
uint32_t index_of_bit(uint64_t bits, uint32_t n) { return uint32_t(std::popcount(bits & ~(~uint64_t(0) >> n))); }
bool has_bit(uint64_t bits, uint32_t n) { return (bits & PointerCoords::bit_of(n)) != 0; }

float float_of(int32_t bits) {
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

int32_t bits_of(float v) {
  int32_t bits;
  std::memcpy(&bits, &v, 4);
  return bits;
}

// roundTransformedCoords: transformed values rounded to about three decimals
// (a power of two), so that 0.99997 shows as 1.0.
float round_transformed_coords(float val) { return std::round(val * 1024.f) / 1024.f; }
Vec2 round_transformed_coords(Vec2 p) { return {round_transformed_coords(p.x), round_transformed_coords(p.y)}; }

bool should_disregard_transformation(uint32_t source) {
  // No transformation for the axes of joysticks, touchpads and relative mice.
  return is_from_source(source, kSourceClassJoystick) || is_from_source(source, kSourceClassPosition) ||
         is_from_source(source, kSourceMouseRelative);
}

bool should_disregard_offset(uint32_t source) {
  // Only pointer events refer to absolute display coordinates.
  return !is_from_source(source, kSourceClassPointer);
}

int32_t resolve_action_for_split_motion_event(int32_t action, uint32_t flags,
                                              const std::vector<PointerProperties>& pointer_properties,
                                              const std::vector<PointerProperties>& split_pointer_properties) {
  if (split_pointer_properties.empty()) throw MotionEventError("splitPointerProperties.empty()");
  const int32_t masked_action = MotionEvent::action_masked(action);
  if (masked_action != kMotionActionPointerDown && masked_action != kMotionActionPointerUp) {
    return action;  // unaffected by the split
  }
  const uint8_t action_index = MotionEvent::action_index(action);
  if (action_index >= pointer_properties.size()) {
    throw MotionEventError("Action index is out of bounds, index: " + std::to_string(action_index));
  }
  const int32_t affected_pointer_id = pointer_properties[action_index].id;
  std::optional<uint32_t> split_action_index;
  for (uint32_t i = 0; i < split_pointer_properties.size(); i++) {
    if (affected_pointer_id == split_pointer_properties[i].id) {
      split_action_index = i;
      break;
    }
  }
  if (!split_action_index) return kMotionActionMove;  // the affected pointer is not in the split event
  if (split_pointer_properties.size() > 1) {
    return masked_action | int32_t(*split_action_index << kMotionActionPointerIndexShift);
  }
  if (masked_action == kMotionActionPointerUp) {
    return (flags & kMotionFlagCanceled) ? kMotionActionCancel : kMotionActionUp;
  }
  return kMotionActionDown;
}

float transform_orientation(const UiTransform& transform, const PointerCoords& coords, uint32_t flags) {
  if (!(flags & kMotionFlagSupportsOrientation)) return 0;
  const bool is_directional = (flags & kMotionFlagSupportsDirectionalOrientation) != 0;
  return transform_angle(transform, coords.axis_value(kAxisOrientation), is_directional);
}

// The transformation of X/Y, without the check of should_disregard_transformation.
Vec2 calculate_transformed_xy_unchecked(uint32_t source, const UiTransform& transform, Vec2 xy) {
  return should_disregard_offset(source) ? transform_without_translation(transform, xy)
                                         : round_transformed_coords(transform.transform(xy));
}

// ftl::to_string of a uint32_t in hexadecimal ("0x" and lower-case digits).
std::string hex_string(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%" PRIx32, v);
  return buf;
}

// ftl::Flags<MotionFlag>::string(): the names of the set flags, lowest first,
// then the unnamed ones in hexadecimal; "0x0" when none.
std::string motion_flags_string(uint32_t flags) {
  static const std::map<uint32_t, const char*> kNames = {
      {0x1, "WINDOW_IS_OBSCURED"},
      {0x2, "WINDOW_IS_PARTIALLY_OBSCURED"},
      {0x4, "HOVER_EXIT_PENDING"},
      {0x8, "IS_GENERATED_GESTURE"},
      {0x20, "CANCELED"},
      {0x40, "NO_FOCUS_CHANGE"},
      {0x80, "SUPPORTS_ORIENTATION"},
      {0x100, "SUPPORTS_DIRECTIONAL_ORIENTATION"},
      {0x800, "IS_ACCESSIBILITY_EVENT"},
      {0x1000, "INJECTED_FROM_ACCESSIBILITY_TOOL"},
      {0x40000000, "TARGET_ACCESSIBILITY_FOCUS"},
      {0x80000000, "TAINTED"},
  };
  std::string result;
  bool first = true;
  uint32_t unstringified = 0;
  auto append = [&](std::string_view flag) {
    if (!first) result += " | ";
    first = false;
    result += flag;
  };
  for (uint32_t bit = 0; bit < 32; bit++) {
    uint32_t f = uint32_t(1) << bit;
    if (!(flags & f)) continue;
    auto it = kNames.find(f);
    if (it != kNames.end()) {
      append(it->second);
    } else {
      unstringified |= f;
    }
  }
  if (unstringified != 0) append(hex_string(unstringified));
  if (first) result += "0x0";
  return result;
}

// ftl::enum_string(ToolType).
std::string tool_type_string(int32_t tool_type) {
  static const char* const kNames[] = {"UNKNOWN", "FINGER", "STYLUS", "MOUSE", "ERASER", "PALM"};
  if (tool_type >= 0 && tool_type <= 5) return kNames[tool_type];
  return std::to_string(tool_type);
}

const char* classification_string(uint8_t classification) {
  switch (classification) {
    case 0: return "NONE";
    case 1: return "AMBIGUOUS_GESTURE";
    case 2: return "DEEP_PRESS";
    case 3: return "TWO_FINGER_SWIPE";
    case 4: return "MULTI_FINGER_SWIPE";
    case 5: return "PINCH";
    default: return "";  // no case in libinput's switch
  }
}

std::ostream& operator<<(std::ostream& out, const PointerProperties& properties) {
  return out << "Pointer(id=" << properties.id << ", " << tool_type_string(properties.tool_type) << ")";
}

// InputEventLabels' AXES_SEQUENCE: the label of axis n at index n.
constexpr std::string_view kAxisLabels[] = {
    "X", "Y", "PRESSURE", "SIZE", "TOUCH_MAJOR", "TOUCH_MINOR", "TOOL_MAJOR", "TOOL_MINOR", "ORIENTATION",
    "VSCROLL", "HSCROLL", "Z", "RX", "RY", "RZ", "HAT_X", "HAT_Y", "LTRIGGER", "RTRIGGER", "THROTTLE", "RUDDER",
    "WHEEL", "GAS", "BRAKE", "DISTANCE", "TILT", "SCROLL", "RELATIVE_X", "RELATIVE_Y", "RESERVED_29", "RESERVED_30",
    "RESERVED_31", "GENERIC_1", "GENERIC_2", "GENERIC_3", "GENERIC_4", "GENERIC_5", "GENERIC_6", "GENERIC_7",
    "GENERIC_8", "GENERIC_9", "GENERIC_10", "GENERIC_11", "GENERIC_12", "GENERIC_13", "GENERIC_14", "GENERIC_15",
    "GENERIC_16", "GESTURE_X_OFFSET", "GESTURE_Y_OFFSET", "GESTURE_SCROLL_X_DISTANCE", "GESTURE_SCROLL_Y_DISTANCE",
    "GESTURE_PINCH_SCALE_FACTOR", "GESTURE_SWIPE_FINGER_COUNT",
};
static_assert(std::size(kAxisLabels) == 54);

// ui::Transform's parcel layout: dsdx, dtdx, tx, dtdy, dsdy, ty.
bool read_transform(UiTransform& transform, const MotionEvent::ParcelReader& r) {
  std::array<std::optional<int32_t>, 6> v;
  bool ok = true;
  for (auto& x : v) {
    x = r.read_i32();
    ok = ok && x.has_value();
  }
  auto f = [&](size_t i) { return float_of(v[i].value_or(0)); };
  transform.set({f(0), f(1), f(2), f(3), f(4), f(5), 0, 0, 1});
  return ok;
}

void write_transform(const UiTransform& transform, const MotionEvent::ParcelWriter& w) {
  for (float v :
       {transform.dsdx(), transform.dtdx(), transform.tx(), transform.dtdy(), transform.dsdy(), transform.ty()}) {
    w.write_i32(bits_of(v));
  }
}

}  // namespace

int32_t next_input_event_id() {
  static std::atomic<uint32_t> counter{1};
  return int32_t((3u << 30) | (counter.fetch_add(1) & 0x3FFFFFFFu));
}

// --- ui::Transform ------------------------------------------------------------

void UiTransform::reset() {
  type_ = kIdentity;
  for (size_t i = 0; i < 3; i++) {
    for (size_t j = 0; j < 3; j++) m_[i][j] = i == j ? 1.0f : 0.0f;
  }
}

void UiTransform::set(float tx, float ty) {
  m_[2][0] = tx;
  m_[2][1] = ty;
  m_[2][2] = 1.0f;
  if (tx == 0 && ty == 0) {  // isZero (EPSILON is 0)
    type_ &= ~uint32_t(kTranslate);
  } else {
    type_ |= kTranslate;
  }
}

void UiTransform::set(const std::array<float, 9>& matrix) {
  m_[0][0] = matrix[0];
  m_[1][0] = matrix[1];
  m_[2][0] = matrix[2];
  m_[0][1] = matrix[3];
  m_[1][1] = matrix[4];
  m_[2][1] = matrix[5];
  m_[0][2] = matrix[6];
  m_[1][2] = matrix[7];
  m_[2][2] = matrix[8];
  type_ = kUnknownType;
  type();
}

Vec2 UiTransform::transform(float x, float y) const {
  return {m_[0][0] * x + m_[1][0] * y + m_[2][0], m_[0][1] * x + m_[1][1] * y + m_[2][1]};
}

UiTransform UiTransform::operator*(const UiTransform& rhs) const {
  if (type_ == kIdentity) return rhs;
  UiTransform r(*this);
  if (rhs.type_ == kIdentity) return r;
  const auto& a = m_;
  const auto& b = rhs.m_;
  auto& d = r.m_;
  for (size_t i = 0; i < 3; i++) {
    const float v0 = a[0][i];
    const float v1 = a[1][i];
    const float v2 = a[2][i];
    d[0][i] = v0 * b[0][0] + v1 * b[0][1] + v2 * b[0][2];
    d[1][i] = v0 * b[1][0] + v1 * b[1][1] + v2 * b[1][2];
    d[2][i] = v0 * b[2][0] + v1 * b[2][1] + v2 * b[2][2];
  }
  r.type_ |= rhs.type_;
  r.type_ &= 0xFF;
  r.type_ |= kUnknownType;
  return r;
}

uint32_t UiTransform::type() const {
  if (type_ & kUnknownType) {
    // Recomputes what this transform is (isZero and absIsOne with EPSILON 0).
    const float a = m_[0][0];
    const float b = m_[1][0];
    const float c = m_[0][1];
    const float d = m_[1][1];
    const float x = m_[2][0];
    const float y = m_[2][1];
    auto abs_is_one = [](float f) { return std::fabs(f) - 1.0f == 0; };
    bool scale = false;
    uint32_t flags = kRot0;
    if (b == 0 && c == 0) {
      if (a < 0) flags |= kFlipH;
      if (d < 0) flags |= kFlipV;
      if (!abs_is_one(a) || !abs_is_one(d)) scale = true;
    } else if (a == 0 && d == 0) {
      flags |= kRot90;
      if (b > 0) flags |= kFlipV;
      if (c < 0) flags |= kFlipH;
      if (!abs_is_one(b) || !abs_is_one(c)) scale = true;
    } else {
      flags = kRotInvalid;  // a skew component and/or a rotation that is not a multiple of 90 degrees
    }
    type_ = flags << 8;
    if (flags & kRotInvalid) {
      type_ |= kUnknown;
    } else {
      if ((flags & kRot90) || ((flags & kRot180) == kRot180)) type_ |= kRotate;
      if (flags & kFlipH) type_ ^= kScale;
      if (flags & kFlipV) type_ ^= kScale;
      if (scale) type_ |= kScale;
    }
    if (x != 0 || y != 0) type_ |= kTranslate;
  }
  return type_;
}

UiTransform UiTransform::inverse() const {
  // A 2x2 transformation followed by a translation, T*M: (T*M)^-1 = M^-1 * T^-1.
  UiTransform result;
  if (type_ <= kTranslate) {
    result = *this;
    result.m_[2][0] = -result.m_[2][0];
    result.m_[2][1] = -result.m_[2][1];
  } else {
    const float a = m_[0][0];
    const float b = m_[1][0];
    const float c = m_[0][1];
    const float d = m_[1][1];
    const float x = m_[2][0];
    const float y = m_[2][1];
    const float idet = 1.0f / det();
    result.m_[0][0] = d * idet;
    result.m_[0][1] = -c * idet;
    result.m_[1][0] = -b * idet;
    result.m_[1][1] = a * idet;
    result.type_ = type_;
    if (orientation() & kRot90) {
      // The inverse of ROT_90 is ROT_270 and vice versa.
      result.type_ |= kUnknownType;
    }
    Vec2 t = result.transform(-x, -y);
    result.m_[2][0] = t.x;
    result.m_[2][1] = t.y;
  }
  return result;
}

float transform_angle(const UiTransform& transform, float angle_radians, bool is_directional) {
  // A vector at the clockwise angle from vertical (down is +Y, right is +X).
  float x = std::sin(angle_radians);
  float y = -std::cos(angle_radians);
  Vec2 transformed = transform.transform(x, y);
  // The transformed origin, to transform orientation vectors.
  const Vec2 origin = transform.transform(0, 0);
  transformed.x -= origin.x;
  transformed.y -= origin.y;
  if (!is_directional && transformed.y > 0) {
    // Limits atan2f to [-pi/2, pi/2] by reversing the vector.
    transformed.x *= -1;
    transformed.y *= -1;
  }
  return std::atan2(transformed.x, -transformed.y);
}

Vec2 transform_without_translation(const UiTransform& transform, Vec2 xy) {
  const Vec2 transformed = transform.transform(xy);
  const Vec2 origin = transform.transform(0, 0);
  return round_transformed_coords(Vec2{transformed.x - origin.x, transformed.y - origin.y});
}

std::string input_event_source_to_string(int32_t source) {
  if (source == int32_t(kSourceUnknown)) return "UNKNOWN";
  if (source == int32_t(kSourceAny)) return "ANY";
  static const std::map<int32_t, const char*> kSources = {
      {kSourceKeyboard, "KEYBOARD"},
      {kSourceDpad, "DPAD"},
      {kSourceGamepad, "GAMEPAD"},
      {kSourceTouchscreen, "TOUCHSCREEN"},
      {kSourceMouse, "MOUSE"},
      {kSourceStylus, "STYLUS"},
      {kSourceBluetoothStylus, "BLUETOOTH_STYLUS"},
      {kSourceTrackball, "TRACKBALL"},
      {kSourceMouseRelative, "MOUSE_RELATIVE"},
      {kSourceTouchpad, "TOUCHPAD"},
      {kSourceTouchNavigation, "TOUCH_NAVIGATION"},
      {kSourceJoystick, "JOYSTICK"},
      {kSourceHdmi, "HDMI"},
      {kSourceSensor, "SENSOR"},
      {kSourceRotaryEncoder, "ROTARY_ENCODER"},
  };
  std::string result;
  for (const auto& [entry, name] : kSources) {
    if ((source & entry) == entry) {
      if (!result.empty()) result += " | ";
      result += name;
    }
  }
  if (result.empty()) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", uint32_t(source));
    result = buf;
  }
  return result;
}

bool is_from_source(uint32_t source, uint32_t test) { return (source & test) == test; }

// --- PointerCoords ------------------------------------------------------------

float PointerCoords::axis_value(int32_t axis) const {
  if (axis < 0 || axis > 63 || !has_bit(bits, uint32_t(axis))) return 0;
  return values[index_of_bit(bits, uint32_t(axis))];
}

bool PointerCoords::set_axis_value(int32_t axis, float value) {
  if (axis < 0 || axis > 63) return false;
  uint32_t index = index_of_bit(bits, uint32_t(axis));
  if (!has_bit(bits, uint32_t(axis))) {
    if (value == 0) return true;  // axes with value 0 need not be stored
    uint32_t count = uint32_t(std::popcount(bits));
    if (count >= kMaxAxes) {
      log::warn("input", "PointerCoords plein (" + std::to_string(kMaxAxes) + " axes au plus) : valeur de l'axe " +
                             std::to_string(axis) + " ignorée");
      return false;
    }
    bits |= bit_of(uint32_t(axis));
    for (uint32_t i = count; i > index; i--) values[i] = values[i - 1];
  }
  values[index] = value;
  return true;
}

void PointerCoords::scale(float global_scale, float window_x_scale, float window_y_scale) {
  // Pressure and size are normalized, orientation is not scaled. The global
  // scale is already in the window scale of X/Y.
  auto scale_axis = [this](int32_t axis, float factor) {
    float value = axis_value(axis);
    if (value != 0) set_axis_value(axis, value * factor);
  };
  scale_axis(kAxisX, window_x_scale);
  scale_axis(kAxisY, window_y_scale);
  scale_axis(kAxisTouchMajor, global_scale);
  scale_axis(kAxisTouchMinor, global_scale);
  scale_axis(kAxisToolMajor, global_scale);
  scale_axis(kAxisToolMinor, global_scale);
  scale_axis(kAxisRelativeX, window_x_scale);
  scale_axis(kAxisRelativeY, window_y_scale);
}

bool PointerCoords::operator==(const PointerCoords& other) const {
  if (bits != other.bits) return false;
  uint32_t count = uint32_t(std::popcount(bits));
  for (uint32_t i = 0; i < count; i++) {
    if (values[i] != other.values[i]) return false;
  }
  return is_resampled == other.is_resampled;
}

// --- MotionEvent --------------------------------------------------------------

void MotionEvent::initialize(int32_t id, int32_t device_id, uint32_t source, int32_t display_id, const Hmac& hmac,
                             int32_t action, int32_t action_button, uint32_t flags, int32_t edge_flags,
                             int32_t meta_state, int32_t button_state, uint8_t classification,
                             const UiTransform& transform, float x_precision, float y_precision,
                             float raw_x_cursor_position, float raw_y_cursor_position,
                             const UiTransform& raw_transform, int64_t down_time, int64_t event_time,
                             size_t pointer_count, const PointerProperties* pointer_properties,
                             const PointerCoords* pointer_coords) {
  id_ = id;
  device_id_ = device_id;
  source_ = source;
  display_id_ = display_id;
  hmac_ = hmac;
  action_ = action;
  action_button_ = action_button;
  flags_ = flags;
  edge_flags_ = edge_flags;
  meta_state_ = meta_state;
  button_state_ = button_state;
  classification_ = classification;
  transform_ = transform;
  x_precision_ = x_precision;
  y_precision_ = y_precision;
  raw_x_cursor_position_ = raw_x_cursor_position;
  raw_y_cursor_position_ = raw_y_cursor_position;
  raw_transform_ = raw_transform;
  down_time_ = down_time;
  pointer_properties_.assign(pointer_properties, pointer_properties + pointer_count);
  sample_event_times_.clear();
  sample_pointer_coords_.clear();
  add_sample(event_time, pointer_coords, id_);
}

void MotionEvent::copy_from(const MotionEvent& source_event, bool keep_history) {
  if (&source_event == this) {  // libinput would read the samples it just cleared
    MotionEvent copy = source_event;
    copy_from(copy, keep_history);
    return;
  }
  const MotionEvent& other = source_event;
  id_ = other.id_;
  device_id_ = other.device_id_;
  source_ = other.source_;
  display_id_ = other.display_id_;
  hmac_ = other.hmac_;
  action_ = other.action_;
  action_button_ = other.action_button_;
  flags_ = other.flags_;
  edge_flags_ = other.edge_flags_;
  meta_state_ = other.meta_state_;
  button_state_ = other.button_state_;
  classification_ = other.classification_;
  transform_ = other.transform_;
  x_precision_ = other.x_precision_;
  y_precision_ = other.y_precision_;
  raw_x_cursor_position_ = other.raw_x_cursor_position_;
  raw_y_cursor_position_ = other.raw_y_cursor_position_;
  raw_transform_ = other.raw_transform_;
  down_time_ = other.down_time_;
  pointer_properties_ = other.pointer_properties_;
  if (keep_history) {
    sample_event_times_ = other.sample_event_times_;
    sample_pointer_coords_ = other.sample_pointer_coords_;
  } else {
    sample_event_times_.assign(1, other.event_time());
    size_t pointer_count = other.pointer_count();
    auto begin = other.sample_pointer_coords_.begin() + std::ptrdiff_t(other.history_size() * pointer_count);
    sample_pointer_coords_.assign(begin, begin + std::ptrdiff_t(pointer_count));
  }
}

void MotionEvent::split_from(const MotionEvent& source_event, PointerIds split_pointer_ids, int32_t new_event_id) {
  if (&source_event == this) {  // libinput would read the samples it just replaced
    MotionEvent copy = source_event;
    split_from(copy, split_pointer_ids, new_event_id);
    return;
  }
  const MotionEvent& other = source_event;
  // libinput's TODO: the down time should come from the caller.
  const int64_t split_down_time = other.down_time_;
  std::string error;
  auto result = split(other.action(), other.flags(), other.history_size(), other.pointer_properties_,
                      other.sample_pointer_coords_, split_pointer_ids, &error);
  if (!result) {
    std::string ids;
    for (size_t i = split_pointer_ids.size(); i-- > 0;) ids += split_pointer_ids.test(i) ? '1' : '0';
    log::error("input", "découpage impossible de " + other.to_string() + " selon " + ids + " (nouvel id " +
                            std::to_string(new_event_id) + ") : " + error);
    // As libinput: the other event's data, as if splitting never occurred.
    initialize(new_event_id, other.device_id_, other.source_, other.display_id_, Hmac{}, other.action(),
               other.action_button_, other.flags_, other.edge_flags_, other.meta_state_, other.button_state_,
               other.classification_, other.transform_, other.x_precision_, other.y_precision_,
               other.raw_x_cursor_position_, other.raw_y_cursor_position_, other.raw_transform_, split_down_time,
               other.event_time(), 0, nullptr, nullptr);
    pointer_properties_ = other.pointer_properties_;
    sample_pointer_coords_ = other.sample_pointer_coords_;
    sample_event_times_ = other.sample_event_times_;
    return;
  }
  // Initialized with zero pointers, then the split pointers.
  initialize(new_event_id, other.device_id_, other.source_, other.display_id_, Hmac{}, result->action,
             other.action_button_, other.flags_, other.edge_flags_, other.meta_state_, other.button_state_,
             other.classification_, other.transform_, other.x_precision_, other.y_precision_,
             other.raw_x_cursor_position_, other.raw_y_cursor_position_, other.raw_transform_, split_down_time,
             other.event_time(), 0, nullptr, nullptr);
  pointer_properties_ = std::move(result->pointer_properties);
  sample_pointer_coords_ = std::move(result->pointer_coords);
  sample_event_times_ = other.sample_event_times_;
}

void MotionEvent::add_sample(int64_t event_time, const PointerCoords* pointer_coords, int32_t event_id) {
  id_ = event_id;
  sample_event_times_.push_back(event_time);
  if (pointer_count() > 0) {
    sample_pointer_coords_.insert(sample_pointer_coords_.end(), pointer_coords, pointer_coords + pointer_count());
  }
}

std::optional<int32_t> MotionEvent::surface_rotation() const {
  // The rotation from the window's space to the display's: the inverse of the
  // transform's, which takes display coordinates to the window.
  switch (transform_.orientation()) {
    case UiTransform::kRot0: return 0;    // ROTATION_0
    case UiTransform::kRot90: return 3;   // ROTATION_270
    case UiTransform::kRot180: return 2;  // ROTATION_180
    case UiTransform::kRot270: return 1;  // ROTATION_90
    default: return std::nullopt;
  }
}

float MotionEvent::x_cursor_position() const {
  return round_transformed_coords(transform_.transform(raw_x_cursor_position_, raw_y_cursor_position_).x);
}

float MotionEvent::y_cursor_position() const {
  return round_transformed_coords(transform_.transform(raw_x_cursor_position_, raw_y_cursor_position_).y);
}

void MotionEvent::set_cursor_position(float x, float y) {
  Vec2 v = transform_.inverse().transform(x, y);
  raw_x_cursor_position_ = v.x;
  raw_y_cursor_position_ = v.y;
}

int64_t MotionEvent::event_time() const { return historical_event_time(history_size()); }

int64_t MotionEvent::historical_event_time(size_t historical_index) const {
  if (historical_index >= sample_event_times_.size()) {
    throw MotionEventError("échantillon " + std::to_string(historical_index) + " absent de " + safe_dump());
  }
  return sample_event_times_[historical_index];
}

const PointerProperties& MotionEvent::pointer_properties(size_t pointer_index) const {
  if (pointer_index >= pointer_count()) {
    throw MotionEventError("getPointerProperties: Invalid pointer index " + std::to_string(pointer_index) +
                           " for " + safe_dump());
  }
  return pointer_properties_[pointer_index];
}

const PointerCoords& MotionEvent::raw_pointer_coords(size_t pointer_index) const {
  return historical_raw_pointer_coords(pointer_index, history_size());
}

const PointerCoords& MotionEvent::historical_raw_pointer_coords(size_t pointer_index, size_t historical_index) const {
  if (pointer_index >= pointer_count()) {
    throw MotionEventError("getHistoricalRawPointerCoords: Invalid pointer index " + std::to_string(pointer_index) +
                           " for " + safe_dump());
  }
  if (historical_index > history_size()) {
    throw MotionEventError("getHistoricalRawPointerCoords: Invalid historical index " +
                           std::to_string(historical_index) + " for " + safe_dump());
  }
  const size_t position = historical_index * pointer_count() + pointer_index;
  if (position >= sample_pointer_coords_.size()) {
    throw MotionEventError("getHistoricalRawPointerCoords: Invalid array index " + std::to_string(position) +
                           " for " + safe_dump());
  }
  return sample_pointer_coords_[position];
}

float MotionEvent::historical_raw_axis_value(int32_t axis, size_t pointer_index, size_t historical_index) const {
  const PointerCoords& coords = historical_raw_pointer_coords(pointer_index, historical_index);
  return calculate_transformed_axis_value(axis, source_, flags_, raw_transform_, coords);
}

float MotionEvent::historical_axis_value(int32_t axis, size_t pointer_index, size_t historical_index) const {
  const PointerCoords& coords = historical_raw_pointer_coords(pointer_index, historical_index);
  return calculate_transformed_axis_value(axis, source_, flags_, transform_, coords);
}

int64_t MotionEvent::find_pointer_index(int32_t pointer_id) const {
  for (size_t i = 0; i < pointer_properties_.size(); i++) {
    if (pointer_properties_[i].id == pointer_id) return int64_t(i);
  }
  return -1;
}

void MotionEvent::offset_location(float x_offset, float y_offset) {
  float curr_x_offset = transform_.tx();
  float curr_y_offset = transform_.ty();
  transform_.set(curr_x_offset + x_offset, curr_y_offset + y_offset);
}

float MotionEvent::raw_x_offset() const {
  // The x of the point the origin of the raw space maps to.
  return (transform_ * raw_transform_.inverse()).tx();
}

float MotionEvent::raw_y_offset() const { return (transform_ * raw_transform_.inverse()).ty(); }

void MotionEvent::scale(float global_scale_factor) {
  transform_.set(transform_.tx() * global_scale_factor, transform_.ty() * global_scale_factor);
  raw_transform_.set(raw_transform_.tx() * global_scale_factor, raw_transform_.ty() * global_scale_factor);
  x_precision_ *= global_scale_factor;
  y_precision_ *= global_scale_factor;
  for (PointerCoords& c : sample_pointer_coords_)
    c.scale(global_scale_factor, global_scale_factor, global_scale_factor);
}

void MotionEvent::transform(const std::array<float, 9>& matrix) {
  // The raw values of the samples are kept: only the transform changes.
  UiTransform new_transform;
  new_transform.set(matrix);
  transform_ = new_transform * transform_;
}

void MotionEvent::apply_transform(const std::array<float, 9>& matrix) {
  UiTransform t;
  t.set(matrix);
  for (PointerCoords& c : sample_pointer_coords_) calculate_transformed_coords_in_place(c, source_, flags_, t);
  // libinput skips the cursor when it differs from AMOTION_EVENT_INVALID_CURSOR_POSITION,
  // a NaN: never, so the cursor is always transformed (a NaN stays a NaN).
  const Vec2 cursor = t.transform(raw_x_cursor_position_, raw_y_cursor_position_);
  raw_x_cursor_position_ = cursor.x;
  raw_y_cursor_position_ = cursor.y;
}

bool MotionEvent::read_from_parcel(const ParcelReader& r) {
  // The value forms of libbinder's reads (readInt32()...) give 0 past the end.
  auto i32 = [&] { return r.read_i32().value_or(0); };
  auto f32 = [&] { return float_of(r.read_i32().value_or(0)); };
  auto i64 = [&] { return r.read_i64().value_or(0); };

  size_t pointer_count = size_t(int64_t(i32()));
  size_t sample_count = size_t(int64_t(i32()));
  if (pointer_count == 0 || pointer_count > kMaxPointers || sample_count == 0 || sample_count > kMaxSamples) {
    return false;
  }
  id_ = i32();
  device_id_ = i32();
  source_ = uint32_t(i32());
  display_id_ = i32();
  auto hmac = r.read_byte_vector();
  if (!hmac || hmac->size() != hmac_.size()) return false;
  std::copy(hmac->begin(), hmac->end(), hmac_.begin());
  action_ = i32();
  action_button_ = i32();
  flags_ = uint32_t(i32());
  edge_flags_ = i32();
  meta_state_ = i32();
  button_state_ = i32();
  classification_ = uint8_t(int8_t(i32()));  // readByte
  if (!read_transform(transform_, r)) return false;
  x_precision_ = f32();
  y_precision_ = f32();
  raw_x_cursor_position_ = f32();
  raw_y_cursor_position_ = f32();
  if (!read_transform(raw_transform_, r)) return false;
  down_time_ = i64();

  pointer_properties_.clear();
  pointer_properties_.reserve(pointer_count);
  sample_event_times_.clear();
  sample_event_times_.reserve(sample_count);
  sample_pointer_coords_.clear();
  sample_pointer_coords_.reserve(sample_count * pointer_count);
  for (size_t i = 0; i < pointer_count; i++) {
    PointerProperties& properties = pointer_properties_.emplace_back();
    properties.id = i32();
    properties.tool_type = i32();
  }
  while (sample_count > 0) {
    sample_count--;
    sample_event_times_.push_back(i64());
    for (size_t i = 0; i < pointer_count; i++) {
      // PointerCoords::readFromParcel.
      PointerCoords& coords = sample_pointer_coords_.emplace_back();
      coords.bits = uint64_t(i64());
      uint32_t count = uint32_t(std::popcount(coords.bits));
      if (count > PointerCoords::kMaxAxes) return false;
      for (uint32_t k = 0; k < count; k++) coords.values[k] = f32();
      coords.is_resampled = i32() != 0;  // readBool
    }
  }
  return true;
}

void MotionEvent::write_to_parcel(const ParcelWriter& w) const {
  size_t pointer_count = pointer_properties_.size();
  size_t sample_count = sample_event_times_.size();
  w.write_i32(int32_t(pointer_count));
  w.write_i32(int32_t(sample_count));
  w.write_i32(id_);
  w.write_i32(device_id_);
  w.write_i32(int32_t(source_));
  w.write_i32(display_id_);
  w.write_byte_vector(std::vector<uint8_t>(hmac_.begin(), hmac_.end()));
  w.write_i32(action_);
  w.write_i32(action_button_);
  w.write_i32(int32_t(flags_));
  w.write_i32(edge_flags_);
  w.write_i32(meta_state_);
  w.write_i32(button_state_);
  w.write_i32(int32_t(int8_t(classification_)));  // writeByte
  write_transform(transform_, w);
  w.write_i32(bits_of(x_precision_));
  w.write_i32(bits_of(y_precision_));
  w.write_i32(bits_of(raw_x_cursor_position_));
  w.write_i32(bits_of(raw_y_cursor_position_));
  write_transform(raw_transform_, w);
  w.write_i64(down_time_);
  for (const PointerProperties& properties : pointer_properties_) {
    w.write_i32(properties.id);
    w.write_i32(properties.tool_type);
  }
  const PointerCoords* pc = sample_pointer_coords_.data();
  for (size_t h = 0; h < sample_count; h++) {
    w.write_i64(sample_event_times_[h]);
    for (size_t i = 0; i < pointer_count; i++, pc++) {
      // PointerCoords::writeToParcel.
      w.write_i64(int64_t(pc->bits));
      uint32_t count = uint32_t(std::popcount(pc->bits));
      for (uint32_t k = 0; k < count; k++) w.write_i32(bits_of(pc->values[k]));
      w.write_i32(pc->is_resampled ? 1 : 0);  // writeBool
    }
  }
}

bool MotionEvent::is_touch_event(uint32_t source, int32_t action) {
  if (is_from_source(source, kSourceClassPointer)) {
    // Not HOVER_MOVE nor SCROLL.
    switch (action & kMotionActionMask) {
      case kMotionActionDown:
      case kMotionActionMove:
      case kMotionActionUp:
      case kMotionActionPointerDown:
      case kMotionActionPointerUp:
      case kMotionActionCancel:
      case kMotionActionOutside:
        return true;
      default:
        break;
    }
  }
  return false;
}

std::optional<std::string_view> MotionEvent::axis_label(int32_t axis) {
  if (size_t(uint32_t(axis)) < std::size(kAxisLabels)) return kAxisLabels[axis];
  return std::nullopt;
}

std::optional<int32_t> MotionEvent::axis_from_label(std::string_view label) {
  static const auto* by_label = [] {
    auto* m = new std::unordered_map<std::string_view, int32_t>;  // never destroyed
    for (size_t i = 0; i < std::size(kAxisLabels); i++) m->emplace(kAxisLabels[i], int32_t(i));
    return m;
  }();
  auto it = by_label->find(label);
  if (it == by_label->end()) return std::nullopt;
  return it->second;
}

std::string MotionEvent::action_to_string(int32_t action) {
  switch (action & kMotionActionMask) {
    case kMotionActionDown: return "DOWN";
    case kMotionActionUp: return "UP";
    case kMotionActionMove: return "MOVE";
    case kMotionActionCancel: return "CANCEL";
    case kMotionActionOutside: return "OUTSIDE";
    case kMotionActionPointerDown: return "POINTER_DOWN(" + std::to_string(action_index(action)) + ")";
    case kMotionActionPointerUp: return "POINTER_UP(" + std::to_string(action_index(action)) + ")";
    case kMotionActionHoverMove: return "HOVER_MOVE";
    case kMotionActionScroll: return "SCROLL";
    case kMotionActionHoverEnter: return "HOVER_ENTER";
    case kMotionActionHoverExit: return "HOVER_EXIT";
    case kMotionActionButtonPress: return "BUTTON_PRESS";
    case kMotionActionButtonRelease: return "BUTTON_RELEASE";
    default: return std::to_string(action);
  }
}

std::optional<MotionEvent::Split> MotionEvent::split(int32_t action, uint32_t flags, size_t history_size,
                                                     const std::vector<PointerProperties>& pointer_properties,
                                                     const std::vector<PointerCoords>& pointer_coords,
                                                     PointerIds split_pointer_ids, std::string* error) {
  if (!split_pointer_ids.any()) throw MotionEventError("!splitPointerIds.any()");
  const size_t pointer_count = pointer_properties.size();
  if (pointer_coords.size() != pointer_count * (history_size + 1)) {
    throw MotionEventError("pointerCoords.size() != (pointerCount * (historySize + 1))");
  }
  // std::bitset::test: a pointer id outside [0, MAX_POINTER_ID] aborts.
  auto selected = [&](int32_t id) {
    if (id < 0 || id > kMaxPointerId) {
      throw MotionEventError("bitset::test: __position (which is " + std::to_string(size_t(int64_t(id))) +
                             ") >= _Nb (which is " + std::to_string(kMaxPointerId + 1) + ")");
    }
    return split_pointer_ids.test(size_t(id));
  };
  Split out;
  for (size_t i = 0; i < pointer_count; i++) {
    if (selected(pointer_properties[i].id)) out.pointer_properties.push_back(pointer_properties[i]);
  }
  for (size_t i = 0; i < pointer_coords.size(); i++) {
    if (selected(pointer_properties[i % pointer_count].id)) out.pointer_coords.push_back(pointer_coords[i]);
  }
  if (out.pointer_properties.size() != split_pointer_ids.count()) {
    if (error) {
      *error = std::to_string(split_pointer_ids.count()) + " pointeur(s) demandé(s), l'événement n'en contient que " +
               std::to_string(out.pointer_properties.size());
    }
    return std::nullopt;
  }
  out.action = resolve_action_for_split_motion_event(action, flags, pointer_properties, out.pointer_properties);
  return out;
}

Vec2 MotionEvent::calculate_transformed_xy(uint32_t source, const UiTransform& transform, Vec2 xy) {
  if (should_disregard_transformation(source)) return xy;
  return calculate_transformed_xy_unchecked(source, transform, xy);
}

// Keep in sync with calculate_transformed_coords_in_place.
float MotionEvent::calculate_transformed_axis_value(int32_t axis, uint32_t source, uint32_t flags,
                                                    const UiTransform& transform, const PointerCoords& coords) {
  if (should_disregard_transformation(source)) return coords.axis_value(axis);
  if (axis == kAxisX || axis == kAxisY) {
    const Vec2 xy = calculate_transformed_xy_unchecked(source, transform, {coords.x(), coords.y()});
    return axis == kAxisX ? xy.x : xy.y;
  }
  if (axis == kAxisRelativeX || axis == kAxisRelativeY) {
    const Vec2 relative = transform_without_translation(
        transform, {coords.axis_value(kAxisRelativeX), coords.axis_value(kAxisRelativeY)});
    return axis == kAxisRelativeX ? relative.x : relative.y;
  }
  if (axis == kAxisOrientation) return transform_orientation(transform, coords, flags);
  return coords.axis_value(axis);
}

void MotionEvent::calculate_transformed_coords_in_place(PointerCoords& coords, uint32_t source, uint32_t flags,
                                                        const UiTransform& transform) {
  if (should_disregard_transformation(source)) return;
  const Vec2 xy = calculate_transformed_xy_unchecked(source, transform, {coords.x(), coords.y()});
  coords.set_axis_value(kAxisX, xy.x);
  coords.set_axis_value(kAxisY, xy.y);
  const Vec2 relative =
      transform_without_translation(transform, {coords.axis_value(kAxisRelativeX), coords.axis_value(kAxisRelativeY)});
  coords.set_axis_value(kAxisRelativeX, relative.x);
  coords.set_axis_value(kAxisRelativeY, relative.y);
  coords.set_axis_value(kAxisOrientation, transform_orientation(transform, coords, flags));
}

bool MotionEvent::operator==(const MotionEvent& o) const {
  // NaN cursor positions (invalid) compare equal.
  auto same = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
  return id_ == o.id_ && device_id_ == o.device_id_ && source_ == o.source_ && display_id_ == o.display_id_ &&
         hmac_ == o.hmac_ && action_ == o.action_ && action_button_ == o.action_button_ && flags_ == o.flags_ &&
         edge_flags_ == o.edge_flags_ && meta_state_ == o.meta_state_ && button_state_ == o.button_state_ &&
         classification_ == o.classification_ && transform_ == o.transform_ && x_precision_ == o.x_precision_ &&
         y_precision_ == o.y_precision_ && same(raw_x_cursor_position_, o.raw_x_cursor_position_) &&
         same(raw_y_cursor_position_, o.raw_y_cursor_position_) && raw_transform_ == o.raw_transform_ &&
         down_time_ == o.down_time_ && pointer_properties_ == o.pointer_properties_ &&
         sample_event_times_ == o.sample_event_times_ && sample_pointer_coords_ == o.sample_pointer_coords_;
}

std::string MotionEvent::to_string() const {
  std::ostringstream out;
  out << "MotionEvent { action=" << action_to_string(action());
  if (action_button() != 0) out << ", actionButton=" << std::to_string(action_button());
  const size_t count = pointer_count();
  if (count > kMaxPointers) {
    throw MotionEventError("Too many pointers : pointerCount = " + std::to_string(count));
  }
  for (size_t i = 0; i < count; i++) {
    out << ", id[" << i << "]=" << pointer_id(i);
    float px = x(i);
    float py = y(i);
    if (px != 0 || py != 0) {
      out << ", x[" << i << "]=" << px;
      out << ", y[" << i << "]=" << py;
    }
    int32_t tool = tool_type(i);
    if (tool != kToolTypeFinger) out << ", toolType[" << i << "]=" << tool_type_string(tool);
  }
  if (button_state() != 0) out << ", buttonState=" << button_state();
  if (classification() != 0) out << ", classification=" << classification_string(classification());
  if (meta_state() != 0) out << ", metaState=" << meta_state();
  if (flags() != 0) out << ", flags=" << motion_flags_string(flags());
  if (edge_flags() != 0) out << ", edgeFlags=" << edge_flags();
  if (count != 1) out << ", pointerCount=" << count;
  if (history_size() != 0) out << ", historySize=" << history_size();
  out << ", eventTime=" << event_time() << "ns";
  out << ", downTime=" << down_time() << "ns";
  out << ", deviceId=" << device_id();
  out << ", source=" << input_event_source_to_string(int32_t(source()));
  out << ", displayId=" << display_id();
  out << ", eventId=0x" << std::hex << id() << std::dec;
  out << "}";
  return out.str();
}

std::string MotionEvent::safe_dump() const {
  std::ostringstream out;
  // The m prefix tells safeDump's output from operator<<'s.
  out << "MotionEvent { mAction=" << action_to_string(action_);
  if (action_button_ != 0) out << ", mActionButton=" << action_button_;
  if (button_state_ != 0) out << ", mButtonState=" << button_state_;
  if (classification_ != 0) out << ", mClassification=" << classification_string(classification_);
  if (meta_state_ != 0) out << ", mMetaState=" << meta_state_;
  if (flags_ != 0) out << ", mFlags=" << motion_flags_string(flags_);
  if (edge_flags_ != 0) out << ", mEdgeFlags=" << edge_flags_;
  out << ", mDownTime=" << down_time_ << "ns";
  out << ", mDeviceId=" << device_id_;
  out << ", mSource=" << input_event_source_to_string(int32_t(source_));
  out << ", mDisplayId=" << display_id_;
  out << ", mEventId=0x" << std::hex << id_ << std::dec;
  // The number of items printed from the vectors is limited.
  out << ", mPointerProperties=(" << pointer_properties_.size() << ")[";
  for (size_t i = 0; i < pointer_properties_.size() && i < kMaxPointers; i++) {
    out << (i > 0 ? ", " : "") << pointer_properties_[i];
  }
  out << "], mSampleEventTimes=(" << sample_event_times_.size() << ")[";
  for (size_t i = 0; i < sample_event_times_.size() && i < 256; i++) {
    out << (i > 0 ? ", " : "") << sample_event_times_[i];
  }
  out << "], mSamplePointerCoords=(" << sample_pointer_coords_.size() << ")[";
  for (size_t i = 0; i < sample_pointer_coords_.size() && i < kMaxPointers; i++) {
    const PointerCoords& coords = sample_pointer_coords_[i];
    out << (i > 0 ? ", " : "") << "(" << coords.x() << ", " << coords.y() << ")";
  }
  out << "] }";
  return out.str();
}

}  // namespace aa::bridge
