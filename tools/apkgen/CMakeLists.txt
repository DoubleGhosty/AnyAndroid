# Tooling used to produce test fixtures and sample APKs from text sources.
add_library(aa_tooling STATIC
  axml_writer.cpp
  dex_writer.cpp
  res_compiler.cpp
  smali.cpp
  xml_text.cpp
  zip_writer.cpp)
target_include_directories(aa_tooling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aa_tooling PUBLIC aa_apk aa_dexfile aa_common)

add_executable(apkgen main.cpp)
target_link_libraries(apkgen PRIVATE aa_tooling)
if(MINGW)
  target_link_options(apkgen PRIVATE -municode)
endif()
