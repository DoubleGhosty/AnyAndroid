add_library(aa_image STATIC
  aosp.cpp
  erofs.cpp
  ext4.cpp
  filesystem.cpp
  lz4.cpp
  source.cpp)
target_link_libraries(aa_image PUBLIC aa_common aa_apk)
