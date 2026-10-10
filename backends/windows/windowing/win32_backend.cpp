#include "windows/windowing/win32_backend.h"

#include <algorithm>
#include <vector>

#include "common/text.h"

namespace aa::backend {

namespace {

constexpr UINT kWakeMsg = WM_APP + 1;
constexpr UINT kQuitMsg = WM_APP + 2;
constexpr int kBackId = 1;
constexpr UINT_PTR kToastTimer = 1;
constexpr UINT kCueBanner = 0x1501;  // EM_SETCUEBANNER

using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

template <typename Fn>
Fn user32_proc(const char* name) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  return user32 ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user32, name))) : nullptr;
}

void enable_dpi_awareness() {
  static bool done = false;
  if (done) return;
  done = true;
  if (auto set_ctx = user32_proc<SetDpiContextFn>("SetProcessDpiAwarenessContext")) {
    set_ctx(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  } else {
    SetProcessDPIAware();
  }
}

UINT dpi_for(HWND hwnd) {
  if (hwnd) {
    if (auto get = user32_proc<GetDpiForWindowFn>("GetDpiForWindow")) return get(hwnd);
  }
  HDC dc = GetDC(nullptr);
  int d = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return UINT(d);
}

std::wstring wide(const std::u16string& s) { return text::u16_to_wide(s); }

COLORREF colorref(uint32_t argb) { return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF); }

std::wstring face_name() {
  NONCLIENTMETRICSW m{};
  m.cbSize = sizeof m;
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0)) return m.lfMessageFont.lfFaceName;
  return L"Segoe UI";
}

bool effectively_visible(const ui::Node* n) { return n->visible && !n->gone; }

}  // namespace

Win32Backend::Win32Backend() { enable_dpi_awareness(); }

Win32Backend::~Win32Backend() {
  if (hwnd_) DestroyWindow(hwnd_);
  clear_fonts();
  for (auto& [c, b] : brushes_) DeleteObject(b);
}

void Win32Backend::clear_fonts() {
  for (auto& [h, f] : fonts_) DeleteObject(f);
  fonts_.clear();
}

HFONT Win32Backend::font(float sp) {
  int height = -std::max(1, MulDiv(int(sp * 100), int(dpi_), 9600));
  auto it = fonts_.find(height);
  if (it != fonts_.end()) return it->second;
  static const std::wstring face = face_name();
  HFONT f = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
  fonts_[height] = f;
  return f;
}

void Win32Backend::ensure_window() {
  if (hwnd_) return;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AnyAndroidWindow";
    RegisterClassExW(&wc);
    WNDCLASSEXW tc{};
    tc.cbSize = sizeof tc;
    tc.lpfnWndProc = ToastProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    tc.lpszClassName = L"AnyAndroidToast";
    RegisterClassExW(&tc);
    registered = true;
  }
  UINT sys_dpi = dpi_for(nullptr);
  CreateWindowExW(WS_EX_CONTROLPARENT, L"AnyAndroidWindow", wide(title_).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                  CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(400, int(sys_dpi), 96), MulDiv(720, int(sys_dpi), 96), nullptr,
                  nullptr, inst, this);
  if (!hwnd_) return;
  dpi_ = dpi_for(hwnd_);
  back_button_ = CreateWindowExW(0, L"BUTTON", L"← Retour", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackId)), inst, nullptr);
  SendMessageW(back_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font(12)), TRUE);
  toast_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"AnyAndroidToast", L"", WS_POPUP, 0, 0, 10,
                           10, hwnd_, nullptr, inst, this);
  if (toast_) SetLayeredWindowAttributes(toast_, 0, 235, LWA_ALPHA);
  rebuild();
}

