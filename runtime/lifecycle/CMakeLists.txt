add_library(aa_lifecycle STATIC app_host.cpp)
target_link_libraries(aa_lifecycle PUBLIC aa_bridge aa_apk)
