#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
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
// from any thread. The mouse and the keyboard become the device's input
// (FrameInput), in display pixels and Android key codes.
class FrameWindow {
 public:
  // What the user does, called on the window's thread. Touch actions are
  // MotionEvent's (0 down, 1 up, 2 move, 3 cancel); scrolls are in notches
  // (+ right, + up); keys are KeyEvent codes with META_* states.
  struct FrameInput {
    std::function<void(int32_t action, float x, float y)> touch;
    std::function<void(float x, float y, float horizontal, float vertical)> scroll;
    std::function<void(int32_t key_code, bool down, int32_t meta_state, int32_t repeat)> key;
    std::function<void(std::u16string text)> text;
  };

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
  // The receivers of the user's input; none by default (input ignored). The
  // left button is a finger, the wheel scrolls, the special keys (Enter,
  // Backspace, Escape as BACK, arrows, Tab, Delete, Home, End, Page up/down)
  // are keys, printable characters (of any keyboard layout) are text.
  void set_input(FrameInput input);

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void run(std::u16string title);
  void paint();
  // The rectangle of the window where the image is drawn, and the image's size.
  RECT image_rect(int32_t* width, int32_t* height);
  bool handle_mouse(UINT msg, WPARAM wp, LPARAM lp);
  bool handle_key(UINT msg, WPARAM wp, LPARAM lp);
  FrameInput input();

  int32_t display_width_, display_height_;
  std::thread thread_;
  std::mutex lock_;
  std::condition_variable ready_cv_;
  bool ready_ = false;
  HWND hwnd_ = nullptr;
  std::function<void()> on_close_;
  // The frame shown, BGRA for GDI.
  int32_t frame_width_ = 0, frame_height_ = 0;
  std::vector<uint32_t> bgra_;
  // Input (window thread only, but the receivers): the finger held by the
  // left button and its last point, the keys held (virtual key -> repeats),
  // a high surrogate waiting for its pair.
  FrameInput input_;
  bool finger_down_ = false;
  float finger_x_ = 0, finger_y_ = 0;
  std::map<UINT, int32_t> keys_held_;
  wchar_t high_surrogate_ = 0;
};

}  // namespace aa::backend
