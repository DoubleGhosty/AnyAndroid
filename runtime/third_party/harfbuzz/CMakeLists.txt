# HarfBuzz 10.2.0, the version of Android's external/harfbuzz_ng (Old MIT:
# see COPYING), reduced to the files that src/harfbuzz.cc (the single
# translation unit build) includes. Built with HarfBuzz's own OpenType font
# functions and Unicode data (hb-ucd): no FreeType, ICU or GLib
# integration. See README.md.
#
# Third-party code: built without the project's warning flags (and -Werror).
add_library(aa_harfbuzz STATIC src/harfbuzz.cc)

target_compile_definitions(aa_harfbuzz PRIVATE
  # HarfBuzz raises some warnings itself (#pragma GCC diagnostic): keep
  # them, and compiler upgrades, out of the build.
  HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR
  HB_NO_PRAGMA_GCC_DIAGNOSTIC_WARNING)
# SYSTEM: the project's -Wpedantic/-Wshadow do not apply to HarfBuzz headers.
target_include_directories(aa_harfbuzz SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(aa_harfbuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
