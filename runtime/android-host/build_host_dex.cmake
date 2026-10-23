# javac (stubs, then host sources against them) and D8 on the host classes
# only; see CMakeLists.txt. gen/IndySitesGen.java first writes IndySites.class
# (invokedynamic and method handle constants, which javac does not write).
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/stubs" "${WORK}/classes" "${WORK}/dex" "${WORK}/gen-tool" "${WORK}/gen")

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "échec (${code}) : ${ARGN}\n${out}\n${err}")
  endif()
endfunction()

set(javac_flags --release 17 -encoding UTF-8 -Xlint:-options -nowarn)
run("${JAVAC}" ${javac_flags} -d "${WORK}/stubs" "@${STUBS_ARGS}")
run("${JAVAC}" ${javac_flags} -d "${WORK}/gen-tool" "${GEN_SOURCE}")
run("${JAVA}" -cp "${WORK}/gen-tool" IndySitesGen "${WORK}/gen")
# The host sources see the stubs and the generated class (an argument file:
# the path separator, ';' on Windows, would split a CMake argument).
if(CMAKE_HOST_WIN32)
  set(sep ";")
else()
  set(sep ":")
endif()
file(WRITE "${WORK}/classpath.args" "-cp\n\"${WORK}/stubs${sep}${WORK}/gen\"\n")
run("${JAVAC}" ${javac_flags} "@${WORK}/classpath.args" -d "${WORK}/classes" "@${SOURCES_ARGS}")
file(GLOB_RECURSE classes "${WORK}/classes/*.class" "${WORK}/gen/*.class")
run("${JAVA}" -cp "${R8}" com.android.tools.r8.D8 --release --min-api 33 --classpath "${WORK}/stubs"
    --output "${WORK}/dex" ${classes})
file(COPY_FILE "${WORK}/dex/classes.dex" "${OUT}")
