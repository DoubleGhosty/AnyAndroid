add_executable(aa_unit_tests
  unit/test_main.cpp
  unit/test_common.cpp
  unit/test_crypto.cpp
  unit/test_apk.cpp
  unit/test_runtime.cpp
  unit/test_threads_gc.cpp
  unit/test_reflect.cpp
  unit/test_jni.cpp
  unit/test_image.cpp
  unit/test_input.cpp
  unit/test_motion_event.cpp
  unit/test_libcore.cpp
  unit/test_graphics.cpp
  unit/test_native_memory.cpp
  unit/test_zip.cpp
  unit/test_regex.cpp
  unit/test_resources.cpp
  unit/test_res_compiler.cpp
  unit/test_skia.cpp
  unit/test_text.cpp)
target_include_directories(aa_unit_tests PRIVATE unit)
target_link_libraries(aa_unit_tests PRIVATE aa_tooling aa_bridge aa_dex aa_apk aa_image aa_vfs aa_regex aa_text aa_skia
                      aa_backend_headless)
add_test(NAME unit COMMAND aa_unit_tests)
# Every HandleTable has its own kind tag (framework-bridge/handles.h).
add_test(NAME handle-kinds COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/check_handle_kinds.cmake)

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
# Cross builds run the CLI through their emulator (for example Wine).
set(CLI ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:anyandroid>)

# aa_e2e(<nom> CODE <n> [GOLDEN <fichier>] [MATCH <regex>]
#        [FRAME <largeur>x<hauteur> [<x>,<y>,#RRGGBB...]] ARGS <arguments...>)
# The `exec --bootclasspath` tests get their own data directory (--data-dir:
# /data and /sdcard of the emulated process), emptied before each run, so that
# they stay independent under `ctest -j` and a failed run leaves nothing behind.
# FRAME: the last frame a window posts is written as a PNG (exec --frame-out),
# which must exist with that size; each probe then gives the colour of a pixel
# of it, read back by `anyandroid image probe`.
function(aa_e2e name)
  cmake_parse_arguments(T "" "CODE;GOLDEN;MATCH" "FRAME;ARGS" ${ARGN})
  set(args ${T_ARGS})
  set(defs "")
  list(GET args 0 command)
  if(command STREQUAL "exec" AND "--bootclasspath" IN_LIST args)
    set(data ${CMAKE_CURRENT_BINARY_DIR}/e2e-data/${name})
    list(INSERT args 1 --data-dir ${data})
    list(APPEND defs -DFRESH_DIR=${data})
  endif()
  if(T_FRAME)
    set(frame ${CMAKE_CURRENT_BINARY_DIR}/e2e-${name}.png)
    list(INSERT args 1 --frame-out ${frame})
    list(POP_FRONT T_FRAME frame_size)
    list(APPEND defs -DFRAME=${frame} -DFRAME_SIZE=${frame_size})
    if(T_FRAME)
      string(REPLACE ";" "|" probes "${T_FRAME}")
      string(REPLACE ";" "|" probe_cli "${CLI}")
      list(APPEND defs -DFRAME_PROBES=${probes} -DPROBE_CLI=${probe_cli})
    endif()
  endif()
  string(REPLACE ";" "|" cmd "${CLI};${args}")
  list(APPEND defs -DCMD=${cmd} -DEXPECT_CODE=${T_CODE} -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/e2e-${name}.txt)
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
# Compiled resources: layout inflated from res/layout, strings of the chosen locale.
aa_e2e(resources-run CODE 0 GOLDEN resources-run.txt
  ARGS run ${S}/resources-demo.apk --yes --headless --locale fr-FR --script ${E}/resources-demo.script --max-instructions 5000000)
