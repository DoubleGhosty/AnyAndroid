// Java threads on Windows threads, serialised by a global interpreter lock.
//
// Invariants:
//  - Java code, the heap and the class linker are only touched by the thread
//    that holds `gil_` (Runtime::running_).
//  - A thread releases the GIL only through park() or at the end of its
//    outermost Scope. Before parking it records its stack position and spills
//    its registers so that the collector can scan its stack conservatively.

#include <algorithm>
#include <chrono>

#include "common/clock.h"
#include "common/log.h"
#include "common/text.h"
#include "dex-engine/runtime.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace aa::rt {

namespace {

std::atomic<uint64_t> g_next_instance{1};

// Per OS thread: the VmThread it is attached as, for each live runtime.
// Plain arrays on purpose: thread_local objects with destructors are fragile
// with statically linked MinGW runtimes (destructors run at thread exit).
constexpr int kMaxAttachments = 16;
struct Attachment {
  uint64_t instance;
  VmThread* thread;
};
thread_local Attachment t_attached[kMaxAttachments];
thread_local int t_attached_count = 0;

VmThread* find_attached(uint64_t instance) {
  for (int i = 0; i < t_attached_count; ++i) {
    if (t_attached[i].instance == instance) return t_attached[i].thread;
  }
  return nullptr;
}

void remember_attached(uint64_t instance, VmThread* t) {
  if (t_attached_count == kMaxAttachments) throw std::runtime_error("trop de runtimes attachés à ce thread");
  t_attached[t_attached_count++] = {instance, t};
}

void forget_attached(uint64_t instance) {
  for (int i = 0; i < t_attached_count; ++i) {
    if (t_attached[i].instance == instance) {
      t_attached[i] = t_attached[--t_attached_count];
      return;
    }
  }
}

const char* current_stack_high() {
#ifdef _WIN32
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  return reinterpret_cast<const char*>(high);
#else
  static thread_local char base_marker;
  return &base_marker + 4096;
#endif
}

}  // namespace

// --- Scopes and the GIL ----------------------------------------------------------

Runtime::Scope::Scope(Runtime& rt, const char* thread_name) : rt_(rt) {
  if (rt.instance_id_ == 0) rt.instance_id_ = g_next_instance++;
  VmThread* t = find_attached(rt.instance_id_);
  if (!t) {
    t = rt.attach_current_thread(thread_name);  // returns with the GIL held
  } else if (t->scope_depth == 0) {
    rt.acquire_gil(t);
  }
  thread_ = t;
  t->scope_depth++;
}

Runtime::Scope::~Scope() {
  if (--thread_->scope_depth == 0) rt_.release_gil(thread_);
}

VmThread* Runtime::attach_current_thread(const std::string& name) {
  auto vt = std::make_unique<VmThread>();
  vt->rt = this;
  vt->name = name;
  vt->attached = true;
  vt->stack_high = current_stack_high();
  vt->gil = std::unique_lock<std::mutex>(gil_, std::defer_lock);
  VmThread* t = vt.get();
  acquire_gil(t);
  t->id = next_thread_id_++;
  threads_.push_back(std::move(vt));
  remember_attached(instance_id_, t);
  return t;
}

void Runtime::acquire_gil(VmThread* t) {
  gil_waiters_++;
  t->gil.lock();
  gil_waiters_--;
  gil_generation_++;
  gil_handoff_.notify_all();
  running_ = t;
  t->in_vm = true;
  t->parked_sp = nullptr;
}

void Runtime::release_gil(VmThread* t) {
  t->in_vm = false;
  if (running_ == t) running_ = nullptr;
  t->gil.unlock();
}

