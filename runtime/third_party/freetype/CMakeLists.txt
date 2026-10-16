# FreeType 2.13.2 as shipped by Android (AOSP external/freetype, commit
# ba86f9bbb890, FTL: see LICENSE.TXT and docs/FTL.TXT), reduced to the
# modules listed in aa-config/aa_ftmodule.h. Its configuration
# (include/freetype/config/ftoption.h) is Android's: TrueType bytecode
# interpreter v35 (subpixel hinting disabled), variable fonts, embedded
# bitmaps. See README.md.
#
# C is enabled by the root project. Third-party code: built without the
# project's warning flags (and -Werror).
add_library(aa_freetype STATIC
  src/base/ftsystem.c
  src/base/ftinit.c
  src/base/ftdebug.c
  src/base/ftbase.c
  src/base/ftbbox.c
  src/base/ftbitmap.c
  src/base/ftfstype.c
  src/base/ftgasp.c
  src/base/ftglyph.c
  src/base/ftmm.c
  src/base/ftstroke.c
  src/base/ftsynth.c
  src/base/fttype1.c
  src/cff/cff.c
  src/psaux/psaux.c
  src/pshinter/pshinter.c
  src/psnames/psnames.c
  src/sfnt/sfnt.c
  src/smooth/smooth.c
  src/truetype/truetype.c)

target_compile_definitions(aa_freetype PRIVATE
  FT2_BUILD_LIBRARY
  "FT_CONFIG_MODULES_H=<aa_ftmodule.h>")
target_include_directories(aa_freetype PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/aa-config)
# SYSTEM: the project's -Wpedantic/-Wshadow do not apply to FreeType headers.
target_include_directories(aa_freetype SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(aa_freetype PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON)