aa_e2e(resources-run-default-locale CODE 0 MATCH "Écran « Resources demo »[^\n]*\nLinearLayout vertical\n  TextView \"Hello from compiled resources!\""
  ARGS run ${S}/resources-demo.apk --yes --headless --locale de-DE --do dump --max-instructions 5000000)
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
  # java.util.zip (Deflater, Inflater, GZIP, CRC32, Adler32) on miniz.
  aa_e2e(zip-demo CODE 0 GOLDEN zip-demo.txt
    ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only ${S}/zip-demo.apk com.anyandroid.samples.zip.Main)
  # Same program, its file system served from the system image (ADR-005).
  set(AA_SYSTEM_IMAGE "" CACHE FILEPATH "system.img d'Android (tests G2 facultatifs)")
  if(AA_SYSTEM_IMAGE)
    aa_e2e(libcore-demo-image CODE 0 GOLDEN libcore-demo.txt
      ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --libcore-only --system-image ${AA_SYSTEM_IMAGE}
           ${S}/libcore-demo.apk com.anyandroid.samples.libcore.Main)
    # ADR-006 J0: host code (javac + D8) on the real framework.
    if(AA_ANDROID_HOST)
      aa_e2e(host-probe CODE 0 GOLDEN host-probe.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/hello-dex.apk aa.host.Probe)
      # J3: local system services ("package" through the framework's PackageParser), and the
      # trace of the framework's calls to them.
      aa_e2e(host-services CODE 0 GOLDEN host-services.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-services ${S}/basic-activity.apk aa.host.ServicesProbe com.anyandroid.samples.basic)
      # J4: direct application launch through the real framework, lifecycle up to onResume.
      aa_e2e(launch CODE 0 GOLDEN launch.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/basic-activity.apk aa.host.Launcher com.anyandroid.samples.basic)
      # J5 and J7: launch by ActivityThread's real handlers, the window's first
      # frame drawn in software (Skia) and written as a PNG. Probes: the action
      # bar and its title, then, below it, the stems of a letter of each
      # TextView, the button and its text, and the window's background.
      aa_e2e(process-launch CODE 0 GOLDEN process-launch.txt
        FRAME 1080x1920 "540,28,#1A1B20" "18,30,#E1E1E2" "2,70,#787A84" "600,70,#EDEDF6" "67,94,#787A84"
              "300,127,#D6D7D7" "544,125,#30323A" "540,1000,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic --until-resumed)
      # J8 (input scripts: steps on separate lines, ';' being CMake's list
      # separator): two taps on the button reach View.onClick through the input
      # dispatcher; the label reads "Clics : 2" (the "2": ink at 40,98 and
      # 43,98, background at 42,92 where a "1" has ink).
      aa_e2e(process-input CODE 0 GOLDEN process-input.txt
        FRAME 1080x1920 "540,28,#1A1B20" "40,98,#787A84" "43,98,#787A84" "42,92,#EDEDF6" "300,127,#D6D7D7"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 127\nsettle 1500\ntap 540 127\nsettle 1500\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # J8: BACK finishes the only activity (onPause, onStop, onDestroy), then
      # the process ends, no home screen to return to.
      aa_e2e(process-back CODE 0 GOLDEN process-back.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input --input "after-frames 1\nsettle 1500\nkey BACK\nsettle 3000\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # J9: the third click shows a text toast (the queue of NotificationManagerService, the
      # framework's ToastPresenter and transient_notification): its window, laid out by the
      # framework's WindowLayout, sits centered 48dp above the bottom, over the activity, in the
      # layer tree the compositor walks. Probes: the action bar, the "T" of "Trois clics !".
      aa_e2e(process-toast CODE 0 GOLDEN process-toast.txt
        FRAME 1080x1920 "540,28,#1A1B20" "509,1846,#30323A" "514,1852,#30323A"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 127\nsettle 1000\ntap 540 127\nsettle 1000\ntap 540 127\nsettle 600\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # ... and goes away after LENGTH_SHORT (2 s, 333 ms more for a text toast): its window and
      # layer are removed, the display shows the activity alone again.
      aa_e2e(process-toast-end CODE 0 GOLDEN process-toast-end.txt
        FRAME 1080x1920 "540,28,#1A1B20" "509,1846,#EDEDF6" "514,1852,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 127\nsettle 1000\ntap 540 127\nsettle 1000\ntap 540 127\nsettle 4000\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
    endif()
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
