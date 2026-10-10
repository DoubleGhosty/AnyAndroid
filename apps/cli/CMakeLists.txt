enable_language(RC)

add_executable(anyandroid main.cpp anyandroid.rc)
target_link_libraries(anyandroid PRIVATE
  aa_compat aa_lifecycle aa_image aa_vfs aa_res aa_backend_headless aa_backend_win32)
if(MINGW)
  target_link_options(anyandroid PRIVATE -municode)
endif()
