#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Toolkit-neutral view tree shared by the framework bridge (which owns the
// nodes through android.view.View peers) and the window backends (which
// realise them as native controls, or print them in headless mode).
namespace aa::ui {

enum class NodeKind { kView, kGroup, kText, kButton, kEdit };
enum class Orientation { kHorizontal, kVertical };

struct Node {
  NodeKind kind = NodeKind::kView;
  std::string type_name;     // "TextView", "Button", ... (diagnostics)
  uint32_t java_handle = 0;  // the android.view.View object
  int32_t id = -1;           // View.NO_ID
  std::u16string text;
  std::u16string hint;
  bool visible = true;   // View.VISIBLE (false = INVISIBLE or GONE)
  bool gone = false;     // View.GONE: takes no space
  bool enabled = true;
  bool clickable = false;
  float text_size_sp = 14.0f;
  uint32_t text_color = 0;  // ARGB, 0 = default
  uint32_t background = 0;  // ARGB, 0 = none
  Orientation orientation = Orientation::kHorizontal;
  float weight = 0;
  int width = -2;   // ViewGroup.LayoutParams: -1 MATCH_PARENT, -2 WRAP_CONTENT, else px
  int height = -2;
  Node* parent = nullptr;
  std::vector<Node*> children;
  void* backend_data = nullptr;  // owned by the backend
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void on_click(Node* node) = 0;
  virtual void on_text_changed(Node* node, const std::u16string& text) = 0;
  virtual void on_back() = 0;
  virtual void on_close() = 0;
  virtual void on_tasks() = 0;  // run posted tasks and pending lifecycle work
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual void set_title(const std::u16string& title) = 0;
  virtual void set_content(Node* root) = 0;  // nullptr clears the window
  virtual void node_changed(Node* node) = 0;
  virtual void tree_changed(Node* parent) = 0;
  virtual void show_toast(const std::u16string& text, bool long_duration) = 0;
  virtual void show_error(const std::u16string& title, const std::u16string& details) = 0;
  virtual void lifecycle_event(const std::string& activity, const std::string& event) {}
  virtual void app_output(const std::string& line) {}
  // Runs the event loop until quit(); returns the exit code.
  virtual int run(EventSink& sink) = 0;
  virtual void quit(int code) = 0;
  // Asks the loop to call EventSink::on_tasks() soon (thread-safe not required).
  virtual void wake() = 0;
  // Delayed wake-up for Handler.postDelayed (backends may approximate).
  virtual void wake_after(uint32_t delay_ms) { wake(); }
};

}  // namespace aa::ui
