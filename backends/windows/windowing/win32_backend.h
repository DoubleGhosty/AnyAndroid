#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "framework-bridge/ui_model.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace aa::backend {

// Native Win32 window: one top-level window per application, Android views
// realised as standard controls (STATIC, BUTTON, EDIT) with a LinearLayout-like
// layout pass. 1 dp = 1 Windows DIP (96 DPI), scaled per monitor.
class Win32Backend final : public ui::Backend {
 public:
  Win32Backend();
  ~Win32Backend() override;

  void set_title(const std::u16string& title) override;
  void set_content(ui::Node* root) override;
  void node_changed(ui::Node* node) override;
  void tree_changed(ui::Node* parent) override;
  void show_toast(const std::u16string& text, bool long_duration) override;
  void show_error(const std::u16string& title, const std::u16string& details) override;
  int run(ui::EventSink& sink) override;
  void quit(int code) override;
  void wake() override;

 private:
  struct Control {
    HWND hwnd = nullptr;
    int id = 0;
    ui::Node* node = nullptr;
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

  void ensure_window();
  int px(float dp) const { return MulDiv(int(dp * 100), int(dpi_), 9600); }
  HFONT font(float sp);
  void clear_fonts();
  void realize(ui::Node* n);
  void unrealize(ui::Node* n);
  void rebuild();
  void apply(Control& c);
  bool attached(const ui::Node* n) const;
  SIZE measure(ui::Node* n, int avail_width);
  void place(ui::Node* n, RECT r);
  void layout();
  void hide_toast();

  HWND hwnd_ = nullptr;
  HWND back_button_ = nullptr;
  HWND toast_ = nullptr;
  std::u16string toast_text_;
  ui::EventSink* sink_ = nullptr;
  ui::Node* root_ = nullptr;
  std::u16string title_ = u"AnyAndroid";
  UINT dpi_ = 96;
  int next_id_ = 1000;
  int exit_code_ = 0;
  bool quitting_ = false;
  bool pending_wake_ = false;
  bool updating_ = false;
  std::map<int, HFONT> fonts_;  // by pixel height
  std::unordered_map<int, std::unique_ptr<Control>> controls_;  // by control id
  std::map<uint32_t, HBRUSH> brushes_;
};

}  // namespace aa::backend
