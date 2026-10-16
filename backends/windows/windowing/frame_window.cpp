#include "windows/windowing/frame_window.h"

#include <algorithm>

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
