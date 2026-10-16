# Runs one command and checks its exit code and standard output.
#
#   cmake -DCMD=<exe|arg|arg...> -DEXPECT_CODE=<n>
#         [-DGOLDEN=<fichier>] [-DMATCH=<regex>] [-DACTUAL=<fichier>]
#         [-DFRESH_DIR=<dossier>] [-DFRAME=<fichier.png> -DFRAME_SIZE=<l>x<h>]
#         -P run_check.cmake
#
# Arguments are separated by '|' so that they survive CTest's list handling.
# FRESH_DIR (the --data-dir of the command) is emptied before the run. FRAME
# (the --frame-out of the command) must be a PNG of FRAME_SIZE afterwards.

string(REPLACE "|" ";" command "${CMD}")
# Programs on AOSP's real libcore (FRESH_DIR set) load the framework from the
# system image: allow them more time, especially under a parallel ctest.
if(DEFINED FRESH_DIR)
  set(timeout 300)
else()
  set(timeout 60)
endif()

if(DEFINED FRESH_DIR)
  file(REMOVE_RECURSE "${FRESH_DIR}")
  file(MAKE_DIRECTORY "${FRESH_DIR}")
endif()
if(DEFINED FRAME)
  file(REMOVE "${FRAME}")
endif()
# anyandroid always writes UTF-8: decode it as such, whatever the code page
# of the console running CTest.
execute_process(COMMAND ${command}
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  ENCODING UTF8
  TIMEOUT ${timeout})
string(REPLACE "\r\n" "\n" out "${out}")

if(DEFINED ACTUAL)
  file(WRITE "${ACTUAL}" "${out}")
endif()

if(NOT "${code}" STREQUAL "${EXPECT_CODE}")
  message(FATAL_ERROR "code de sortie ${code} (attendu ${EXPECT_CODE})\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(DEFINED GOLDEN)
  file(READ "${GOLDEN}" expected)
  string(REPLACE "\r\n" "\n" expected "${expected}")
  if(NOT "${out}" STREQUAL "${expected}")
    message(FATAL_ERROR "sortie différente de ${GOLDEN}\n--- obtenue ---\n${out}\n--- stderr ---\n${err}")
  endif()
endif()

if(DEFINED MATCH AND NOT "${out}" MATCHES "${MATCH}")
  message(FATAL_ERROR "la sortie ne contient pas « ${MATCH} »\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(DEFINED FRAME)
  if(NOT EXISTS "${FRAME}")
    message(FATAL_ERROR "aucune image écrite dans ${FRAME}\n--- stderr ---\n${err}")
  endif()
  # PNG signature, then the IHDR chunk: width and height, big-endian.
  file(READ "${FRAME}" head LIMIT 24 HEX)
  string(SUBSTRING "${head}" 0 16 signature)
  string(SUBSTRING "${head}" 32 8 width_hex)
  string(SUBSTRING "${head}" 40 8 height_hex)
  math(EXPR width "0x${width_hex}")
  math(EXPR height "0x${height_hex}")
  if(NOT signature STREQUAL "89504e470d0a1a0a" OR NOT "${width}x${height}" STREQUAL "${FRAME_SIZE}")
    message(FATAL_ERROR "${FRAME} : PNG ${width}x${height} (attendu ${FRAME_SIZE}), signature ${signature}")
  endif()
endif()