LRESULT CALLBACK Win32Backend::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Win32Backend* self = nullptr;
  if (msg == WM_NCCREATE) {
    self = static_cast<Win32Backend*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<Win32Backend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self && self->hwnd_ == hwnd) return self->handle(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Win32Backend::ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
  }
  auto* self = reinterpret_cast<Win32Backend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    HBRUSH bg = CreateSolidBrush(RGB(50, 50, 50));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SelectObject(dc, self->font(14));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    std::wstring t = wide(self->toast_text_);
    InflateRect(&r, -self->px(12), -self->px(8));
    DrawTextW(dc, t.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Win32Backend::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize = {px(280), px(240)};
      return 0;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      auto* r = reinterpret_cast<RECT*>(lp);
      clear_fonts();
      SendMessageW(back_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font(12)), TRUE);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      rebuild();
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      int code = HIWORD(wp);
      if ((id == kBackId && code == BN_CLICKED) || id == IDCANCEL) {
        if (sink_) sink_->on_back();
        return 0;
      }
      auto it = controls_.find(id);
      if (it == controls_.end() || !sink_) return 0;
      ui::Node* node = it->second->node;  // the tree may be rebuilt by the callback
      switch (node->kind) {
        case ui::NodeKind::kButton:
          if (code == BN_CLICKED) sink_->on_click(node);
          break;
        case ui::NodeKind::kText:
          if (code == STN_CLICKED && node->clickable) sink_->on_click(node);
          break;
        case ui::NodeKind::kEdit:
          if (code == EN_CHANGE && !updating_) {
            HWND h = it->second->hwnd;
            int n = GetWindowTextLengthW(h);
            std::wstring buf(size_t(n) + 1, L'\0');
            GetWindowTextW(h, buf.data(), n + 1);
            buf.resize(size_t(n));
            sink_->on_text_changed(node, text::wide_to_u16(buf));
          }
          break;
        default:
          break;
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
      auto it = controls_.find(id);
      uint32_t bg = 0;
      if (it != controls_.end()) {
        ui::Node* n = it->second->node;
        SetTextColor(dc, n->text_color ? colorref(n->text_color) : GetSysColor(n->enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
        bg = n->background;
      }
      SetBkMode(dc, TRANSPARENT);
      if (!bg) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
      auto b = brushes_.find(bg);
      if (b == brushes_.end()) b = brushes_.emplace(bg, CreateSolidBrush(colorref(bg))).first;
      return reinterpret_cast<LRESULT>(b->second);
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      RECT cr;
      GetClientRect(hwnd_, &cr);
      RECT line = {0, px(44), cr.right, px(44) + std::max(1, px(1))};
      FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wp == kToastTimer) {
        KillTimer(hwnd_, kToastTimer);
        hide_toast();
      }
      return 0;
    case WM_MOVE:
      hide_toast();
      return 0;
    case kWakeMsg:
      if (sink_) sink_->on_tasks();
      return 0;
    case kQuitMsg:
      DestroyWindow(hwnd_);
      return 0;
    case WM_CLOSE:
      if (sink_ && !quitting_) {
        sink_->on_close();
      } else {
        DestroyWindow(hwnd_);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kToastTimer);
      for (auto& [id, c] : controls_) c->node->backend_data = nullptr;
      controls_.clear();
      hwnd_ = nullptr;
      PostQuitMessage(exit_code_);
      return 0;
    default:
      return DefWindowProcW(hwnd_, msg, wp, lp);
  }
}

void Win32Backend::set_title(const std::u16string& title) {
  title_ = title;
  if (hwnd_) SetWindowTextW(hwnd_, wide(title).c_str());
}

void Win32Backend::set_content(ui::Node* root) {
  root_ = root;
  if (hwnd_) rebuild();
}

bool Win32Backend::attached(const ui::Node* n) const {
  for (; n; n = n->parent) {
    if (n == root_) return true;
  }
  return false;
}

void Win32Backend::node_changed(ui::Node* node) {
  if (!hwnd_ || !node) return;
  if (node->backend_data) apply(*static_cast<Control*>(node->backend_data));
  if (attached(node)) layout();
}

void Win32Backend::tree_changed(ui::Node* parent) {
  if (hwnd_ && attached(parent)) rebuild();
}

void Win32Backend::realize(ui::Node* n) {
  const wchar_t* cls = nullptr;
  DWORD style = WS_CHILD;
  DWORD ex = 0;
  switch (n->kind) {
    case ui::NodeKind::kText:
      cls = L"STATIC";
      style |= SS_LEFT | SS_NOPREFIX | SS_NOTIFY;
      break;
    case ui::NodeKind::kButton:
      cls = L"BUTTON";
      style |= WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE;
      break;
    case ui::NodeKind::kEdit:
      cls = L"EDIT";
      style |= WS_TABSTOP | ES_AUTOHSCROLL;
      ex = WS_EX_CLIENTEDGE;
      break;
    default:
      break;
  }
  if (cls) {
    auto c = std::make_unique<Control>();
    c->id = next_id_++;
    c->node = n;
    c->hwnd = CreateWindowExW(ex, cls, L"", style, 0, 0, 10, 10, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(c->id)), GetModuleHandleW(nullptr), nullptr);
    n->backend_data = c.get();
    apply(*c);
    controls_[c->id] = std::move(c);
  }
  for (ui::Node* child : n->children) realize(child);
}

