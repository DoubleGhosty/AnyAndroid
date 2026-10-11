# miniz 3.1.2 + 12 commits (2948eeb, MIT: see LICENSE and README.md): raw/zlib
# deflate and inflate, CRC-32 and Adler-32 behind the java.util.zip natives.
# Only the zlib-style stream API is built; the ZIP archive API is compiled out
# (MINIZ_NO_ARCHIVE_APIS: miniz_zip.h is still included by miniz.h, but its
# implementation, miniz_zip.c, is not vendored).
#
# C is enabled by the root project. Third-party code: built without the
# project's warning flags (and -Werror).
add_library(aa_miniz STATIC
  miniz.c
  miniz_tdef.c
  miniz_tinfl.c)

# These macros change the declarations of miniz.h, so users get them too.
target_compile_definitions(aa_miniz PUBLIC
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# SYSTEM: the project's -Wpedantic/-Wshadow do not apply to miniz headers.
target_include_directories(aa_miniz SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(aa_miniz PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
