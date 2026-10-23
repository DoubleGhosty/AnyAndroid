#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dex-engine/runtime.h"
#include "framework-bridge/ui_model.h"

namespace aa::vfs {
class Vfs;
}
namespace aa::res {
class AssetManager;
}

// Native implementation of the Android framework subset supported by the
// prototype. Natives reach the outside world only through `Host`.
namespace aa::bridge {

class Host {
 public:
  virtual ~Host() = default;
  virtual ui::Backend* ui() = 0;  // nullptr in console mode
  virtual const std::string& package_name() const = 0;
  virtual void start_activity(rt::Handle caller, rt::Handle intent) = 0;
  virtual void finish_activity(rt::Handle activity) = 0;
  virtual void content_changed(rt::Handle activity) = 0;
  virtual void title_changed(rt::Handle activity) = 0;
  virtual void post(std::function<void()> task, uint32_t delay_ms) = 0;
  virtual void console(bool is_err, const std::string& utf8) = 0;  // System.out / System.err
  virtual void logcat(char level, const std::string& tag, const std::string& message) = 0;
  virtual rt::Handle application() = 0;
  // Resources of the application (its resources.arsc, device configuration
  // applied); nullptr when the APK has none.
  virtual const res::AssetManager* resources() { return nullptr; }
  // True on the thread that owns the views (Android's main/UI thread).
  virtual bool on_ui_thread() const { return true; }
};

// Host that refuses every interaction; used for static compatibility
// analysis, where natives are registered but never executed.
class NullHost final : public Host {
 public:
  ui::Backend* ui() override { return nullptr; }
  const std::string& package_name() const override { return package_; }
  void start_activity(rt::Handle, rt::Handle) override {}
  void finish_activity(rt::Handle) override {}
  void content_changed(rt::Handle) override {}
  void title_changed(rt::Handle) override {}
  void post(std::function<void()>, uint32_t) override {}
  void console(bool, const std::string&) override {}
  void logcat(char, const std::string&, const std::string&) override {}
  rt::Handle application() override { return rt::kNull; }

