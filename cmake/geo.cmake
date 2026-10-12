# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/design/06 "Place": the offline reverse geocoder. Included by the Windows
# root and cmake/darwin.cmake after mv_io.
#
#   mv_geo        src/geo: the database over bytes it is given, lookups, name
#                 matching, fold. Portable C++ over core's header-only pieces
#                 (result.h, status.h) and nothing else, position-independent,
#                 so the Places add-on can link it while linking nothing of the
#                 core (docs/design/18 "add-ons link nothing of the core").
#   mv_geo_open   src/geo/open.cpp: open a blob by path through io/mapped_file
#                 (tests, tools, and any in-process host).
#   mv_geo_data   Not built by default. Runs tools/geo/build_places.py to make
#                 ${MV_GEO_BLOB} from pinned downloads cached in
#                 ${MV_GEO_DOWNLOADS}. The first run downloads ~66 MB and takes
#                 about a minute; later runs reuse the cache. -DMV_GEO_DATA=ON
#                 adds it to ALL; packaging runs the script itself.
#
# Expects: mv_project_options, mv_io.

if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_library(mv_geo STATIC
  ${MV_SOURCE_ROOT}/src/geo/geo.cpp
  ${MV_SOURCE_ROOT}/src/geo/geo.h
  ${MV_SOURCE_ROOT}/src/geo/blob.h
  ${MV_SOURCE_ROOT}/src/geo/fold.cpp
  ${MV_SOURCE_ROOT}/src/geo/fold.h
  ${MV_SOURCE_ROOT}/src/geo/fold_table.inc
)
target_include_directories(mv_geo PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_geo PUBLIC mv_project_options)
# Hidden: linked into the Places module, whose only export is mv_addon_get.
set_target_properties(mv_geo PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
add_library(mv::geo ALIAS mv_geo)

add_library(mv_geo_open STATIC
  ${MV_SOURCE_ROOT}/src/geo/open.cpp
  ${MV_SOURCE_ROOT}/src/geo/open.h
)
target_link_libraries(mv_geo_open PUBLIC mv_geo mv_io)

option(MV_GEO_DATA "Build the full place database (downloads pinned Natural Earth + GeoNames data once)" OFF)
set(MV_GEO_DOWNLOADS "${CMAKE_BINARY_DIR}/geo-downloads" CACHE PATH
    "Where build_places.py keeps its pinned downloads (outside the source tree)")
set(MV_GEO_BLOB "${CMAKE_BINARY_DIR}/geo/places.mvgeo")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(_geo_all "")
  if(MV_GEO_DATA)
    set(_geo_all ALL)
  endif()
  add_custom_command(
    OUTPUT "${MV_GEO_BLOB}"
    COMMAND Python3::Interpreter -I "${MV_SOURCE_ROOT}/tools/geo/build_places.py"
            --downloads "${MV_GEO_DOWNLOADS}" --out "${MV_GEO_BLOB}"
    DEPENDS "${MV_SOURCE_ROOT}/tools/geo/build_places.py"
    COMMENT "Building the place database (docs/design/06 Place)"
    VERBATIM)
  add_custom_target(mv_geo_data ${_geo_all} DEPENDS "${MV_GEO_BLOB}")
endif()
