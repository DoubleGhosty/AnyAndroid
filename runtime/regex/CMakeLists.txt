add_library(aa_regex STATIC
  regex.cpp)
target_include_directories(aa_regex PUBLIC ${AA_RUNTIME_DIR})
target_link_libraries(aa_regex PUBLIC aa_common)