 private:
  std::string package_;
};

// --- Native state of framework objects ----------------------------------------

struct Extra {
  enum class Kind { kString, kInt, kLong, kBool, kFloat, kDouble } kind = Kind::kString;
  std::u16string s;
  int64_t i = 0;
  double d = 0;
};
using Extras = std::map<std::u16string, Extra>;

struct ActivityPeer final : rt::NativePeer {
  std::string class_name;  // descriptor
  std::u16string title;
  rt::Handle content_view = rt::kNull;
  rt::Handle intent = rt::kNull;
  bool finishing = false;
  bool super_called = false;
  void trace(std::vector<rt::Handle>& out) const override {
    out.push_back(content_view);
    out.push_back(intent);
  }
};

struct ViewPeer final : rt::NativePeer {
  std::unique_ptr<ui::Node> node;
  rt::Handle click_listener = rt::kNull;
  rt::Handle context = rt::kNull;
  rt::Handle tag = rt::kNull;
  rt::Handle layout_params = rt::kNull;
  void trace(std::vector<rt::Handle>& out) const override;
  void on_collect(const std::function<bool(rt::Handle)>& is_live) override;
};

struct IntentPeer final : rt::NativePeer {
  std::u16string action;
  std::string component;  // class descriptor of the target activity
  Extras extras;
  int32_t flags = 0;
};

struct BundlePeer final : rt::NativePeer {
  Extras values;
};

// Registers java.lang/java.util extras, the android.* subset and the native
// methods of AOSP's libcore (used when a boot class path is loaded).
void install(rt::NativeRegistry& registry, Host& host);

// Values the libcore natives report about the emulated VM (G2).
struct LibcoreConfig {
  std::string boot_class_path;          // "/apex/com.android.art/javalib/core-oj.jar:..."
  std::string class_path;               // application class path
  std::vector<std::string> properties;  // VMRuntime.properties(): "key=value" (-D options)
  std::shared_ptr<vfs::Vfs> vfs;        // file system of the process (ADR-005); /dev only if unset
};
LibcoreConfig& libcore_config();
void register_libcore_natives(rt::NativeRegistry& registry);
// Start-up sequence of ART for the boot class path: initialises the classes
// libcore expects to be ready first (java.lang.System before anything that
// reaches dalvik.system.VMRuntime). Call inside a Runtime::Scope.
void start_libcore(rt::Runtime& rt);
// The display, as the host's DisplayService reports it (1080 x 1920 pixels at
// 420 dpi).
constexpr int32_t kDisplayWidth = 1080, kDisplayHeight = 1920, kDisplayDensityDpi = 420;
// A frame of the display: the windows' layers composed as SurfaceFlinger
// does, presented when a buffer is posted (Surface.unlockCanvasAndPost) or a
// transaction changes what shows. width x height pixels, RGBA bytes
// premultiplied (Skia's N32 as Android lays it out), rows packed. A copy, for
// the host to show or record.
struct PostedFrame {
  int32_t layer_id = 0;
  std::string layer_name;
  int64_t frame_number = 0;
  int32_t width = 0, height = 0;
  bool opaque = false;
  std::shared_ptr<const std::vector<uint32_t>> pixels;
};
// Called on the presenting thread for every frame; none by default (the
// frames are then not composed).
void set_frame_listener(std::function<void(const PostedFrame&)> listener);

// --- Input from the host (J8 of ADR-006) --------------------------------------
//
// The host's events reach the application's windows as Android's
// InputDispatcher delivers them (input_dispatcher.cpp): the inject functions
// may be called from any host thread and return at once; a dispatcher thread
// routes each event to a window of the window manager's list (kept by the
// host's WindowService) and hands it to that window's InputEventReceiver on
// its Looper. Touches and scrolls go to the topmost touchable window under
// the point, keys and text to the focused window (the topmost focusable one).
// Coordinates are display pixels; each window gets them in its own space.
// Event times come from the common monotonic clock (common/clock.h), the one
// of SystemClock.uptimeMillis.

// MotionEvent.ACTION_* of a touch.
enum class TouchAction : int32_t { kDown = 0, kUp = 1, kMove = 2, kCancel = 3 };
// One finger of a touchscreen (SOURCE_TOUCHSCREEN, TOOL_TYPE_FINGER); the
// events of a gesture keep the time of its ACTION_DOWN.
void inject_touch(TouchAction action, float x, float y);
// A key of the virtual keyboard (device -1): KeyEvent.KEYCODE_*, META_* state,
// repeat count of an auto-repeated ACTION_DOWN.
void inject_key(int32_t key_code, bool down, int32_t meta_state = 0, int32_t repeat = 0);
// Typed text (UTF-16), as the key strokes of the virtual keyboard's key
// character map (Virtual.kcm); characters no key types arrive as a KeyEvent
// ACTION_MULTIPLE with KEYCODE_UNKNOWN and the characters, as Android does.
void inject_text(std::u16string text);
// A mouse wheel at (x, y): MotionEvent ACTION_SCROLL (SOURCE_MOUSE) with
// AXIS_HSCROLL (+ right) and AXIS_VSCROLL (+ up), in notches.
void inject_scroll(float x, float y, float horizontal, float vertical);

// An event as the dispatcher delivered it.
struct InputEventInfo {
  enum class Type { kTouch, kKey, kCharacters, kScroll } type = Type::kKey;
  int32_t action = 0;                                // MotionEvent or KeyEvent action
  float x = 0, y = 0;                                // touch, scroll: display pixels
  float horizontal = 0, vertical = 0;                // scroll
  int32_t key_code = 0, meta_state = 0, repeat = 0;  // key
  std::u16string characters;                         // ACTION_MULTIPLE
};
// The fate of an event: finished by its window (InputEventReceiver
// .finishInputEvent's `handled`), or dropped, never finished by a window
// (`reason` in French).
struct InputFinished {
  uint32_t seq = 0;  // the connection's sequence number; 0 when dropped before
  bool handled = false;
  bool dropped = false;
  std::string reason;
  std::string window;  // name of the target window, if any
  InputEventInfo event;
};
// Called once per injected event (focus changes are not reported), on the
// window's Looper thread or the dispatcher's; none by default.
void set_input_finished_listener(std::function<void(const InputFinished&)> listener);
// Waits until every injected event is finished or dropped; false on timeout.
bool wait_for_input_idle(int64_t timeout_ms);
// The event in a few French words ("appui 540,127", "appui sur la touche BACK").
std::string describe_input_event(const InputEventInfo& event);
// A PNG read by the decoder of ImageDecoder (framework_images.cpp): width x
// height pixels 0xAARRGGBB, unpremultiplied, rows packed. False when the data
// is not a PNG it reads. For the host: frames written as PNG read back.
struct DecodedPng {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> pixels;
};
bool decode_png_pixels(const uint8_t* data, size_t size, DecodedPng& out);
// Opaque sun.misc.Unsafe offset of an instance field slot (Field.getOffset()).
int64_t unsafe_field_offset(uint32_t slot);

// Helpers shared with the lifecycle module.
ActivityPeer* activity_peer(rt::Runtime& rt, rt::Handle activity);
ViewPeer* view_peer(rt::Runtime& rt, rt::Handle view);
IntentPeer* intent_peer(rt::Runtime& rt, rt::Handle intent);
ui::Node* find_view_by_id(ui::Node* root, int32_t id);

}  // namespace aa::bridge
