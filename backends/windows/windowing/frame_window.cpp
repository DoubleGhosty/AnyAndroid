#include "windows/windowing/frame_window.h"

#include <windowsx.h>

#include <algorithm>

#include "common/text.h"
#include "windows/windowing/letterbox.h"

namespace aa::backend {

namespace {

constexpr UINT kFrameMsg = WM_APP + 1;   // a new frame: repaint
constexpr UINT kCloseMsg = WM_APP + 2;   // the owner goes away: destroy the window
constexpr wchar_t kClass[] = L"AnyAndroidScreen";

using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);

void enable_dpi_awareness() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  auto set_ctx = user32 ? reinterpret_cast<SetDpiContextFn>(
                              reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
                        : nullptr;
  if (set_ctx) {
    set_ctx(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  } else {
    SetProcessDPIAware();
  }
}

// The largest w x h rectangle of the frame's aspect ratio within `area`,
// centred.
RECT fit(const RECT& area, int32_t w, int32_t h) {
  Box b = fit_letterbox(Box{area.left, area.top, area.right, area.bottom}, w, h);
  return RECT{b.left, b.top, b.right, b.bottom};
}

Box box_of(const RECT& r) { return Box{r.left, r.top, r.right, r.bottom}; }

// Android key codes of the special keys (KeyEvent.KEYCODE_*); 0 for the others.
int32_t android_key_code(UINT vk) {
  switch (vk) {
    case VK_RETURN: return 66;   // ENTER
    case VK_BACK: return 67;     // DEL
    case VK_ESCAPE: return 4;    // BACK
    case VK_UP: return 19;       // DPAD_UP
    case VK_DOWN: return 20;     // DPAD_DOWN
    case VK_LEFT: return 21;     // DPAD_LEFT
    case VK_RIGHT: return 22;    // DPAD_RIGHT
    case VK_TAB: return 61;      // TAB
    case VK_DELETE: return 112;  // FORWARD_DEL
    case VK_HOME: return 122;    // MOVE_HOME
    case VK_END: return 123;     // MOVE_END
    case VK_PRIOR: return 92;    // PAGE_UP
    case VK_NEXT: return 93;     // PAGE_DOWN
    default: return 0;
  }
}

// KeyEvent.META_* of the modifiers held: Shift, Ctrl and Alt, left and right.
int32_t meta_state() {
  auto held = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };
  int32_t meta = 0;
  if (held(VK_LSHIFT)) meta |= 0x1 | 0x40;      // META_SHIFT_ON | META_SHIFT_LEFT_ON
  if (held(VK_RSHIFT)) meta |= 0x1 | 0x80;      // META_SHIFT_RIGHT_ON
  if (held(VK_LCONTROL)) meta |= 0x1000 | 0x2000;  // META_CTRL_ON | META_CTRL_LEFT_ON
  if (held(VK_RCONTROL)) meta |= 0x1000 | 0x4000;  // META_CTRL_RIGHT_ON
  if (held(VK_LMENU)) meta |= 0x2 | 0x10;       // META_ALT_ON | META_ALT_LEFT_ON
  if (held(VK_RMENU)) meta |= 0x2 | 0x20;       // META_ALT_RIGHT_ON
  return meta;
}

}  // namespace

FrameWindow::FrameWindow(std::u16string title, int32_t display_width, int32_t display_height)
    : display_width_(display_width), display_height_(display_height) {
  thread_ = std::thread([this, title = std::move(title)] { run(title); });
  std::unique_lock<std::mutex> guard(lock_);
  ready_cv_.wait(guard, [this] { return ready_; });
}

FrameWindow::~FrameWindow() {
  HWND hwnd;
  {
    std::lock_guard<std::mutex> guard(lock_);
    hwnd = hwnd_;
  }
  if (hwnd) PostMessageW(hwnd, kCloseMsg, 0, 0);
  if (thread_.joinable()) thread_.join();
}

void FrameWindow::set_on_close(std::function<void()> fn) {
  std::lock_guard<std::mutex> guard(lock_);
  on_close_ = std::move(fn);
}

void FrameWindow::set_input(FrameInput input) {
  std::lock_guard<std::mutex> guard(lock_);
  input_ = std::move(input);
}