void Win32Backend::unrealize(ui::Node* n) {
  if (auto* c = static_cast<Control*>(n->backend_data)) {
    DestroyWindow(c->hwnd);
    n->backend_data = nullptr;
    controls_.erase(c->id);
  }
  for (ui::Node* child : n->children) unrealize(child);
}

void Win32Backend::rebuild() {
  if (!hwnd_) return;
  SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
  for (auto& [id, c] : controls_) {
    DestroyWindow(c->hwnd);
    c->node->backend_data = nullptr;
  }
  controls_.clear();
  if (root_) realize(root_);
  layout();
  SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void Win32Backend::apply(Control& c) {
  ui::Node* n = c.node;
  updating_ = true;
  int len = GetWindowTextLengthW(c.hwnd);
  std::wstring current(size_t(len) + 1, L'\0');
  GetWindowTextW(c.hwnd, current.data(), len + 1);
  current.resize(size_t(len));
  std::wstring wanted = wide(n->text);
  if (current != wanted) SetWindowTextW(c.hwnd, wanted.c_str());
  if (n->kind == ui::NodeKind::kEdit) {
    std::wstring hint = wide(n->hint);
    SendMessageW(c.hwnd, kCueBanner, TRUE, reinterpret_cast<LPARAM>(hint.c_str()));
  }
  SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font(n->text_size_sp)), TRUE);
  EnableWindow(c.hwnd, n->enabled);
  InvalidateRect(c.hwnd, nullptr, TRUE);
  updating_ = false;
}

SIZE Win32Backend::measure(ui::Node* n, int avail) {
  avail = std::max(avail, 1);
  if (n->gone) return {0, 0};
  auto text_extent = [&](bool wrap, int width) {
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font(n->text_size_sp));
    std::wstring t = wide(n->text.empty() ? (n->hint.empty() ? u" " : n->hint) : n->text);
    RECT r = {0, 0, std::max(width, 1), 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE));
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return SIZE{r.right, r.bottom};
  };
  const int spacing = px(8);
  switch (n->kind) {
    case ui::NodeKind::kText: {
      SIZE s = text_extent(true, avail);
      return {std::min<LONG>(s.cx, avail), s.cy};
    }
    case ui::NodeKind::kButton: {
      SIZE s = text_extent(false, avail);
      LONG w = std::clamp<LONG>(s.cx + 2 * px(16), px(88), avail);
      if (n->width == -1) w = avail;
      return {w, std::max<LONG>(s.cy + 2 * px(10), px(40))};
    }
    case ui::NodeKind::kEdit: {
      SIZE s = text_extent(false, avail);
      LONG w = n->width == -2 && n->parent && n->parent->orientation == ui::Orientation::kHorizontal
                   ? std::min<LONG>(avail, std::max<LONG>(s.cx + px(24), px(160)))
                   : avail;
      return {w, s.cy + px(14)};
    }
    case ui::NodeKind::kGroup: {
      LONG w = 0, h = 0;
      int visible = 0;
      if (n->orientation == ui::Orientation::kVertical) {
        for (ui::Node* c : n->children) {
          if (c->gone) continue;
          SIZE s = measure(c, avail);
          h += s.cy + (visible++ ? spacing : 0);
          w = std::max(w, s.cx);
        }
        return {n->width == -2 ? std::max<LONG>(w, 1) : avail, h};
      }
      for (ui::Node* c : n->children) {
        if (c->gone) continue;
        SIZE s = measure(c, std::max(1, avail - int(w)));
        w += s.cx + (visible++ ? spacing : 0);
        h = std::max(h, s.cy);
      }
      return {n->width == -2 ? std::min<LONG>(w, avail) : avail, h};
    }
    default:
      return {n->width > 0 ? std::min(px(float(n->width)), avail) : 0, n->height > 0 ? px(float(n->height)) : 0};
  }
}

