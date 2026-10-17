#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace aa::backend {

// The screen of the emulated device in a Win32 window (J8 of ADR-006): the
// frames that the real framework posts (bridge::PostedFrame, RGBA bytes
// premultiplied) shown scaled to the window, aspect ratio kept. The window
// lives on its own thread, with its own message loop; present() may be called
// from any thread.
class FrameWindow {
 public:
  // A window for a display of display_width x display_height pixels, sized to
  // fit the work area of the main monitor.
  FrameWindow(std::u16string title, int32_t display_width, int32_t display_height);
  ~FrameWindow();
  FrameWindow(const FrameWindow&) = delete;
  FrameWindow& operator=(const FrameWindow&) = delete;

  // False when the window could not be created (no desktop).
  bool ok() const { return hwnd_ != nullptr; }
  // Shows a frame of width x height pixels (rows packed); the last one stays.
  void present(int32_t width, int32_t height, const uint32_t* rgba);
  // Called on the window's thread when the user closes the window; without
  // it, the window is destroyed.
  void set_on_close(std::function<void()> fn);

  using TouchCallback = std::function<void(int32_t action, float x, float y, int64_t down_time, int64_t event_time)>;
  using KeyCallback = std::function<void(int32_t device_id, int32_t key_code, int32_t meta_state, bool down, int64_t time)>;

  void set_on_touch(TouchCallback fn);
  void set_on_key(KeyCallback fn);

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void run(std::u16string title);
  void paint();

  int32_t display_width_, display_height_;
  std::thread thread_;
  std::mutex lock_;
  std::condition_variable ready_cv_;
  bool ready_ = false;
  HWND hwnd_ = nullptr;
  std::function<void()> on_close_;
  TouchCallback on_touch_;
  KeyCallback on_key_;
  bool touching_ = false;
  float last_x_ = 0.0f, last_y_ = 0.0f;
  int64_t down_time_ = 0;
  // The frame shown, BGRA for GDI.
  int32_t frame_width_ = 0, frame_height_ = 0;
  std::vector<uint32_t> bgra_;
};

}  // namespace aa::backend
