# build_id.cmake — exécuté à CHAQUE compilation (cible picow_build_id) :
# écrit OUT (build_id.h) avec PICOW_MODEM_BUILD = `git describe` du dépôt
# (vX.Y.Z pour une release taguée, vX.Y.Z-N-gSHA[-dirty] sinon), et ne le
# réécrit que s'il change, pour ne pas tout recompiler.
#   cmake -DSRC=<racine du dépôt> -DOUT=<fichier> -P build_id.cmake
execute_process(
    COMMAND git describe --tags --always --dirty --match "v[0-9]*"
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR id STREQUAL "")
    set(id "unknown")
endif()
set(content "/* Généré par cmake/build_id.cmake — ne pas éditer. */\n#define PICOW_MODEM_BUILD \"${id}\"\n")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT "${old}" STREQUAL "${content}")
    file(WRITE ${OUT} "${content}")
endif()
