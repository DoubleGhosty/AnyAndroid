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
      # G2: the libcore paths added on 17 October (scatter-gather I/O, MethodHandle,
      # annotations and Proxy, time zones, residual POSIX) on the real libcore. The
      # golden is the output Android gives for this code.
      aa_e2e(host-libcore CODE 0 GOLDEN host-libcore.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/hello-dex.apk aa.host.LibcoreProbe)
      # G2: libcore's sockets on Winsock (loopback only), for an application holding
      # android.permission.INTERNET; host-libcore checks the refusal without it.
      aa_e2e(host-net CODE 0 GOLDEN host-net.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/net-demo.apk aa.host.NetProbe)
      # G2: libcore's NIO channels (pipes, socket and datagram channels, the poll selector).
      aa_e2e(host-nio CODE 0 GOLDEN host-nio.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/net-demo.apk aa.host.NioProbe)
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
        FRAME 1080x1920 "540,28,#1A1B20" "48,75,#E1E1E2" "6,184,#787A84" "1000,185,#EDEDF6" "135,244,#787A84"
              "300,300,#D6D7D7" "495,332,#30323A" "540,1000,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic --until-resumed)
      # J8 (input scripts: steps on separate lines, ';' being CMake's list
      # separator): two taps on the button reach View.onClick through the input
      # dispatcher; the label reads "Clics : 2" (the "2": ink on its base at
      # 115,256, background at 124,248 where a "3" has ink).
      aa_e2e(process-input CODE 0 GOLDEN process-input.txt
        FRAME 1080x1920 "540,28,#1A1B20" "28,244,#787A84" "115,256,#787A84" "124,248,#EDEDF6" "300,300,#D6D7D7"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 332\nsettle 1500\ntap 540 332\nsettle 1500\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # J8: BACK on the root activity, started from the home screen with a main
      # intent, moves the task to the back as Android 12+ does (onPause, onStop,
      # no onDestroy: ActivityClientController.shouldMoveTaskToBack); without a
      # home screen to show, the process then ends.
      aa_e2e(process-back CODE 0 GOLDEN process-back.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input --input "after-frames 1\nsettle 1500\nkey BACK\nsettle 3000\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # J9: the third click shows a text toast (the queue of NotificationManagerService, the
      # framework's ToastPresenter and transient_notification): its window, laid out by the
      # framework's WindowLayout, sits centered 48dp above the bottom, over the activity, in the
      # layer tree the compositor walks. Probes: the action bar, the "3" of "Clics : 3", two
      # letters of "Trois clics !".
      aa_e2e(process-toast CODE 0 GOLDEN process-toast.txt
        FRAME 1080x1920 "540,28,#1A1B20" "124,248,#7B7D86" "505,1741,#30323A" "561,1737,#30323A"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 332\nsettle 1000\ntap 540 332\nsettle 1000\ntap 540 332\nsettle 600\nquit"
             ${S}/basic-activity.apk aa.host.ProcessLauncher com.anyandroid.samples.basic)
      # J9: navigation on the activity stack of the host's ActivityTaskService. A tap on
      # "Ouvrir le détail" starts DetailActivity with an extra (onPause, onCreate, onStart,
      # onResume, then onStop of MainActivity); the frame shows the detail screen.
      # Probes: the title "Détail" (no ink where "Navigation" has some), the message received,
      # the button's background where "OUVRIR LE DÉTAIL" has ink.
      aa_e2e(process-nav CODE 0 GOLDEN process-nav.txt
        FRAME 1080x1920 "540,28,#1A1B20" "48,75,#E1E1E2" "245,77,#1A1B20" "27,177,#787A84" "670,261,#D6D7D7"
              "540,1000,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input --input "after-frames 1\nsettle 1500\ntap 540 261\nsettle 3000\nquit"
             ${S}/two-screens.apk aa.host.ProcessLauncher com.anyandroid.samples.nav)
      # ... back by the button (finish()), then again by the BACK key: MainActivity
      # comes back to the front each time, DetailActivity is destroyed.
      # Probes: the main screen again (title "Navigation", no message, "OUVRIR LE DÉTAIL").
      aa_e2e(process-nav-back CODE 0 GOLDEN process-nav-back.txt
        FRAME 1080x1920 "540,28,#1A1B20" "245,77,#E1E1E2" "27,177,#EDEDF6" "670,261,#30323A" "540,1000,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 261\nsettle 3000\ntap 540 261\nsettle 3000\ntap 540 261\nsettle 3000\nkey BACK\nsettle 3000\nquit"
             ${S}/two-screens.apk aa.host.ProcessLauncher com.anyandroid.samples.nav)
      # J9: a dialog of the framework (AlertDialog, its Material layout) over the activity:
      # its window centered by the framework's WindowLayout, the Dimmer's black layer of
      # alpha 0.6 (the theme's backgroundDimAmount) below it. Probes: the action bar and the
      # window behind at 40 %, the dialog's surface, the "Q" of its title, the "I" of "OUI".
      aa_e2e(process-dialog CODE 0 GOLDEN process-dialog.txt
        FRAME 1080x1920 "540,28,#0A0A0C" "540,1500,#5F5F62" "540,1000,#EDEDF6" "139,829,#30323A"
              "918,1092,#4B5D8B"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input --input "after-frames 1\nsettle 1500\ntap 540 261\nsettle 2000\nquit"
             ${S}/dialog-demo.apk aa.host.ProcessLauncher com.anyandroid.samples.dialog)
      # ... "OUI" calls the activity's DialogInterface.OnClickListener and dismisses the dialog:
      # the label reads "Réponse : oui" (ink at 174,175 where "aucune" has none, none at 254,172
      # where it has), no dim, no dialog.
      aa_e2e(process-dialog-answer CODE 0 GOLDEN process-dialog-answer.txt
        FRAME 1080x1920 "540,28,#1A1B20" "540,1500,#EDEDF6" "174,175,#787A84" "254,172,#EDEDF6"
              "540,1000,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 261\nsettle 2000\ntap 894 1093\nsettle 2000\nquit"
             ${S}/dialog-demo.apk aa.host.ProcessLauncher com.anyandroid.samples.dialog)
      # ... BACK goes to the dialog, which has the focus, and cancels it (Dialog.onKeyUp; its
      # window is gone before the key's release is finished, which InputDispatcher then drops);
      # the focus is back on the activity: the next BACK moves its task to the back.
      aa_e2e(process-dialog-back CODE 0 GOLDEN process-dialog-back.txt
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 261\nsettle 2000\nkey BACK\nsettle 2000\nkey BACK\nsettle 3000\nquit"
             ${S}/dialog-demo.apk aa.host.ProcessLauncher com.anyandroid.samples.dialog)
      # ... and goes away after LENGTH_SHORT (2 s, 333 ms more for a text toast): its window and
      # layer are removed, the display shows the activity alone again.
      aa_e2e(process-toast-end CODE 0 GOLDEN process-toast-end.txt
        FRAME 1080x1920 "540,28,#1A1B20" "124,248,#7B7D86" "505,1741,#EDEDF6" "561,1737,#EDEDF6"
        ARGS exec --bootclasspath ${AA_LIBCORE_DIR} --system-image ${AA_SYSTEM_IMAGE} --host-dex ${AA_HOST_DEX}
             --trace-input
             --input "after-frames 1\nsettle 1500\ntap 540 332\nsettle 1000\ntap 540 332\nsettle 1000\ntap 540 332\nsettle 4000\nquit"
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

# Under wine, the first wine program of a session starts wineserver and wine's
# helper processes (services.exe, winedevice.exe, plugplay...), which inherit
# that program's standard output and error and live as long as the server:
# CTest then waits for the first test's pipes until the server ends, or until
# the test's timeout in a full run (the test's own process has ended). The
# fixture starts the server and the helpers first, detached from every test's
# pipes. A server already running is kept if it persists (-p30: an earlier run's
# fixture); another one (the build's apkgen's, say, about to exit) is waited
# for first (bounded: one kept alive by unrelated wine programs is not the
# tests' to wait for).
if(CMAKE_CROSSCOMPILING_EMULATOR)
  list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 aa_wine)
  get_filename_component(aa_emulator ${aa_wine} NAME)
  find_program(AA_WINESERVER wineserver)
  if(aa_emulator MATCHES "^wine" AND AA_WINESERVER)
    add_test(NAME wineserver COMMAND sh -c "exec </dev/null >/dev/null 2>&1; '${AA_WINESERVER}' -p30 || pgrep -a wineserver | grep -q -- ' -p30' || { timeout 30 '${AA_WINESERVER}' -w; '${AA_WINESERVER}' -p30; }; '${aa_wine}' cmd /c exit; exit 0")
    set_tests_properties(wineserver PROPERTIES FIXTURES_SETUP wineserver)
    get_property(aa_tests DIRECTORY PROPERTY TESTS)
    list(REMOVE_ITEM aa_tests wineserver)
    set_tests_properties(${aa_tests} PROPERTIES FIXTURES_REQUIRED wineserver)
  endif()
endif()
