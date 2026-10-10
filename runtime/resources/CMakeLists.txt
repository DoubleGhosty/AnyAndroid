add_library(aa_res STATIC
  asset_manager.cpp
  res_config.cpp
  res_table.cpp
  res_xml.cpp)
target_link_libraries(aa_res PUBLIC aa_common aa_apk)
