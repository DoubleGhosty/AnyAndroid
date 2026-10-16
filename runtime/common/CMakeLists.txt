add_library(aa_common STATIC
  bytes.cpp
  clock.cpp
  file_io.cpp
  hash.cpp
  json.cpp
  log.cpp
  text.cpp
  unicode.cpp)
target_include_directories(aa_common PUBLIC ${AA_RUNTIME_DIR})
target_link_libraries(aa_common PUBLIC aa_compile_options)
if(WIN32)
  target_link_libraries(aa_common PRIVATE winmm)
endif()
