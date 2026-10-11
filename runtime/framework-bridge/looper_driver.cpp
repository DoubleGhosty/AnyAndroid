// Host-driven Looper and virtual clock implementation (ADR-006 J5).
#include "framework-bridge/looper_driver.h"

#include <algorithm>
#include <windows.h>

namespace aa::bridge {

int64_t monotonic_nanos_real() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return int64_t(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t(c.QuadPart / freq * 1000000000 + (c.QuadPart % freq) * 1000000000 / freq);
}

int64_t VirtualClock::now_nanos() const {
  std::lock_guard lock(mutex_);
  if (mode_ == Mode::kVirtual) return virtual_nanos_;
  return monotonic_nanos_real();
}

void LooperDriver::register_wake(int64_t queue_id, int64_t deadline_nanos) {
  std::lock_guard lock(mutex_);
  if (!host_driven_) return;
  pending_wakes_.emplace(deadline_nanos, queue_id);
}

void LooperDriver::cancel_wakes(int64_t queue_id) {
  std::lock_guard lock(mutex_);
  if (!host_driven_) return;
  for (auto it = pending_wakes_.begin(); it != pending_wakes_.end();) {
    if (it->second == queue_id)
      it = pending_wakes_.erase(it);
    else
      ++it;
  }
}

std::optional<int64_t> LooperDriver::advance_to_next_message() {
  std::lock_guard lock(mutex_);
  if (!host_driven_ || pending_wakes_.empty()) return std::nullopt;

  auto it = pending_wakes_.begin();
  int64_t deadline = it->first;
  int64_t queue_id = it->second;
  pending_wakes_.erase(it);

  // Advance virtual clock to deadline
  VirtualClock::instance().set_time(deadline);

  // Wake will happen when the MessageQueue polls next
  return deadline;
}

int64_t LooperDriver::advance_time_by(int64_t delta_nanos) {
  std::lock_guard lock(mutex_);
  if (!host_driven_) return VirtualClock::instance().now_nanos();

  int64_t new_time = VirtualClock::instance().advance(delta_nanos);

  // Wake any queues whose messages are now due
  while (!pending_wakes_.empty() && pending_wakes_.begin()->first <= new_time) {
    pending_wakes_.erase(pending_wakes_.begin());
    // The wake will trigger when MessageQueue polls next
  }

  return new_time;
}

int LooperDriver::run_until_idle() {
  int processed = 0;
  while (true) {
    auto next = advance_to_next_message();
    if (!next) break;
    ++processed;
    // Give the runtime a chance to process the message
    // (actual processing happens in the MessageQueue poll)
  }
  return processed;
}

std::optional<int64_t> LooperDriver::next_wake_deadline() const {
  std::lock_guard lock(mutex_);
  if (pending_wakes_.empty()) return std::nullopt;
  return pending_wakes_.begin()->first;
}

}  // namespace aa::bridge
