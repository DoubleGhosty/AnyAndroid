// Skia's configuration in AnyAndroid: Android's own (SkUserConfigManual.h,
// with SK_BUILD_FOR_ANDROID_FRAMEWORK and its rendering behaviours), built
// for Windows with GCC (MinGW).
#pragma once

#include "include/config/SkUserConfigManual.h"

// Android's pixel order: N32 is RGBA (Skia picks BGRA on Windows), as the
// bytes that Bitmap.copyPixelsToBuffer gives applications.
#ifndef SK_R32_SHIFT
#define SK_R32_SHIFT 0
#endif

// Skia assumes MSVC's __forceinline on Windows.
#if defined(__GNUC__) && !defined(SK_ALWAYS_INLINE)
#define SK_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif
