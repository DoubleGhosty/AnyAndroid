# Text engine (ADR-007): FreeType and HarfBuzz behind the measurement rules
# of HWUI's Skia and minikin.
add_library(aa_text STATIC
  coverage.cpp
  font_collection.cpp
  font_face.cpp
  layout.cpp
  locale.cpp
  unicode.cpp)
target_include_directories(aa_text PUBLIC ${AA_RUNTIME_DIR})
target_link_libraries(aa_text PUBLIC aa_compile_options PRIVATE aa_freetype aa_harfbuzz)
