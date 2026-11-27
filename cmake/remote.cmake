# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Another desktop opens the host's drives (docs/plans/desktop-client.md, #686).
#
#   mv_mobile_route  the phone's link to `driftwire serve` (src/mobile/link.*, the
#                    route race, reconnect, range reads) and its pairing code
#                    (link_payload.*): one copy of the code, an OBJECT library whose
#                    objects go into the phone's libmv_mobile_link.a (which the iOS
#                    project links by path, unchanged) and into the desktop's mv_remote.
#   mv_remote        the paired servers, their folders as `remote:` keys, tiles
#                    and the originals cache (src/remote). Portable (D9).
#
# Included by the app builds (the Windows root, cmake/darwin.cmake) after
# cmake/wire.cmake, and by cmake/mobile-link.cmake.
# Expects: mv_project_options, MV_SODIUM_TARGET, Threads, mv_io.

if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/wire.cmake")

if(NOT TARGET mv_mobile_route)
  set(_M "${MV_SOURCE_ROOT}/src/mobile")
  add_library(mv_mobile_route OBJECT
    ${_M}/link.cpp ${_M}/link.h
    ${_M}/link_payload.cpp ${_M}/link_payload.h)
  target_include_directories(mv_mobile_route PUBLIC "${MV_SOURCE_ROOT}/src")
  target_compile_features(mv_mobile_route PUBLIC cxx_std_20)
  target_link_libraries(mv_mobile_route PUBLIC mv_project_options mv_wire PRIVATE ${MV_SODIUM_TARGET})
  set_target_properties(mv_mobile_route PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# iOS builds only the route library (through mobile-link.cmake): no mv_io there.
if(NOT TARGET mv_remote AND TARGET mv_io)
  set(_R "${MV_SOURCE_ROOT}/src/remote")
  add_library(mv_remote STATIC
    $<TARGET_OBJECTS:mv_mobile_route>
    ${_R}/key.cpp ${_R}/key.h
    ${_R}/cache.cpp ${_R}/cache.h
    ${_R}/library.cpp ${_R}/library.h
    ${_R}/lan_share.cpp ${_R}/lan_share.h)
  target_include_directories(mv_remote PUBLIC "${MV_SOURCE_ROOT}/src")
  target_link_libraries(mv_remote PUBLIC mv_project_options mv_wire Threads::Threads
    PRIVATE ${MV_SODIUM_TARGET} mv_io)
  set_target_properties(mv_remote PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endif()
