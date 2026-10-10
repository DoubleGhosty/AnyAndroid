#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "framework-bridge/ui_model.h"

namespace aa::backend {

// Window-less backend driven by a script, used for automated tests and CI.
// Commands (separated by ';' or newlines, '#' starts a comment):
//   dump                    print the current screen
//   click <sélecteur>       click a view
//   type <sélecteur> <txt>  replace the text of an EditText
//   expect "<texte>"        fail unless a visible view shows this text
//   back                    Android back button
//   close                   close the window
// Selectors: "texte exact" | #id | Type | Type:N (N-th view of that type).
class HeadlessBackend final : public ui::Backend {
 public:
  HeadlessBackend(const std::string& script, std::FILE* out);

  void set_title(const std::u16string& title) override { title_ = title; }
  void set_content(ui::Node* root) override { root_ = root; }
  void node_changed(ui::Node*) override {}
  void tree_changed(ui::Node*) override {}
  void show_toast(const std::u16string& text, bool long_duration) override;
  void show_error(const std::u16string& title, const std::u16string& details) override;
  void lifecycle_event(const std::string& activity, const std::string& event) override;
  void app_output(const std::string& line) override;
  int run(ui::EventSink& sink) override;
  void quit(int code) override;
  void wake() override {}

  static std::vector<std::string> parse_script(const std::string& script);

 private:
  void print(const std::string& line);
  void dump();
  void dump_node(const ui::Node& n, int depth);
  ui::Node* select(const std::string& selector);
  void fail(const std::string& message);

  std::vector<std::string> commands_;
  std::FILE* out_;
  std::u16string title_;
  ui::Node* root_ = nullptr;
  bool quit_ = false;
  int code_ = 0;
};

}  // namespace aa::backend