void Win32Backend::place(ui::Node* n, RECT r) {
  bool shown = effectively_visible(n);
  for (const ui::Node* p = n->parent; p && shown; p = p->parent) shown = effectively_visible(p);
  if (auto* c = static_cast<Control*>(n->backend_data)) {
    MoveWindow(c->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    ShowWindow(c->hwnd, shown ? SW_SHOWNA : SW_HIDE);
  }
  if (n->kind != ui::NodeKind::kGroup) return;
  const int spacing = px(8);
  const int width = r.right - r.left;
  if (n->orientation == ui::Orientation::kVertical) {
    int y = r.top;
    bool first = true;
    for (ui::Node* c : n->children) {
      if (c->gone) {
        place(c, {r.left, y, r.left, y});
        continue;
      }
      if (!first) y += spacing;
      first = false;
      SIZE s = measure(c, width);
      int w = (c->kind == ui::NodeKind::kButton || (c->kind == ui::NodeKind::kGroup && c->width == -2)) ? s.cx : width;
      place(c, {r.left, y, r.left + w, y + s.cy});
      y += s.cy;
    }
    return;
  }
  // Horizontal: preferred widths, leftover space shared by layout weights.
  std::vector<SIZE> sizes;
  int used = 0, visible = 0;
  float weights = 0;
  for (ui::Node* c : n->children) {
    SIZE s = c->gone ? SIZE{0, 0} : measure(c, std::max(1, width - used));
    if (!c->gone) {
      used += s.cx + (visible++ ? spacing : 0);
      weights += c->weight;
    }
    sizes.push_back(s);
  }
  int leftover = std::max(0, width - used);
  int x = r.left;
  bool first = true;
  for (size_t i = 0; i < n->children.size(); ++i) {
    ui::Node* c = n->children[i];
    if (c->gone) {
      place(c, {x, r.top, x, r.top});
      continue;
    }
    if (!first) x += spacing;
    first = false;
    int w = sizes[i].cx + (weights > 0 && c->weight > 0 ? int(float(leftover) * c->weight / weights) : 0);
    place(c, {x, r.top, x + w, r.top + sizes[i].cy});
    x += w;
  }
}

void Win32Backend::layout() {
  if (!hwnd_) return;
  RECT cr;
  GetClientRect(hwnd_, &cr);
  if (back_button_) MoveWindow(back_button_, px(8), px(6), px(104), px(32), TRUE);
  if (!root_) return;
  RECT area = {px(16), px(44) + px(12), cr.right - px(16), cr.bottom - px(16)};
  int width = std::max<int>(1, area.right - area.left);
  SIZE s = measure(root_, width);
  LONG w = root_->kind == ui::NodeKind::kGroup || root_->kind == ui::NodeKind::kEdit ? width : s.cx;
  place(root_, {area.left, area.top, area.left + w, area.top + s.cy});
}

void Win32Backend::show_toast(const std::u16string& text_value, bool long_duration) {
  ensure_window();
  if (!toast_ || !hwnd_) return;
  toast_text_ = text_value;
  RECT cr;
  GetClientRect(hwnd_, &cr);
  int max_w = std::max<int>(px(120), cr.right - px(64));
  HDC dc = GetDC(toast_);
  HGDIOBJ old = SelectObject(dc, font(14));
  std::wstring t = wide(text_value);
  RECT tr = {0, 0, max_w - px(24), 0};
  DrawTextW(dc, t.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(toast_, dc);
  int w = tr.right + px(24), h = tr.bottom + px(16);
  POINT origin = {(cr.right - w) / 2, cr.bottom - h - px(48)};
  ClientToScreen(hwnd_, &origin);
  SetWindowPos(toast_, HWND_TOP, origin.x, origin.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(toast_, nullptr, TRUE);
  SetTimer(hwnd_, kToastTimer, long_duration ? 3500 : 2000, nullptr);
}

void Win32Backend::hide_toast() {
  if (toast_) ShowWindow(toast_, SW_HIDE);
}

void Win32Backend::show_error(const std::u16string& title, const std::u16string& details) {
  std::wstring body = wide(details) + L"\n\nLes détails figurent dans le journal (stderr, ou --log-file).";
  MessageBoxW(hwnd_, body.c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR);
}

int Win32Backend::run(ui::EventSink& sink) {
  if (quitting_) return exit_code_;
  sink_ = &sink;
  ensure_window();
  if (!hwnd_) return 70;
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
  if (pending_wake_) {
    pending_wake_ = false;
    sink.on_tasks();
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_LEFT && (HIWORD(msg.lParam) & KF_ALTDOWN)) {
      sink.on_back();
      continue;
    }
    if (hwnd_ && IsDialogMessageW(hwnd_, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  sink_ = nullptr;
  return exit_code_;
}

void Win32Backend::quit(int code) {
  if (quitting_) return;
  quitting_ = true;
  exit_code_ = code;
  if (hwnd_) PostMessageW(hwnd_, kQuitMsg, 0, 0);
}

void Win32Backend::wake() {
  if (hwnd_) {
    PostMessageW(hwnd_, kWakeMsg, 0, 0);
  } else {
    pending_wake_ = true;
  }
}

}  // namespace aa::backend
