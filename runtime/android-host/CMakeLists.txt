# AnyAndroid's host code (ADR-006): Java compiled by javac then D8 into
# anyandroid-host.dex, placed next to the CLI. The declarations of
# stubs/ are only compiled against: the classes come from the real
# framework.jar at run time.
file(GLOB_RECURSE aa_host_stubs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/stubs/*.java)
file(GLOB_RECURSE aa_host_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/java/*.java)

set(work ${CMAKE_CURRENT_BINARY_DIR}/work)
string(REPLACE ";" "\n" stubs_lines "${aa_host_stubs}")
string(REPLACE ";" "\n" sources_lines "${aa_host_sources}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stubs.args "${stubs_lines}\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sources.args "${sources_lines}\n")

set(AA_HOST_DEX ${CMAKE_BINARY_DIR}/apps/cli/anyandroid-host.dex CACHE INTERNAL "DEX d'hôte (ADR-006)")
add_custom_command(OUTPUT ${AA_HOST_DEX}
  COMMAND ${CMAKE_COMMAND}
    -DJAVAC=${Java_JAVAC_EXECUTABLE} -DJAVA=${Java_JAVA_EXECUTABLE} -DR8=${AA_R8_JAR}
    -DSTUBS_ARGS=${CMAKE_CURRENT_BINARY_DIR}/stubs.args -DSOURCES_ARGS=${CMAKE_CURRENT_BINARY_DIR}/sources.args
    -DWORK=${work} -DOUT=${AA_HOST_DEX} -DGEN_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/gen/IndySitesGen.java
    -P ${CMAKE_CURRENT_SOURCE_DIR}/build_host_dex.cmake
  DEPENDS ${aa_host_stubs} ${aa_host_sources} ${CMAKE_CURRENT_SOURCE_DIR}/build_host_dex.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/gen/IndySitesGen.java
  COMMENT "javac + D8 : anyandroid-host.dex"
  VERBATIM)
add_custom_target(anyandroid_host_dex ALL DEPENDS ${AA_HOST_DEX})
