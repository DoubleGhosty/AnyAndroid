// Skia's configuration in AnyAndroid: Android's own (SkUserConfigManual.h,
// with SK_BUILD_FOR_ANDROID_FRAMEWORK and its rendering behaviours), built
// for Windows with GCC (MinGW).
#pragma once

#include "include/config/SkUserConfigManual.h"

// Skia assumes MSVC's __forceinline on Windows.
#if defined(__GNUC__) && !defined(SK_ALWAYS_INLINE)
#define SK_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif
