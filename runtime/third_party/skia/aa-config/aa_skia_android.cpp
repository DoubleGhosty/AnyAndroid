// __android_log_assert for SK_ABORT (Android's liblog): the message on
// stderr, then abort, as on Android.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "android/log.h"

extern "C" void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  std::fprintf(stderr, "%s: ", tag ? tag : "skia");
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  } else if (cond) {
    std::fprintf(stderr, "assertion échouée : %s", cond);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}
