cmake_minimum_required(VERSION 3.21)

project(AnyAndroid
  VERSION 0.1.0
  DESCRIPTION "Runtime de compatibilité Android pour Windows (prototype de recherche)"
  LANGUAGES CXX)

if(NOT WIN32)
  message(WARNING "AnyAndroid cible Windows : le backend Win32 et la CLI ne seront pas construits.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Type de build" FORCE)
endif()

option(AA_WARNINGS_AS_ERRORS "Traiter les avertissements comme des erreurs" OFF)
option(AA_BUILD_TESTS "Construire les tests" ON)
option(AA_BUILD_SAMPLES "Construire les APK de démonstration" ON)

# Options communes à toutes les cibles du projet.
add_library(aa_compile_options INTERFACE)
target_compile_definitions(aa_compile_options INTERFACE
  NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00
  AA_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(aa_compile_options INTERFACE /W4 /utf-8 /permissive- /EHsc)
  if(AA_WARNINGS_AS_ERRORS)
    target_compile_options(aa_compile_options INTERFACE /WX)
  endif()
else()
  target_compile_options(aa_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers)
  if(AA_WARNINGS_AS_ERRORS)
    target_compile_options(aa_compile_options INTERFACE -Werror)
  endif()
endif()

# Exécutables autonomes : pas de dépendance aux DLL du runtime MinGW, et une
# pile plus grande pour l'interpréteur (un appel Java = un appel C++).
if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++ -Wl,--stack,16777216)
elseif(MSVC)
  add_link_options(/STACK:16777216)
endif()

set(AA_RUNTIME_DIR ${PROJECT_SOURCE_DIR}/runtime)

add_subdirectory(runtime/common)
add_subdirectory(runtime/apk-inspector)
add_subdirectory(runtime/image)
add_subdirectory(runtime/vfs)
add_subdirectory(runtime/resources)
add_subdirectory(runtime/dex-engine)
add_subdirectory(runtime/framework-bridge)
add_subdirectory(runtime/lifecycle)
add_subdirectory(runtime/compat)
add_subdirectory(backends)
add_subdirectory(tools/apkgen)
if(WIN32)
  add_subdirectory(apps/cli)
endif()

if(AA_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

if(AA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