bool Runtime::park(VmThread* t, std::condition_variable& cv, const std::function<bool()>& pred, int64_t timeout_ms) {
  if (shutting_down_ && !t->attached) throw ThreadKill{};
  if (pred()) return true;
  // Spill callee-saved registers and record the stack top for the collector.
  setjmp(t->parked_regs);
  volatile char marker = 0;
  t->parked_sp = const_cast<char*>(&marker);
  running_ = nullptr;
  bool ok;
  auto wake = [&] { return shutting_down_.load() || pred(); };
  if (timeout_ms > 0) {
    // The condition variable's timed wait ends on a system tick (~15.6 ms on
    // Windows, whatever timeBeginPeriod asked). It covers all but the last
    // tick; the rest goes by 1 ms high-resolution sleeps, the lock released,
    // the predicate checked between them (a wakeup is seen within 1 ms).
    constexpr int64_t kTickNanos = 16000000, kStepNanos = 1000000;
    constexpr int64_t kDayMs = int64_t(24) * 3600 * 1000;
    const int64_t deadline = clock::monotonic_nanos() + std::min(timeout_ms, kDayMs) * 1000000;
    ok = false;
    while (!(ok = wake())) {
      int64_t left = deadline - clock::monotonic_nanos();
      if (left <= 0) break;
      if (left > 2 * kTickNanos) {
        cv.wait_for(t->gil, std::chrono::nanoseconds(left - kTickNanos), wake);
        continue;
      }
      t->gil.unlock();
      bool slept = clock::precise_sleep(std::min(left, kStepNanos));
      t->gil.lock();
      if (!slept) {  // no high-resolution timer: the tick it is
        ok = cv.wait_until(t->gil, std::chrono::steady_clock::now() + std::chrono::nanoseconds(left), wake);
        break;
      }
    }
    // A day at most per call: the callers' loops resume longer waits.
  } else {
    cv.wait(t->gil, wake);
    ok = true;
  }
  running_ = t;
  t->parked_sp = nullptr;
  if (shutting_down_ && !t->attached) throw ThreadKill{};
  return ok && pred();
}

void Runtime::yield_thread() {
  VmThread* t = running_;
  if (!t || gil_waiters_ == 0) return;
  uint64_t generation = gil_generation_;
  park(t, gil_handoff_, [&] { return gil_generation_ != generation || gil_waiters_ == 0; }, 0);
}

void Runtime::safepoint() {
  VmThread* t = running_;
  if (!t) return;
  if (shutting_down_ && !t->attached) throw ThreadKill{};
  if (gc_requested_ && !in_gc_) collect_garbage();
  if (!cleared_references_.empty() && !in_gc_) deliver_cleared_references();
  if (gil_waiters_ > 0 && ++t->poll_count >= 1000) {
    t->poll_count = 0;
    yield_thread();
  }
}

// --- Thread lifecycle --------------------------------------------------------------

Handle Runtime::current_thread_peer() {
  VmThread* t = running_;
  if (!t) return kNull;
  if (t->peer == kNull) {
    Handle h = alloc(find_class("Ljava/lang/Thread;"));
    auto p = std::make_unique<ThreadPeer>();
    p->vm = t;
    p->name = text::utf8_to_utf16(t->name);
    p->started = true;
    p->daemon = t->daemon;
    object(h)->peer = std::move(p);
    t->peer = h;
    init_thread_object(*this, h);
  }
  return t->peer;
}

VmThread* Runtime::start_thread(Handle peer, const std::string& name, bool daemon) {
  auto vt = std::make_unique<VmThread>();
  vt->rt = this;
  vt->id = next_thread_id_++;
  vt->name = name;
  vt->peer = peer;
  vt->daemon = daemon;
  vt->gil = std::unique_lock<std::mutex>(gil_, std::defer_lock);
  VmThread* t = vt.get();
  threads_.push_back(std::move(vt));
  try {
    t->os = std::thread([this, t] { thread_main(t); });
  } catch (const std::system_error&) {
    t->alive = false;
    throw_new("Ljava/lang/OutOfMemoryError;", "unable to create new native thread");
  }
  stats_.threads_started++;
  log::debug("threads", "thread démarré : " + name);
  return t;
}

