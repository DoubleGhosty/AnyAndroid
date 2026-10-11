# javac (stubs, then host sources against them) and D8 on the host classes
# only; see CMakeLists.txt.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/stubs" "${WORK}/classes" "${WORK}/dex")

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "échec (${code}) : ${ARGN}\n${out}\n${err}")
  endif()
endfunction()

set(javac_flags --release 17 -encoding UTF-8 -Xlint:-options -nowarn)
run("${JAVAC}" ${javac_flags} -d "${WORK}/stubs" "@${STUBS_ARGS}")
run("${JAVAC}" ${javac_flags} -cp "${WORK}/stubs" -d "${WORK}/classes" "@${SOURCES_ARGS}")
file(GLOB_RECURSE classes "${WORK}/classes/*.class")
run("${JAVA}" -cp "${R8}" com.android.tools.r8.D8 --release --min-api 33 --classpath "${WORK}/stubs"
    --output "${WORK}/dex" ${classes})
file(COPY_FILE "${WORK}/dex/classes.dex" "${OUT}")
