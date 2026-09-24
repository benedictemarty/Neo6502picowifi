# build_id.cmake — exécuté à CHAQUE compilation (cible picow_build_id) :
# écrit OUT (build_id.h) avec PICOW_MODEM_BUILD = `git describe` du dépôt
# (vX.Y.Z pour une release taguée, vX.Y.Z-N-gSHA[-dirty] sinon), et ne le
# réécrit que s'il change, pour ne pas tout recompiler. PICOW_MODEM_DATE =
# date du commit en UTC (US-W5 : pas de __DATE__/__TIME__, compilation
# reproductible — même commit, même UF2).
#   cmake -DSRC=<racine du dépôt> -DOUT=<fichier> -P build_id.cmake
execute_process(
    COMMAND git describe --tags --always --dirty --match "v[0-9]*"
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR id STREQUAL "")
    set(id "unknown")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env TZ=UTC LC_ALL=C git log -1 --format=%cd "--date=format-local:%b %d %Y %H:%M:%S"
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR date STREQUAL "")
    set(date "unknown")
else()
    set(date "${date} UTC")
endif()
set(content "/* Généré par cmake/build_id.cmake — ne pas éditer. */\n#define PICOW_MODEM_BUILD \"${id}\"\n#define PICOW_MODEM_DATE \"${date}\"\n")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT "${old}" STREQUAL "${content}")
    file(WRITE ${OUT} "${content}")
endif()
