#pragma once

#include <deque>
#include <thread>
#include <functional>
#include <string>
#include <vector>

#include "apk-inspector/manifest.h"
#include "dex-engine/runtime.h"
#include "framework-bridge/bridge.h"
#include "framework-bridge/ui_model.h"

// Drives an Android application inside one Windows window: activity stack,
// lifecycle callbacks (with Android's ordering), intents between activities,
// posted tasks, back navigation and crash handling.
namespace aa::lifecycle {

struct AppOptions {
  // Instruction budget per event/callback (0 = unlimited).
  uint64_t max_instructions_per_event = 0;
};

class AppHost final : public bridge::Host, public ui::EventSink {
 public:
  AppHost(rt::Runtime& runtime, const apk::Manifest& manifest, ui::Backend& backend, AppOptions options = {});
  ~AppHost() override;

  // Creates the Application object (if declared) and the launch activity,
  // then runs the backend event loop. Returns the process exit code.
  int run(const std::string& activity_class_name);

  // bridge::Host
  ui::Backend* ui() override { return &backend_; }
  const std::string& package_name() const override { return manifest_.package; }
  void start_activity(rt::Handle caller, rt::Handle intent) override;
  void finish_activity(rt::Handle activity) override;
  void content_changed(rt::Handle activity) override;
  void title_changed(rt::Handle activity) override;
  void post(std::function<void()> task, uint32_t delay_ms) override;
  void console(bool is_err, const std::string& utf8) override;
  void logcat(char level, const std::string& tag, const std::string& message) override;
  rt::Handle application() override { return application_; }
  bool on_ui_thread() const override { return std::this_thread::get_id() == ui_thread_; }

  // ui::EventSink
  void on_click(ui::Node* node) override;
  void on_text_changed(ui::Node* node, const std::u16string& text) override;
  void on_back() override;
  void on_close() override;
  void on_tasks() override;

 private:
  enum class State { kCreated, kStarted, kResumed, kPaused, kStopped, kDestroyed };
  struct Record {
    rt::Handle activity;
    std::string name;  // Java class name
    State state;
  };

  // Runs `fn` as a top-level entry; Java exceptions crash the app.
  bool guarded(const std::string& what, const std::function<void()>& fn);
  void callback(Record& r, const char* name, const char* sig = "()V", std::vector<uint32_t> args = {});
  void move_to(Record& r, State target);
  rt::Handle instantiate(const std::string& descriptor, rt::Handle intent);
  void launch(rt::Handle intent);
  void finish_now(rt::Handle activity);
  void show_top();
  void crash(rt::Handle exception, const std::string& where);
  void exit_app(int code);
  void process_pending();
  std::u16string default_title(const std::string& descriptor) const;
  Record* top() { return stack_.empty() ? nullptr : &stack_.back(); }

  rt::Runtime& rt_;
  const apk::Manifest& manifest_;
  ui::Backend& backend_;
  AppOptions options_;
  rt::Handle application_ = rt::kNull;
  std::vector<Record> stack_;
  std::deque<rt::Handle> pending_starts_;
  std::deque<rt::Handle> pending_finishes_;
  std::deque<std::function<void()>> tasks_;
  bool exiting_ = false;
  int exit_code_ = 0;
  int depth_ = 0;  // nesting of guarded() calls
  size_t root_provider_ = 0;
  std::thread::id ui_thread_;
};

// Host for console programs (`anyandroid exec`): System.out goes to stdout,
// UI and activity APIs fail explicitly.
class ConsoleHost final : public bridge::Host {
 public:
  explicit ConsoleHost(std::string package) : package_(std::move(package)) {}
  ui::Backend* ui() override { return nullptr; }
  const std::string& package_name() const override { return package_; }
  void start_activity(rt::Handle, rt::Handle) override;
  void finish_activity(rt::Handle) override {}
  void content_changed(rt::Handle) override {}
  void title_changed(rt::Handle) override {}
  void post(std::function<void()> task, uint32_t) override { tasks_.push_back(std::move(task)); }
  void console(bool is_err, const std::string& utf8) override;
  void logcat(char level, const std::string& tag, const std::string& message) override;
  rt::Handle application() override { return rt::kNull; }
  void run_tasks();

 private:
  std::string package_;
  std::deque<std::function<void()>> tasks_;
};

}  // namespace aa::lifecycle
