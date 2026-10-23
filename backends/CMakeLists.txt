# Window backends: a scriptable headless backend (tests, CI) with the input
# scripts of `exec --input`, and the native Win32 backend.
add_library(aa_backend_headless STATIC headless/headless_backend.cpp headless/input_script.cpp)
target_include_directories(aa_backend_headless PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aa_backend_headless PUBLIC aa_bridge)

if(WIN32)
  add_library(aa_backend_win32 STATIC windows/windowing/win32_backend.cpp windows/windowing/frame_window.cpp)
  target_include_directories(aa_backend_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(aa_backend_win32 PUBLIC aa_bridge user32 gdi32)
endif()
