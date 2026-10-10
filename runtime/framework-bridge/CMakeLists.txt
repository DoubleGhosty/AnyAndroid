add_library(aa_bridge STATIC
  android_app.cpp
  android_widget.cpp
  bridge.cpp
  framework_natives.cpp
  framework_parcel.cpp
  framework_resources.cpp
  java_lang.cpp
  java_reflect.cpp
  java_util.cpp
  libcore_charsets.cpp
  libcore_files.cpp
  libcore_natives.cpp
  libcore_nio.cpp)
target_link_libraries(aa_bridge PUBLIC aa_dex aa_vfs aa_res)
