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
// A frame posted to a window's buffer queue (Surface.unlockCanvasAndPost):
// width x height pixels, RGBA bytes premultiplied (Skia's N32 as Android
// lays it out), rows packed. A copy, for the host to show or record.
struct PostedFrame {
  int32_t layer_id = 0;
  std::string layer_name;
  int64_t frame_number = 0;
  int32_t width = 0, height = 0;
  bool opaque = false;
  std::shared_ptr<const std::vector<uint32_t>> pixels;
};
// Called on the posting thread for every frame; none by default (the frames
// are then not copied).
void set_frame_listener(std::function<void(const PostedFrame&)> listener);
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

// Window input delivery (J8): touch and keyboard events dispatched to the
// active window's InputEventReceiver on its Looper.
void set_active_runtime(rt::Runtime* rt);
bool post_window_touch(int32_t action, float x, float y, int64_t down_time, int64_t event_time);
bool post_window_key(int32_t device_id, int32_t key_code, int32_t meta_state, bool down, int64_t time);

// Helpers shared with the lifecycle module.
ActivityPeer* activity_peer(rt::Runtime& rt, rt::Handle activity);
ViewPeer* view_peer(rt::Runtime& rt, rt::Handle view);
IntentPeer* intent_peer(rt::Runtime& rt, rt::Handle intent);
ui::Node* find_view_by_id(ui::Node* root, int32_t id);

}  // namespace aa::bridge
