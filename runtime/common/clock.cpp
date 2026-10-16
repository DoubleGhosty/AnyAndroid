#include "common/clock.h"

#include <chrono>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace aa::clock {

#ifdef _WIN32

int64_t monotonic_nanos() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return int64_t(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t(c.QuadPart / freq * 1000000000 + (c.QuadPart % freq) * 1000000000 / freq);
}

int64_t unix_millis() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return int64_t((t - 116444736000000000ull) / 10000);
}

void use_precise_timers() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Windows 11 may ignore the request for a process without a visible
    // window unless the process opts out of timer-resolution throttling.
    // Looked up dynamically: older systems (and wine) may lack it.
    struct PowerThrottlingState {
      ULONG version, control_mask, state_mask;
    } state{1, 0x4 /* PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION */, 0};
    using SetInfo = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
      if (auto set = reinterpret_cast<SetInfo>(reinterpret_cast<void*>(GetProcAddress(k32, "SetProcessInformation")))) {
        set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof state);
      }
    }
    // Held until the process exits (per process since Windows 10 2004).
    timeBeginPeriod(1);
  });
}

#else

int64_t monotonic_nanos() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t unix_millis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void use_precise_timers() {}

#endif

}  // namespace aa::clock
