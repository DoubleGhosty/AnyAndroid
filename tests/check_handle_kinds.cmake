# Checks that the HandleTables of the runtime have distinct kind tags
# (framework-bridge/handles.h): two tables sharing a tag would accept each
# other's handles.
#
#   cmake -DROOT=<source dir> -P check_handle_kinds.cmake

set(decl "HandleTable<[^>]+>[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[({][ \t]*(0[xX][0-9a-fA-F]+)")
file(GLOB_RECURSE sources ${ROOT}/runtime/*.cpp ${ROOT}/runtime/*.h)
set(count 0)
foreach(f ${sources})
  file(STRINGS ${f} lines REGEX "${decl}")
  file(RELATIVE_PATH rel ${ROOT} ${f})
  foreach(line ${lines})
    if(NOT line MATCHES "${decl}")
      continue()  # the rest of a line split at a ';'
    endif()
    math(EXPR tag "${CMAKE_MATCH_1} & 0x7fff" OUTPUT_FORMAT HEXADECIMAL)  # as HandleTable's constructor
    if(DEFINED owner_${tag})
      message(SEND_ERROR "étiquette de poignée ${tag} utilisée deux fois : ${owner_${tag}} et ${rel}")
    endif()
    set(owner_${tag} ${rel})
    math(EXPR count "${count} + 1")
  endforeach()
endforeach()
if(count EQUAL 0)
  message(FATAL_ERROR "aucune HandleTable trouvée sous ${ROOT}/runtime")
endif()
message(STATUS "${count} tables de poignées")
