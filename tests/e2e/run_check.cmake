# Runs one command and checks its exit code and standard output.
#
#   cmake -DCMD=<exe|arg|arg...> -DEXPECT_CODE=<n>
#         [-DGOLDEN=<fichier>] [-DMATCH=<regex>] [-DACTUAL=<fichier>]
#         [-DFRESH_DIR=<dossier>] -P run_check.cmake
#
# Arguments are separated by '|' so that they survive CTest's list handling.
# FRESH_DIR (the --data-dir of the command) is emptied before the run.

string(REPLACE "|" ";" command "${CMD}")
if(DEFINED FRESH_DIR)
  file(REMOVE_RECURSE "${FRESH_DIR}")
  file(MAKE_DIRECTORY "${FRESH_DIR}")
endif()
# anyandroid always writes UTF-8: decode it as such, whatever the code page
# of the console running CTest.
execute_process(COMMAND ${command}
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  ENCODING UTF8
  TIMEOUT 60)
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
