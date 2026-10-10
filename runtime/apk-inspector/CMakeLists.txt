add_library(aa_apk STATIC
  apk.cpp
  axml.cpp
  elf.cpp
  inflate.cpp
  manifest.cpp
  zip_reader.cpp)
target_link_libraries(aa_apk PUBLIC aa_common aa_dexfile)
