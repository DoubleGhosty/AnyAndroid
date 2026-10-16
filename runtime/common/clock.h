#pragma once

#include <cstdint>

// The runtime's clocks. Everything that reads time goes through here, so that
// System.nanoTime, SystemClock and the deadlines of timed waits share one
// monotonic clock (two clocks drift apart and break Looper timeouts).
namespace aa::clock {

// Monotonic time in nanoseconds since an arbitrary origin (boot on Windows).
int64_t monotonic_nanos();

// Wall-clock time in milliseconds since the Unix epoch.
int64_t unix_millis();

// Asks the system for 1 ms timer resolution for the rest of the process. On
// Windows the default tick is ~15.6 ms: without this a 1 ms timed wait
// (Thread.sleep, LockSupport.parkNanos, MessageQueue.nativePollOnce) lasts a
// whole tick, and anything paced by delays runs many times too slowly with
// nothing in the logs. Windows may ignore the request (seen on the CI's
// Windows Server: 14 ms per 1 ms wait), hence precise_sleep. Idempotent; a
// no-op elsewhere.
void use_precise_timers();

// Sleeps about `nanos` with a high-resolution timer, whatever the system tick
// (Windows 10 1803 and later). False, without sleeping, where there is none.
bool precise_sleep(int64_t nanos);

}  // namespace aa::clock
