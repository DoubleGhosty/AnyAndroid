#include "lifecycle/app_host.h"

#include <cstdio>

#include "common/log.h"
#include "common/text.h"

namespace aa::lifecycle {

using rt::Handle;
using rt::kNull;

namespace {

std::string simple_name(const std::string& java_name) {
  size_t p = java_name.rfind('.');
  return p == std::string::npos ? java_name : java_name.substr(p + 1);
}

}  // namespace

AppHost::AppHost(rt::Runtime& runtime, const apk::Manifest& manifest, ui::Backend& backend, AppOptions options)
    : rt_(runtime), manifest_(manifest), backend_(backend), options_(options), ui_thread_(std::this_thread::get_id()) {
  rt_.mutable_options().max_instructions = options.max_instructions_per_event;
  rt::Runtime::Scope scope(rt_, "main");
  // Everything the host keeps outside the Java heap is a collection root.
  root_provider_ = rt_.add_root_provider([this](std::vector<Handle>& out) {
    out.push_back(application_);
    for (const Record& r : stack_) out.push_back(r.activity);
    out.insert(out.end(), pending_starts_.begin(), pending_starts_.end());
    out.insert(out.end(), pending_finishes_.begin(), pending_finishes_.end());
  });
  // Android semantics: an uncaught exception in any thread kills the app.
  rt_.on_uncaught_exception = [this](rt::VmThread* t, Handle exception) {
    crash(exception, "thread « " + t->name + " »");
  };
  rt_.on_exit_request = [this](int code) { exit_app(code); };
}

AppHost::~AppHost() {
  rt::Runtime::Scope scope(rt_, "main");
  rt_.remove_root_provider(root_provider_);
  rt_.on_uncaught_exception = nullptr;
  rt_.on_exit_request = nullptr;
}

int AppHost::run(const std::string& activity_class_name) {
  log::set_package(manifest_.package);
  ui_thread_ = std::this_thread::get_id();
  if (!manifest_.application_class.empty()) {
    guarded("création de l'Application", [&] {
      rt::Class* c = rt_.find_class(text::java_to_descriptor(manifest_.application_class));
      if (!rt_.is_assignable(c, rt_.find_class("Landroid/app/Application;"))) {
        rt_.throw_new("Ljava/lang/ClassCastException;", c->java_name() + " cannot be cast to android.app.Application");
      }
      rt_.ensure_initialized(c);
      rt::Method* ctor = c->declared_method("<init>", "()V");
      if (!ctor) rt_.throw_new("Ljava/lang/InstantiationException;", c->java_name() + " has no zero argument constructor");
      Handle h = rt_.alloc(c);
      uint32_t args[1] = {h};
      rt_.invoke(ctor, args, 1);
      application_ = h;
      log::info("cycle", c->java_name() + ".onCreate()");
      rt_.call_virtual(h, "onCreate", "()V");
    });
  }
  if (!exiting_) {
    guarded("démarrage de l'activité principale", [&] {
      Handle intent = rt_.alloc(rt_.find_class("Landroid/content/Intent;"));
      auto p = std::make_unique<bridge::IntentPeer>();
      p->action = u"android.intent.action.MAIN";
      p->component = text::java_to_descriptor(activity_class_name);
      rt_.object(intent)->peer = std::move(p);
      launch(intent);
    });
  }
  process_pending();
  if (exiting_) return exit_code_;
  int code = backend_.run(*this);
  return exiting_ ? exit_code_ : code;
}

bool AppHost::guarded(const std::string& what, const std::function<void()>& fn) {
  if (exiting_) return false;
  rt::Runtime::Scope scope(rt_, "main");
  if (depth_ == 0) rt_.reset_budget();
  ++depth_;
  try {
    fn();
    --depth_;
    return true;
  } catch (const rt::JavaThrow& t) {
    --depth_;
    crash(t.exception, what);
  } catch (const rt::ExitRequest& e) {
    --depth_;
    log::info("cycle", "System.exit(" + std::to_string(e.code) + ")");
    exit_app(e.code);
  } catch (const std::exception& e) {
    --depth_;
    log::error("hôte", what + " : erreur interne : " + e.what());
    backend_.show_error(u"Erreur interne d'AnyAndroid", text::utf8_to_utf16(what + " : " + e.what()));
    exit_app(70);
  }
  return false;
}

void AppHost::callback(Record& r, const char* name, const char* sig, std::vector<uint32_t> args) {
  bridge::ActivityPeer* p = bridge::activity_peer(rt_, r.activity);
  if (!p) rt_.throw_new("Ljava/lang/IllegalStateException;", "activité sans état natif : " + r.name);
  p->super_called = false;
  log::info("cycle", simple_name(r.name) + "." + name + "()");
  backend_.lifecycle_event(simple_name(r.name), name);
  rt_.call_virtual(r.activity, name, sig, std::move(args));
  if (!p->super_called) {
    rt_.throw_new("Landroid/util/SuperNotCalledException;",
                  "Activity {" + manifest_.package + "/" + r.name + "} did not call through to super." + name + "()");
  }
}

void AppHost::move_to(Record& r, State target) {
  while (r.state != target && !exiting_) {
    switch (r.state) {
      case State::kCreated:
        if (target == State::kDestroyed) {
          callback(r, "onDestroy");
          r.state = State::kDestroyed;
        } else {
          callback(r, "onStart");
          r.state = State::kStarted;
        }
        break;
      case State::kStarted:
        if (target == State::kResumed) {
          callback(r, "onResume");
          r.state = State::kResumed;
        } else {
          callback(r, "onStop");
          r.state = State::kStopped;
        }
        break;
      case State::kResumed:
        callback(r, "onPause");
        r.state = State::kPaused;
        break;
      case State::kPaused:
        if (target == State::kResumed) {
          callback(r, "onResume");
          r.state = State::kResumed;
        } else {
          callback(r, "onStop");
          r.state = State::kStopped;
        }
        break;
      case State::kStopped:
        if (target == State::kDestroyed) {
          callback(r, "onDestroy");
          r.state = State::kDestroyed;
        } else {
          callback(r, "onRestart");
          callback(r, "onStart");
          r.state = State::kStarted;
        }
        break;
      case State::kDestroyed:
        return;
    }
  }
}

Handle AppHost::instantiate(const std::string& descriptor, Handle intent) {
  rt::Class* c = rt_.find_class(descriptor);
  if (!rt_.is_assignable(c, rt_.find_class("Landroid/app/Activity;"))) {
    rt_.throw_new("Ljava/lang/ClassCastException;", c->java_name() + " cannot be cast to android.app.Activity");
  }
  rt_.ensure_initialized(c);
  rt::Method* ctor = c->declared_method("<init>", "()V");
  if (!ctor) rt_.throw_new("Ljava/lang/InstantiationException;", c->java_name() + " has no zero argument constructor");
  Handle h = rt_.alloc(c);
  uint32_t args[1] = {h};
  rt_.invoke(ctor, args, 1);
  bridge::ActivityPeer* p = bridge::activity_peer(rt_, h);
  if (!p) rt_.throw_new("Ljava/lang/IllegalStateException;", c->java_name() + " : le constructeur n'a pas appelé super()");
  p->intent = intent;
  p->title = default_title(descriptor);
  return h;
}

void AppHost::launch(Handle intent) {
  bridge::IntentPeer* ip = bridge::intent_peer(rt_, intent);
  if (!ip) rt_.throw_npe("Intent");
  if (ip->component.empty()) {
    rt_.throw_new("Landroid/content/ActivityNotFoundException;",
                  u"No Activity found to handle Intent { act=" + ip->action + u" } (intents implicites non pris en charge)");
  }
  std::string java_name = text::descriptor_to_java(ip->component);
  if (!manifest_.find_activity(java_name)) {
    rt_.throw_new("Landroid/content/ActivityNotFoundException;",
                  "Unable to find explicit activity class {" + manifest_.package + "/" + java_name +
                      "}; have you declared this activity in your AndroidManifest.xml?");
  }
  const size_t prev = stack_.size();
  if (prev) move_to(stack_[prev - 1], State::kPaused);
  Handle activity = instantiate(ip->component, intent);
  stack_.push_back({activity, java_name, State::kCreated});
  callback(stack_.back(), "onCreate", "(Landroid/os/Bundle;)V", {kNull});
  move_to(stack_.back(), State::kResumed);
  show_top();
  if (prev) move_to(stack_[prev - 1], State::kStopped);
}

void AppHost::finish_now(Handle activity) {
  size_t i = 0;
  while (i < stack_.size() && stack_[i].activity != activity) ++i;
  if (i == stack_.size()) return;
  if (i + 1 == stack_.size()) {
    move_to(stack_[i], State::kPaused);
    Record finished = stack_[i];
    stack_.pop_back();
    if (!stack_.empty()) {
      move_to(stack_.back(), State::kResumed);
      show_top();
    }
    move_to(finished, State::kDestroyed);
  } else {
    move_to(stack_[i], State::kDestroyed);
    stack_.erase(stack_.begin() + long(i));
  }
  if (stack_.empty() && !exiting_) {
    log::info("cycle", "plus aucune activité : fin de l'application");
    exit_app(0);
  }
}

void AppHost::show_top() {
  Record* r = top();
  if (!r) return;
  bridge::ActivityPeer* p = bridge::activity_peer(rt_, r->activity);
  if (!p) return;
  backend_.set_title(p->title);
  bridge::ViewPeer* v = bridge::view_peer(rt_, p->content_view);
  backend_.set_content(v ? v->node.get() : nullptr);
}

void AppHost::crash(Handle exception, const std::string& where) {
  std::string details;
  try {
    details = rt_.describe_throwable(exception);
  } catch (...) {
    details = "(exception non descriptible)";
  }
  log::error("app", "FATAL EXCEPTION: main\nProcess: " + manifest_.package + " (" + where + ")\n" + details);
  std::string label = manifest_.app_label.empty() || manifest_.app_label[0] == '@' ? manifest_.package : manifest_.app_label;
  backend_.show_error(text::utf8_to_utf16("« " + label + " » s'est arrêtée"), text::utf8_to_utf16(details));
  exit_app(1);
}

void AppHost::exit_app(int code) {
  if (exiting_) return;
  exiting_ = true;
  exit_code_ = code;
  backend_.quit(code);
}

void AppHost::start_activity(Handle, Handle intent) {
  pending_starts_.push_back(intent);
  backend_.wake();
}

void AppHost::finish_activity(Handle activity) {
  if (bridge::ActivityPeer* p = bridge::activity_peer(rt_, activity)) {
    if (p->finishing) return;
    p->finishing = true;
  }
  pending_finishes_.push_back(activity);
  backend_.wake();
}

void AppHost::content_changed(Handle activity) {
  if (top() && top()->activity == activity) show_top();
}

void AppHost::title_changed(Handle activity) {
  if (top() && top()->activity == activity) {
    if (bridge::ActivityPeer* p = bridge::activity_peer(rt_, activity)) backend_.set_title(p->title);
  }
}

void AppHost::post(std::function<void()> task, uint32_t delay_ms) {
  // The prototype runs posted work in order on the UI thread; delays only
  // postpone it to the next turn of the event loop.
  (void)delay_ms;
  tasks_.push_back(std::move(task));
  backend_.wake();
}

void AppHost::console(bool is_err, const std::string& utf8) {
  log::info(is_err ? "System.err" : "System.out", utf8);
  backend_.app_output(std::string(is_err ? "[stderr] " : "[stdout] ") + utf8);
}

void AppHost::logcat(char level, const std::string& tag, const std::string& message) {
  log::Level l = level == 'E' || level == 'F' ? log::Level::Error
                 : level == 'W'                ? log::Level::Warn
                 : level == 'I'                ? log::Level::Info
                                               : log::Level::Debug;
  std::string line = std::string(1, level) + "/" + tag + ": " + message;
  log::write(l, "logcat", line);
  backend_.app_output("[logcat] " + line);
}

void AppHost::process_pending() {
  if (depth_ > 0) return;  // never re-enter while a callback is running
  rt::Runtime::Scope scope(rt_, "main");  // the queues are shared with Java threads
  while (!exiting_ && (!pending_finishes_.empty() || !pending_starts_.empty() || !tasks_.empty())) {
    if (!pending_finishes_.empty()) {
      Handle h = pending_finishes_.front();
      pending_finishes_.pop_front();
      guarded("finish()", [&] { finish_now(h); });
    } else if (!pending_starts_.empty()) {
      Handle intent = pending_starts_.front();
      pending_starts_.pop_front();
      guarded("startActivity()", [&] { launch(intent); });
    } else {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      guarded("tâche postée", task);
    }
  }
}

void AppHost::on_click(ui::Node* node) {
  if (!node || exiting_) return;
  guarded("clic", [&] {
    bridge::ViewPeer* vp = bridge::view_peer(rt_, node->java_handle);
    if (vp && vp->click_listener != kNull && node->enabled) {
      rt_.call_virtual(vp->click_listener, "onClick", "(Landroid/view/View;)V", {node->java_handle});
    }
  });
  process_pending();
}

void AppHost::on_text_changed(ui::Node* node, const std::u16string& text) {
  rt::Runtime::Scope scope(rt_, "main");
  if (node) node->text = text;
}

void AppHost::on_back() {
  if (exiting_) return;
  if (Record* r = top()) {
    Handle a = r->activity;
    guarded("retour", [&] { rt_.call_virtual(a, "onBackPressed", "()V"); });
  }
  process_pending();
}

void AppHost::on_close() {
  if (exiting_) return;
  rt::Runtime::Scope scope(rt_, "main");
  while (!stack_.empty() && !exiting_) {
    Record r = stack_.back();
    stack_.pop_back();
    guarded("fermeture", [&] { move_to(r, State::kDestroyed); });
  }
  exit_app(0);
}

void AppHost::on_tasks() { process_pending(); }

std::u16string AppHost::default_title(const std::string& descriptor) const {
  std::string java_name = text::descriptor_to_java(descriptor);
  if (const apk::Component* c = manifest_.find_activity(java_name)) {
    if (!c->label.empty() && c->label[0] != '@') return text::utf8_to_utf16(c->label);
  }
  if (!manifest_.app_label.empty() && manifest_.app_label[0] != '@') return text::utf8_to_utf16(manifest_.app_label);
  return text::utf8_to_utf16(simple_name(java_name));
}

// --- ConsoleHost -------------------------------------------------------------------

void ConsoleHost::start_activity(Handle, Handle) {
  log::error("hôte", "startActivity() appelé en mode console : ignoré");
}

void ConsoleHost::console(bool is_err, const std::string& utf8) {
  std::string line = utf8 + "\n";
  std::fwrite(line.data(), 1, line.size(), is_err ? stderr : stdout);
  std::fflush(is_err ? stderr : stdout);
}

void ConsoleHost::logcat(char level, const std::string& tag, const std::string& message) {
  log::info("logcat", std::string(1, level) + "/" + tag + ": " + message);
}

void ConsoleHost::run_tasks() {
  while (!tasks_.empty()) {
    auto t = std::move(tasks_.front());
    tasks_.pop_front();
    t();
  }
}

}  // namespace aa::lifecycle
