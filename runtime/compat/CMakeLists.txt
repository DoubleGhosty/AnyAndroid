add_library(aa_compat STATIC compat.cpp gaps.cpp)
target_link_libraries(aa_compat PUBLIC aa_apk aa_dex)
