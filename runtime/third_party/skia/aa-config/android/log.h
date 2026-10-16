// The part of Android's <android/log.h> that SkUserConfigManual.h uses
// (SK_ABORT), implemented in aa_skia_android.cpp.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

[[noreturn]] void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