FrameWindow::FrameInput FrameWindow::input() {
  std::lock_guard<std::mutex> guard(lock_);
  return input_;
}

RECT FrameWindow::image_rect(int32_t* width, int32_t* height) {
  RECT client;
  GetClientRect(hwnd_, &client);
  std::lock_guard<std::mutex> guard(lock_);
  *width = frame_width_ > 0 ? frame_width_ : display_width_;
  *height = frame_height_ > 0 ? frame_height_ : display_height_;
  return fit(client, *width, *height);
}

// The left button is a finger on the image (captured while it is down, so
// that a drag out of the window keeps the gesture); the wheel scrolls at the
// pointer. Presses in the bands around the image are ignored.
bool FrameWindow::handle_mouse(UINT msg, WPARAM wp, LPARAM lp) {
  FrameInput in = input();
  int32_t w = 0, h = 0;
  RECT image = image_rect(&w, &h);
  switch (msg) {
    case WM_LBUTTONDOWN: {
      auto p = window_to_image(box_of(image), w, h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (!p || !in.touch) return true;
      SetCapture(hwnd_);
      finger_down_ = true;
      finger_x_ = p->x;
      finger_y_ = p->y;
      in.touch(0, p->x, p->y);
      return true;
    }
    case WM_MOUSEMOVE: {
      if (!finger_down_ || !in.touch) return true;
      ImagePoint p = window_to_image_clamped(box_of(image), w, h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (p.x == finger_x_ && p.y == finger_y_) return true;
      finger_x_ = p.x;
      finger_y_ = p.y;
      in.touch(2, p.x, p.y);
      return true;
    }
    case WM_LBUTTONUP: {
      if (!finger_down_) return true;
      ImagePoint p = window_to_image_clamped(box_of(image), w, h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      finger_down_ = false;  // before ReleaseCapture's WM_CAPTURECHANGED
      ReleaseCapture();
      if (in.touch) in.touch(1, p.x, p.y);
      return true;
    }
    case WM_CAPTURECHANGED:
      // The capture lost while the button is down: the gesture is cancelled.
      if (finger_down_) {
        finger_down_ = false;
        if (in.touch) in.touch(3, finger_x_, finger_y_);
      }
      return true;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
      ScreenToClient(hwnd_, &pt);
      auto p = window_to_image(box_of(image), w, h, pt.x, pt.y);
      if (!p || !in.scroll) return true;
      float notches = float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
      if (msg == WM_MOUSEWHEEL) {
        in.scroll(p->x, p->y, 0.0f, notches);
      } else {
        in.scroll(p->x, p->y, notches, 0.0f);
      }
      return true;
    }
  }
  return false;
}

// The special keys as Android keys (auto-repeat counted), printable
// characters as text: WM_CHAR gives them as the keyboard layout types them.
bool FrameWindow::handle_key(UINT msg, WPARAM wp, LPARAM lp) {
  FrameInput in = input();
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      int32_t code = android_key_code(UINT(wp));
      if (code == 0 || !in.key) return false;
      bool repeated = (lp & (1 << 30)) != 0;  // the key was already down
      int32_t& repeat = keys_held_[UINT(wp)];
      repeat = repeated ? repeat + 1 : 0;
      in.key(code, true, meta_state(), repeat);
      return msg == WM_KEYDOWN;  // system keys keep their default handling (Alt+F4...)
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      int32_t code = android_key_code(UINT(wp));
      if (code == 0 || !in.key) return false;
      keys_held_.erase(UINT(wp));
      in.key(code, false, meta_state(), 0);
      return msg == WM_KEYUP;
    }
    case WM_KILLFOCUS:
      // Keys held when the window loses the keyboard: released.
      for (const auto& [vk, repeat] : keys_held_) {
        if (in.key) in.key(android_key_code(vk), false, 0, 0);
      }
      keys_held_.clear();
      return false;
    case WM_CHAR: {
      wchar_t c = wchar_t(wp);
      if (c >= 0xD800 && c <= 0xDBFF) {
        high_surrogate_ = c;
        return true;
      }
      std::u16string text;
      if (c >= 0xDC00 && c <= 0xDFFF) {
        if (high_surrogate_ != 0) text = {char16_t(high_surrogate_), char16_t(c)};
      } else if (c >= 0x20 && c != 0x7F) {  // control characters come as keys
        text = {char16_t(c)};
      }
      high_surrogate_ = 0;
      if (!text.empty() && in.text) in.text(std::move(text));
      return true;
    }
  }
  return false;
}

