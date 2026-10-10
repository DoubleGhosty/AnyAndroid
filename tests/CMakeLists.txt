add_executable(aa_unit_tests
  unit/test_main.cpp
  unit/test_common.cpp
  unit/test_apk.cpp
  unit/test_runtime.cpp
  unit/test_threads_gc.cpp
  unit/test_reflect.cpp
  unit/test_jni.cpp
  unit/test_image.cpp
  unit/test_libcore.cpp
  unit/test_resources.cpp)
target_include_directories(aa_unit_tests PRIVATE unit)
target_link_libraries(aa_unit_tests PRIVATE aa_tooling aa_bridge aa_dex aa_apk aa_image aa_vfs)
add_test(NAME unit COMMAND aa_unit_tests)

add_executable(aa_fuzz fuzz/fuzz_apk.cpp)
target_link_libraries(aa_fuzz PRIVATE aa_apk aa_dex)
if(MINGW)
  target_link_options(aa_fuzz PRIVATE -municode)
endif()

if(NOT WIN32 OR NOT AA_BUILD_SAMPLES)
  return()
endif()

# --- End-to-end tests on the demo APKs ---------------------------------------
set(S ${AA_SAMPLES_DIR})
set(E ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
set(CLI $<TARGET_FILE:anyandroid>)

# aa_e2e(<nom> CODE <n> [GOLDEN <fichier>] [MATCH <regex>] ARGS <arguments...>)
function(aa_e2e name)
  cmake_parse_arguments(T "" "CODE;GOLDEN;MATCH" "ARGS" ${ARGN})
  string(REPLACE ";" "|" cmd "${CLI};${T_ARGS}")
  set(defs -DCMD=${cmd} -DEXPECT_CODE=${T_CODE} -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/e2e-${name}.txt)
  if(T_GOLDEN)
    list(APPEND defs -DGOLDEN=${E}/expected/${T_GOLDEN})
  endif()
  if(T_MATCH)
    list(APPEND defs "-DMATCH=${T_MATCH}")
  endif()
  add_test(NAME e2e-${name} COMMAND ${CMAKE_COMMAND} ${defs} -P ${E}/run_check.cmake)
endfunction()

aa_e2e(hello-exec CODE 0 GOLDEN hello-exec.txt
  ARGS exec ${S}/hello-dex.apk com.anyandroid.samples.hello.Hello)
aa_e2e(threads-exec CODE 0 GOLDEN threads-exec.txt
  ARGS exec ${S}/threads-demo.apk com.anyandroid.samples.threads.Main)
aa_e2e(basic-run CODE 0 GOLDEN basic-run.txt
  ARGS run ${S}/basic-activity.apk --yes --headless --script ${E}/basic-activity.script --max-instructions 5000000)
aa_e2e(nav-run CODE 0 GOLDEN nav-run.txt
  ARGS run ${S}/two-screens.apk --yes --headless --script ${E}/two-screens.script --max-instructions 5000000)
aa_e2e(basic-inspect CODE 0 MATCH "Verdict : COMPATIBLE"
  ARGS inspect ${S}/basic-activity.apk)
aa_e2e(basic-inspect-json CODE 0 MATCH "\"verdict\": \"compatible\""
  ARGS inspect ${S}/basic-activity.apk --json)
aa_e2e(native-inspect CODE 0 MATCH "Verdict : INCOMPATIBLE"
  ARGS inspect ${S}/native-lib.apk)
aa_e2e(native-run-refused CODE 4
  ARGS run ${S}/native-lib.apk --yes --headless)
aa_e2e(run-requires-confirmation CODE 5
  ARGS run ${S}/basic-activity.apk --headless)
aa_e2e(planned-command CODE 3
  ARGS install ${S}/basic-activity.apk)

# G2 (optional): the same programs on AOSP's real libcore, extracted locally
# by `anyandroid image bootclasspath` (never stored in the repository).
set(AA_LIBCORE_DIR "" CACHE PATH "Libcore AOSP extraite (tests G2 facultatifs)")
if(AA_LIBCORE_DIR)
  aa_e2e(hello-libcore CODE 0 GOLDEN hello-exec.txt
    ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only ${S}/hello-dex.apk com.anyandroid.samples.hello.Hello)
  aa_e2e(threads-libcore CODE 0 GOLDEN threads-exec.txt
    ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only ${S}/threads-demo.apk com.anyandroid.samples.threads.Main)
  aa_e2e(libcore-demo CODE 0 GOLDEN libcore-demo.txt
    ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only ${S}/libcore-demo.apk com.anyandroid.samples.libcore.Main)
  # Same program, its file system served from the system image (ADR-005).
  set(AA_SYSTEM_IMAGE "" CACHE FILEPATH "system.img d'Android (tests G2 facultatifs)")
  if(AA_SYSTEM_IMAGE)
    aa_e2e(libcore-demo-image CODE 0 GOLDEN libcore-demo.txt
      ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only --system-image ${AA_SYSTEM_IMAGE}
           ${S}/libcore-demo.apk com.anyandroid.samples.libcore.Main)
    # G3 entry point: utility classes of the real framework.jar.
    aa_e2e(framework-demo CODE 0 GOLDEN framework-demo.txt
      ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE}
           ${S}/framework-demo.apk com.anyandroid.samples.framework.Main)
  endif()
endif()

# Gap analysis on an app bundle (.apks) with a GMS-like profile.
aa_e2e(gaps-gms-profile CODE 0 GOLDEN gaps-gms-profile.txt
  ARGS gaps ${S}/gms-profile.apks --top 12)
aa_e2e(inspect-bundle CODE 0 MATCH "split config.x86_64|config.x86_64"
  ARGS inspect ${S}/gms-profile.apks --manifest)

# Malformed archives: clean diagnostics, never a crash.
aa_e2e(malformed-truncated CODE 2 ARGS inspect ${S}/malformed-truncated.apk)
aa_e2e(malformed-duplicate CODE 2 ARGS inspect ${S}/malformed-duplicate.apk)
aa_e2e(malformed-bomb CODE 2 ARGS inspect ${S}/malformed-bomb.apk)
aa_e2e(malformed-bad-crc CODE 0 MATCH "CRC invalide" ARGS inspect ${S}/malformed-bad-crc.apk)
aa_e2e(malformed-traversal CODE 0 MATCH "nom d'entrée dangereux" ARGS inspect ${S}/malformed-traversal.apk)
aa_e2e(malformed-bad-manifest CODE 0 MATCH "manifeste illisible" ARGS inspect ${S}/malformed-bad-manifest.apk)
aa_e2e(malformed-bad-dex CODE 0 MATCH "DEX_INVALID|classes.dex invalide" ARGS inspect ${S}/malformed-bad-dex.apk)
aa_e2e(malformed-bad-dex-run-refused CODE 4 ARGS run ${S}/malformed-bad-dex.apk --yes --headless)

# Fuzzing (fixed seeds, reproducible).
add_test(NAME fuzz-basic COMMAND aa_fuzz ${S}/basic-activity.apk 3000 1)
add_test(NAME fuzz-nav COMMAND aa_fuzz ${S}/two-screens.apk 3000 2)
add_test(NAME fuzz-hello-exec COMMAND aa_fuzz ${S}/hello-dex.apk 3000 3 Lcom/anyandroid/samples/hello/Hello;)
