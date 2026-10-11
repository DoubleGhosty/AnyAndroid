// Host-driven Looper and virtual clock for deterministic tests (ADR-006 J5).
// In test mode, the host controls time progression and drives the message loop
// explicitly instead of waiting on real time.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace aa::bridge {

// Virtual clock for deterministic tests. In production mode, delegates to
// real monotonic time. In test mode, time only advances when the host calls
// advance().
class VirtualClock {
 public:
  enum class Mode { kRealTime, kVirtual };

  static VirtualClock& instance() {
    static VirtualClock clock;
    return clock;
  }

  // Switch between real-time and virtual modes. Must be called before any
  // time queries in the session.
  void set_mode(Mode m) {
    std::lock_guard lock(mutex_);
    mode_ = m;
    if (m == Mode::kVirtual) virtual_nanos_ = 0;
  }

  Mode mode() const {
    std::lock_guard lock(mutex_);
    return mode_;
  }

  // Current monotonic time in nanoseconds. In real-time mode, returns actual
  // monotonic time. In virtual mode, returns the virtual time set by advance().
  int64_t now_nanos() const;

  // Virtual mode only: advance virtual time by the given nanoseconds.
  // Returns the new virtual time.
  int64_t advance(int64_t delta_nanos) {
    std::lock_guard lock(mutex_);
    if (mode_ != Mode::kVirtual) return now_nanos();
    virtual_nanos_ += delta_nanos;
    return virtual_nanos_;
  }

  // Virtual mode only: set virtual time to an absolute value.
  void set_time(int64_t nanos) {
    std::lock_guard lock(mutex_);
    if (mode_ == Mode::kVirtual) virtual_nanos_ = nanos;
  }

 private:
  VirtualClock() = default;
  mutable std::mutex mutex_;
  Mode mode_ = Mode::kRealTime;
  int64_t virtual_nanos_ = 0;
};

// Message loop driver for host-controlled execution (J5).
// In production, MessageQueue uses park/unpark for natural flow.
// In test mode, the host explicitly drives the loop: runUntilIdle(),
// advanceTimeBy(), etc.
class LooperDriver {
 public:
  struct PendingWake {
    int64_t deadline_nanos;  // when this wake should fire
    int64_t queue_id;        // which MessageQueue to wake
  };

  static LooperDriver& instance() {
    static LooperDriver driver;
    return driver;
  }

  // Enable host-driven mode. Must be called before Looper.prepare().
  void enable_host_driven(bool enabled) {
    std::lock_guard lock(mutex_);
    host_driven_ = enabled;
  }

  bool is_host_driven() const {
    std::lock_guard lock(mutex_);
    return host_driven_;
  }

  // Register a pending wake: MessageQueue has a message at deadline_nanos.
  // In host-driven mode, the host will advance time to trigger it.
  void register_wake(int64_t queue_id, int64_t deadline_nanos);

  // Cancel all pending wakes for a queue (called when nativeWake or new message).
  void cancel_wakes(int64_t queue_id);

  // Host-driven mode: advance virtual time to the next pending message deadline,
  // wake the corresponding queue, and return the new virtual time.
  // Returns nullopt if no pending messages.
  std::optional<int64_t> advance_to_next_message();

  // Host-driven mode: advance virtual time by delta_nanos, waking any queues
  // whose messages are now due. Returns the new virtual time.
  int64_t advance_time_by(int64_t delta_nanos);

  // Host-driven mode: run until all pending messages up to the current virtual
  // time have been processed. Returns the number of messages processed.
  int run_until_idle();

  // Get the next wake deadline, if any (for testing/debugging).
  std::optional<int64_t> next_wake_deadline() const;

 private:
  LooperDriver() = default;
  mutable std::mutex mutex_;
  bool host_driven_ = false;
  std::multimap<int64_t, int64_t> pending_wakes_;  // deadline -> queue_id
};

}  // namespace aa::bridge
