# DEX format parsing (used by the static inspector) and the runtime proper.
add_library(aa_dexfile STATIC
  dex_file.cpp
  opcodes.cpp)
target_link_libraries(aa_dexfile PUBLIC aa_common)

add_library(aa_dex STATIC
  code.cpp
  core_classes.cpp
  gc.cpp
  interpreter.cpp
  jni.cpp
  native_registry.cpp
  runtime.cpp
  threads.cpp)
target_link_libraries(aa_dex PUBLIC aa_dexfile)