void Runtime::thread_main(VmThread* t) {
  if (instance_id_ == 0) instance_id_ = g_next_instance++;
  remember_attached(instance_id_, t);
  t->stack_high = current_stack_high();
  acquire_gil(t);
  t->scope_depth = 1;
  try {
    try {
      call_virtual(t->peer, "run", "()V");
    } catch (const JavaThrow& e) {
      Handle handler = kNull;
      if (auto* p = peer<ThreadPeer>(t->peer)) handler = p->handler;
      if (!handler) {
        auto it = object_cache_.find("thread:default_handler");
        if (it != object_cache_.end()) handler = it->second;
      }
      if (handler) {
        call_virtual(handler, "uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V", {t->peer, e.exception});
      } else if (on_uncaught_exception) {
        on_uncaught_exception(t, e.exception);
      } else {
        log::error("threads", "Exception in thread \"" + t->name + "\" " + describe_throwable(e.exception));
      }
    }
  } catch (const JavaThrow& e) {
    log::error("threads", "exception dans le gestionnaire d'exceptions du thread " + t->name);
  } catch (const ThreadKill&) {
  } catch (const ExitRequest& e) {
    if (on_exit_request) on_exit_request(e.code);
  } catch (const std::exception& e) {
    log::error("threads", "erreur interne dans le thread " + t->name + " : " + e.what());
  }
  t->alive = false;
  t->frames.clear();
  t->scope_depth = 0;
  threads_cv_.notify_all();
  release_gil(t);
  forget_attached(instance_id_);
}

VmThread* Runtime::attached_thread() {
  if (instance_id_ == 0) instance_id_ = g_next_instance++;
  return find_attached(instance_id_);
}

void Runtime::detach_current_thread() {
  if (VmThread* t = find_attached(instance_id_)) {
    t->alive = false;
    t->in_vm = false;
    forget_attached(instance_id_);
  }
}

VmThread* Runtime::thread_of_peer(Handle peer) {
  for (const auto& t : threads_) {
    if (t->peer == peer) return t.get();
  }
  return nullptr;
}

void Runtime::check_interrupted(VmThread* t) {
  if (t->interrupted) {
    t->interrupted = false;
    throw_new("Ljava/lang/InterruptedException;");
  }
}

void Runtime::sleep(int64_t millis) {
  VmThread* t = running_;
  if (millis < 0) throw_new("Ljava/lang/IllegalArgumentException;", "timeout value is negative");
  check_interrupted(t);
  if (millis == 0) {
    yield_thread();
    return;
  }
  const int64_t start = clock::monotonic_nanos();
  const int64_t deadline = millis >= (INT64_MAX - start) / 1000000 ? INT64_MAX : start + millis * 1000000;
  while (!t->interrupted) {
    int64_t left = deadline - clock::monotonic_nanos();
    if (left <= 0) break;
    park(t, t->sleep_cv, [t] { return t->interrupted; }, left / 1000000 + (left % 1000000 != 0));
  }
  check_interrupted(t);
}

void Runtime::park_current(bool absolute, int64_t time) {
  VmThread* t = running_;
  if (t->park_permit) {
    t->park_permit = false;
    return;
  }
  if (t->interrupted) return;
  int64_t ms = 0;  // 0: no timeout
  if (absolute) {
    ms = time - clock::unix_millis();
    if (ms <= 0) return;
  } else if (time < 0) {
    return;
  } else if (time > 0) {
    ms = std::max<int64_t>(1, time / 1000000 + (time % 1000000 != 0));
  }
  park(t, t->sleep_cv, [t] { return t->park_permit || t->interrupted; }, ms);
  t->park_permit = false;
}

void Runtime::unpark(Handle thread_peer) {
  VmThread* t = thread_of_peer(thread_peer);
  if (!t) return;  // not started: no guarantee, as in Java
  t->park_permit = true;
  t->sleep_cv.notify_all();
}

void Runtime::interrupt(VmThread* target) {
  target->interrupted = true;
  target->sleep_cv.notify_all();
  if (target->waiting_on) {
    target->waiting_on->waitset.notify_all();
    target->waiting_on->entry.notify_all();
  }
}

bool Runtime::join(VmThread* target, int64_t millis) {
  VmThread* t = running_;
  if (target == t) {
    // Thread.join() on itself waits forever in Java; refuse explicitly.
    throw_new("Ljava/lang/IllegalStateException;", "[AnyAndroid] un thread ne peut pas attendre sa propre fin");
  }
  check_interrupted(t);
  bool done = park(t, threads_cv_, [&] { return !target->alive || t->interrupted; }, millis);
  check_interrupted(t);
  return done && !target->alive;
}

void Runtime::wait_for_non_daemon_threads() {
  VmThread* t = running_;
  park(t, threads_cv_, [&] {
    for (const auto& other : threads_) {
      if (!other->attached && !other->daemon && other->alive) return false;
    }
    return true;
  }, 0);
}

void Runtime::shutdown_threads() {
  if (VmThread* self = find_attached(instance_id_)) {
    // The destroying thread must not hold the GIL while joining.
    if (self->scope_depth > 0) {
      self->scope_depth = 0;
      release_gil(self);
    }
  }
  shutting_down_ = true;
  {
    std::lock_guard<std::mutex> lock(gil_);
    gil_handoff_.notify_all();
    threads_cv_.notify_all();
    for (auto& t : threads_) {
      t->sleep_cv.notify_all();
      if (t->waiting_on) {
        t->waiting_on->waitset.notify_all();
        t->waiting_on->entry.notify_all();
      }
    }
  }
  for (auto& t : threads_) {
    if (t->os.joinable()) t->os.join();
  }
  forget_attached(instance_id_);
}

// --- Monitors ------------------------------------------------------------------------

Monitor& Runtime::monitor_of(Object& o) {
  if (!o.monitor) o.monitor = std::make_unique<Monitor>();
  return *o.monitor;
}

void Runtime::monitor_enter(Handle h) {
  VmThread* t = running_;
  Monitor& m = monitor_of(nonnull(h, "synchronized sur une référence null"));
  if (m.owner && m.owner != t) {
    t->waiting_on = &m;
    try {
      park(t, m.entry, [&] { return m.owner == nullptr; }, 0);
    } catch (...) {
      t->waiting_on = nullptr;
      throw;
    }
    t->waiting_on = nullptr;
  }
  m.owner = t;
  m.count++;
}

void Runtime::monitor_exit(Handle h) {
  VmThread* t = running_;
  Object& o = nonnull(h, "fin de synchronized sur une référence null");
  if (!o.monitor || o.monitor->owner != t) {
    throw_new("Ljava/lang/IllegalMonitorStateException;", "monitor-exit sans possession du moniteur");
  }
  Monitor& m = *o.monitor;
  if (--m.count == 0) {
    m.owner = nullptr;
    m.entry.notify_one();
  }
}

bool Runtime::holds_lock(Handle h) {
  Object& o = nonnull(h, "holdsLock(null)");
  return o.monitor && o.monitor->owner == running_;
}

void Runtime::monitor_wait(Handle h, int64_t millis) {
  VmThread* t = running_;
  if (millis < 0) throw_new("Ljava/lang/IllegalArgumentException;", "timeout value is negative");
  Object& o = nonnull(h, "wait() sur une référence null");
  if (!o.monitor || o.monitor->owner != t) {
    throw_new("Ljava/lang/IllegalMonitorStateException;", "object not locked by thread before wait()");
  }
  check_interrupted(t);
  Monitor& m = *o.monitor;
  Monitor::Waiter w;
  m.waiters.push_back(&w);
  uint32_t saved = m.count;
  m.owner = nullptr;
  m.count = 0;
  m.entry.notify_one();
  t->waiting_on = &m;
  struct Cleanup {
    Monitor& m;
    Monitor::Waiter* w;
    VmThread* t;
    ~Cleanup() {
      std::erase(m.waiters, w);
      t->waiting_on = nullptr;
    }
  } cleanup{m, &w, t};
  park(t, m.waitset, [&] { return w.notified || t->interrupted; }, millis);
  std::erase(m.waiters, &w);
  park(t, m.entry, [&] { return m.owner == nullptr; }, 0);
  m.owner = t;
  m.count = saved;
  check_interrupted(t);
}

void Runtime::monitor_notify(Handle h, bool all) {
  VmThread* t = running_;
  Object& o = nonnull(h, "notify() sur une référence null");
  if (!o.monitor || o.monitor->owner != t) {
    throw_new("Ljava/lang/IllegalMonitorStateException;", "object not locked by thread before notify()");
  }
  Monitor& m = *o.monitor;
  if (m.waiters.empty()) return;
  if (all) {
    for (Monitor::Waiter* w : m.waiters) w->notified = true;
    m.waiters.clear();
  } else {
    m.waiters.front()->notified = true;
    m.waiters.erase(m.waiters.begin());
  }
  m.waitset.notify_all();
}

}  // namespace aa::rt
