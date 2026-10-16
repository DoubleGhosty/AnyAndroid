# Skia, the version of Android 17 (AOSP external/skia, tag android-17.0.0_r1;
# BSD: see LICENSE), reduced to its CPU raster subset: core, effects, shaders,
# image filters, SkSL (runtime effects), path ops, utils, the FreeType font
# port and skcms. No GPU backend, codec, encoder, PDF or SVG. Configured as
# Android configures it (aa-config/SkUserConfig.h). See README.md.
#
# Third-party code: built without the project's warning flags (and -Werror).
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

add_library(aa_skia STATIC ${AA_SKIA_SOURCES} ${AA_SKIA_HSW_SOURCES} aa-config/aa_skia_android.cpp)

target_compile_definitions(aa_skia
  PUBLIC SK_USER_CONFIG_HEADER="SkUserConfig.h"
  # As Android's build of skcms: the baseline color transforms only.
  PRIVATE SKIA_IMPLEMENTATION=1 SKCMS_DISABLE_HSW SKCMS_DISABLE_SKX)
# SYSTEM: the project's -Wpedantic/-Wshadow do not apply to Skia's headers.
target_include_directories(aa_skia SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/aa-config
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/skcms)
target_link_libraries(aa_skia PRIVATE aa_freetype)
target_compile_options(aa_skia PRIVATE -w)

# Haswell's raster pipeline, chosen at run time (SkOpts). GCC does not align
# the stack to 32 bytes on Windows (bug 54412): its aligned AVX moves become
# unaligned ones, which cost nothing on these processors.
set_source_files_properties(${AA_SKIA_HSW_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-march=haswell;-Wa,-muse-unaligned-vector-move")

set_target_properties(aa_skia PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
