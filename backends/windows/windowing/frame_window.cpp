#include "windows/windowing/frame_window.h"

#include <algorithm>

#include "common/clock.h"
#include "common/text.h"

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
  int aw = area.right - area.left, ah = area.bottom - area.top;
  if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return RECT{area.left, area.top, area.left, area.top};
  int dw = aw, dh = int(int64_t(aw) * h / w);
  if (dh > ah) {
    dh = ah;
    dw = int(int64_t(ah) * w / h);
  }
  int x = area.left + (aw - dw) / 2, y = area.top + (ah - dh) / 2;
  return RECT{x, y, x + dw, y + dh};
}

int32_t win32_vk_to_android_keycode(WPARAM vk) {
  switch (vk) {
    case VK_BACK: return 67;       // KEYCODE_DEL
    case VK_TAB: return 61;        // KEYCODE_TAB
    case VK_RETURN: return 66;     // KEYCODE_ENTER
    case VK_ESCAPE: return 4;      // KEYCODE_BACK
    case VK_SPACE: return 62;      // KEYCODE_SPACE
    case VK_PRIOR: return 92;      // KEYCODE_PAGE_UP
    case VK_NEXT: return 93;       // KEYCODE_PAGE_DOWN
    case VK_END: return 123;       // KEYCODE_MOVE_END
    case VK_HOME: return 122;      // KEYCODE_MOVE_HOME
    case VK_LEFT: return 21;       // KEYCODE_DPAD_LEFT
    case VK_UP: return 19;         // KEYCODE_DPAD_UP
    case VK_RIGHT: return 22;      // KEYCODE_DPAD_RIGHT
    case VK_DOWN: return 20;       // KEYCODE_DPAD_DOWN
    case VK_DELETE: return 112;    // KEYCODE_FORWARD_DEL
    case VK_SHIFT:
    case VK_LSHIFT: return 59;     // KEYCODE_SHIFT_LEFT
    case VK_RSHIFT: return 60;     // KEYCODE_SHIFT_RIGHT
    case VK_CONTROL:
    case VK_LCONTROL: return 113;  // KEYCODE_CTRL_LEFT
    case VK_RCONTROL: return 114;  // KEYCODE_CTRL_RIGHT
    case VK_MENU:
    case VK_LMENU: return 57;      // KEYCODE_ALT_LEFT
    case VK_RMENU: return 58;      // KEYCODE_ALT_RIGHT
    case VK_LWIN: return 117;      // KEYCODE_META_LEFT
    case VK_RWIN: return 118;      // KEYCODE_META_RIGHT
    case VK_CAPITAL: return 115;   // KEYCODE_CAPS_LOCK
    case VK_SCROLL: return 116;    // KEYCODE_SCROLL_LOCK
    case VK_NUMLOCK: return 143;   // KEYCODE_NUM_LOCK
    case VK_MULTIPLY: return 155;  // KEYCODE_NUMPAD_MULTIPLY
    case VK_ADD: return 157;       // KEYCODE_NUMPAD_ADD
    case VK_SUBTRACT: return 156;  // KEYCODE_NUMPAD_SUBTRACT
    case VK_DECIMAL: return 158;   // KEYCODE_NUMPAD_DOT
    case VK_DIVIDE: return 154;    // KEYCODE_NUMPAD_DIVIDE
    case VK_OEM_1: return 74;      // KEYCODE_SEMICOLON (;)
    case VK_OEM_PLUS: return 70;   // KEYCODE_EQUALS (=)
    case VK_OEM_COMMA: return 55;  // KEYCODE_COMMA (,)
    case VK_OEM_MINUS: return 69;  // KEYCODE_MINUS (-)
    case VK_OEM_PERIOD: return 56; // KEYCODE_PERIOD (.)
    case VK_OEM_2: return 76;      // KEYCODE_SLASH (/)
    case VK_OEM_3: return 68;      // KEYCODE_GRAVE (`)
    case VK_OEM_4: return 71;      // KEYCODE_LEFT_BRACKET ([)
    case VK_OEM_5: return 73;      // KEYCODE_BACKSLASH (\)
    case VK_OEM_6: return 72;      // KEYCODE_RIGHT_BRACKET (])
    case VK_OEM_7: return 75;      // KEYCODE_APOSTROPHE (')
    default:
      if (vk >= '0' && vk <= '9') return 7 + int32_t(vk - '0');
      if (vk >= 'A' && vk <= 'Z') return 29 + int32_t(vk - 'A');
      if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 144 + int32_t(vk - VK_NUMPAD0);
      return 0;
  }
}

int32_t get_win32_meta_state() {
  int32_t meta = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) meta |= 0x01;
  if (GetKeyState(VK_LSHIFT) & 0x8000) meta |= 0x40;
  if (GetKeyState(VK_RSHIFT) & 0x8000) meta |= 0x80;
  if (GetKeyState(VK_CONTROL) & 0x8000) meta |= 0x1000;
  if (GetKeyState(VK_LCONTROL) & 0x8000) meta |= 0x2000;
  if (GetKeyState(VK_RCONTROL) & 0x8000) meta |= 0x4000;
  if (GetKeyState(VK_MENU) & 0x8000) meta |= 0x02;
  if (GetKeyState(VK_LMENU) & 0x8000) meta |= 0x10;
  if (GetKeyState(VK_RMENU) & 0x8000) meta |= 0x20;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) meta |= 0x10000;
  if (GetKeyState(VK_CAPITAL) & 0x0001) meta |= 0x100000;
  if (GetKeyState(VK_NUMLOCK) & 0x0001) meta |= 0x200000;
  if (GetKeyState(VK_SCROLL) & 0x0001) meta |= 0x400000;
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

void FrameWindow::set_on_touch(TouchCallback fn) {
  std::lock_guard<std::mutex> guard(lock_);
  on_touch_ = std::move(fn);
}

void FrameWindow::set_on_key(KeyCallback fn) {
  std::lock_guard<std::mutex> guard(lock_);
  on_key_ = std::move(fn);
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
    case WM_LBUTTONDOWN: {
      SetCapture(hwnd_);
      touching_ = true;
      int x = static_cast<short>(LOWORD(lp));
      int y = static_cast<short>(HIWORD(lp));
      RECT client;
      GetClientRect(hwnd_, &client);
      TouchCallback on_touch;
      {
        std::lock_guard<std::mutex> guard(lock_);
        RECT image = fit(client, display_width_, display_height_);
        int iw = std::max(1, int(image.right - image.left));
        int ih = std::max(1, int(image.bottom - image.top));
        float dx = float(x - image.left) * float(display_width_) / float(iw);
        float dy = float(y - image.top) * float(display_height_) / float(ih);
        dx = std::clamp(dx, 0.0f, float(display_width_ - 1));
        dy = std::clamp(dy, 0.0f, float(display_height_ - 1));
        last_x_ = dx;
        last_y_ = dy;
        down_time_ = clock::monotonic_nanos();
        on_touch = on_touch_;
      }
      if (on_touch) on_touch(0, last_x_, last_y_, down_time_, down_time_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!touching_) return 0;
      int x = static_cast<short>(LOWORD(lp));
      int y = static_cast<short>(HIWORD(lp));
      RECT client;
      GetClientRect(hwnd_, &client);
      TouchCallback on_touch;
      int64_t now = clock::monotonic_nanos();
      {
        std::lock_guard<std::mutex> guard(lock_);
        RECT image = fit(client, display_width_, display_height_);
        int iw = std::max(1, int(image.right - image.left));
        int ih = std::max(1, int(image.bottom - image.top));
        float dx = float(x - image.left) * float(display_width_) / float(iw);
        float dy = float(y - image.top) * float(display_height_) / float(ih);
        dx = std::clamp(dx, 0.0f, float(display_width_ - 1));
        dy = std::clamp(dy, 0.0f, float(display_height_ - 1));
        last_x_ = dx;
        last_y_ = dy;
        on_touch = on_touch_;
      }
      if (on_touch) on_touch(2, last_x_, last_y_, down_time_, now);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!touching_) return 0;
      touching_ = false;
      ReleaseCapture();
      int x = static_cast<short>(LOWORD(lp));
      int y = static_cast<short>(HIWORD(lp));
      RECT client;
      GetClientRect(hwnd_, &client);
      TouchCallback on_touch;
      int64_t now = clock::monotonic_nanos();
      {
        std::lock_guard<std::mutex> guard(lock_);
        RECT image = fit(client, display_width_, display_height_);
        int iw = std::max(1, int(image.right - image.left));
        int ih = std::max(1, int(image.bottom - image.top));
        float dx = float(x - image.left) * float(display_width_) / float(iw);
        float dy = float(y - image.top) * float(display_height_) / float(ih);
        dx = std::clamp(dx, 0.0f, float(display_width_ - 1));
        dy = std::clamp(dy, 0.0f, float(display_height_ - 1));
        last_x_ = dx;
        last_y_ = dy;
        on_touch = on_touch_;
      }
      if (on_touch) on_touch(1, last_x_, last_y_, down_time_, now);
      return 0;
    }
    case WM_CAPTURECHANGED: {
      if (touching_ && reinterpret_cast<HWND>(lp) != hwnd_) {
        touching_ = false;
        int64_t now = clock::monotonic_nanos();
        TouchCallback on_touch;
        {
          std::lock_guard<std::mutex> guard(lock_);
          on_touch = on_touch_;
        }
        if (on_touch) on_touch(3, last_x_, last_y_, down_time_, now);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      int32_t key_code = win32_vk_to_android_keycode(wp);
      if (key_code != 0) {
        int32_t meta = get_win32_meta_state();
        int64_t now = clock::monotonic_nanos();
        KeyCallback on_key;
        {
          std::lock_guard<std::mutex> guard(lock_);
          on_key = on_key_;
        }
        if (on_key) on_key(0, key_code, meta, true, now);
      }
      return 0;
    }
    case WM_KEYUP: {
      int32_t key_code = win32_vk_to_android_keycode(wp);
      if (key_code != 0) {
        int32_t meta = get_win32_meta_state();
        int64_t now = clock::monotonic_nanos();
        KeyCallback on_key;
        {
          std::lock_guard<std::mutex> guard(lock_);
          on_key = on_key_;
        }
        if (on_key) on_key(0, key_code, meta, false, now);
      }
      return 0;
    }
    case WM_SYSKEYDOWN: {
      if (wp == VK_F4) break;
      int32_t key_code = win32_vk_to_android_keycode(wp);
      if (key_code != 0) {
        int32_t meta = get_win32_meta_state();
        int64_t now = clock::monotonic_nanos();
        KeyCallback on_key;
        {
          std::lock_guard<std::mutex> guard(lock_);
          on_key = on_key_;
        }
        if (on_key) on_key(0, key_code, meta, true, now);
      }
      break;
    }
    case WM_SYSKEYUP: {
      if (wp == VK_F4) break;
      int32_t key_code = win32_vk_to_android_keycode(wp);
      if (key_code != 0) {
        int32_t meta = get_win32_meta_state();
        int64_t now = clock::monotonic_nanos();
        KeyCallback on_key;
        {
          std::lock_guard<std::mutex> guard(lock_);
          on_key = on_key_;
        }
        if (on_key) on_key(0, key_code, meta, false, now);
      }
      break;
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