void FrameWindow::present(int32_t width, int32_t height, const uint32_t* rgba) {
  if (width <= 0 || height <= 0 || !rgba) return;
  size_t count = size_t(width) * size_t(height);
  std::vector<uint32_t> bgra(count);
  for (size_t i = 0; i < count; i++) {
    uint32_t p = rgba[i];  // bytes R, G, B, A: little-endian 0xAABBGGRR
    bgra[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
  }
  HWND hwnd;
  {
    std::lock_guard<std::mutex> guard(lock_);
    frame_width_ = width;
    frame_height_ = height;
    bgra_.swap(bgra);
    hwnd = hwnd_;
  }
  if (hwnd) PostMessageW(hwnd, kFrameMsg, 0, 0);
}

void FrameWindow::run(std::u16string title) {
  enable_dpi_awareness();
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // painted in WM_PAINT, without flicker
  wc.lpszClassName = kClass;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);

  // A phone-shaped window: the display scaled to 85 % of the work area's height.
  RECT work{0, 0, 1280, 800};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int work_h = work.bottom - work.top, work_w = work.right - work.left;
  int client_h = std::max(200, work_h * 85 / 100);
  int client_w = std::max(120, int(int64_t(client_h) * display_width_ / std::max(1, display_height_)));
  if (client_w > work_w) {
    client_w = work_w * 85 / 100;
    client_h = int(int64_t(client_w) * display_height_ / std::max(1, display_width_));
  }
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r{0, 0, client_w, client_h};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  int w = r.right - r.left, h = r.bottom - r.top;
  int x = work.left + (work_w - w) / 2, y = work.top + std::max(0, (work_h - h) / 2);
  HWND hwnd = CreateWindowExW(0, kClass, text::u16_to_wide(title).c_str(), style, x, y, w, h, nullptr, nullptr,
                              instance, this);
  {
    std::lock_guard<std::mutex> guard(lock_);
    hwnd_ = hwnd;
    ready_ = true;
  }
  ready_cv_.notify_all();
  if (!hwnd) return;
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  std::lock_guard<std::mutex> guard(lock_);
  hwnd_ = nullptr;
}

LRESULT CALLBACK FrameWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<FrameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->hwnd_ == nullptr) self->hwnd_ = hwnd;  // messages before CreateWindowEx returns
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT FrameWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (handle_mouse(msg, wp, lp) || handle_key(msg, wp, lp)) return 0;
  switch (msg) {
    case kFrameMsg:
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_PAINT:
      paint();
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_CLOSE: {
      std::function<void()> on_close;
      {
        std::lock_guard<std::mutex> guard(lock_);
        on_close = on_close_;
      }
      if (on_close) {
        on_close();
        return 0;
      }
      DestroyWindow(hwnd_);
      return 0;
    }
    case kCloseMsg:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FrameWindow::paint() {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd_, &ps);
  RECT client;
  GetClientRect(hwnd_, &client);
  std::lock_guard<std::mutex> guard(lock_);
  RECT image = fit(client, frame_width_ > 0 ? frame_width_ : display_width_,
                   frame_height_ > 0 ? frame_height_ : display_height_);
  // The bands around the image (letterbox), then the image over the rest.
  HBRUSH band = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  RECT bands[4] = {{client.left, client.top, client.right, image.top},
                   {client.left, image.bottom, client.right, client.bottom},
                   {client.left, image.top, image.left, image.bottom},
                   {image.right, image.top, client.right, image.bottom}};
  for (const RECT& b : bands) {
    if (b.left < b.right && b.top < b.bottom) FillRect(dc, &b, band);
  }
  if (bgra_.empty()) {
    FillRect(dc, &image, band);
  } else {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = frame_width_;
    bmi.bmiHeader.biHeight = -frame_height_;  // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, image.left, image.top, image.right - image.left, image.bottom - image.top, 0, 0, frame_width_,
                  frame_height_, bgra_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
  }
  EndPaint(hwnd_, &ps);
}

}  // namespace aa::backend
