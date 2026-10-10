# miniz 3.1.2 (MIT, see LICENSE): raw/zlib deflate and inflate, CRC-32 and
# Adler-32 behind the java.util.zip natives. Only the zlib-style stream API is
# built; the ZIP archive API (miniz_zip.c) is left out.
#
# The root project only enables C++, so C is enabled here for this target.
# Third-party code: built without the project's warning flags (and -Werror).
enable_language(C)

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
