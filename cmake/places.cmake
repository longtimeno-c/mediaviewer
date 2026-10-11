# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Places add-on (docs/design/27 "Delivery: the Places add-on"; plan slice 3).
# Included by the Windows root and cmake/darwin.cmake after geo.cmake and
# import.cmake, so the two cannot drift.
#
#   mv_places_engine   the section, the map rasteriser and search by place,
#                      over mv_geo. ABI-free; the unit tests link it.
#   mv_places          the add-on: ONE shared library whose only export is
#                      mv_addon_get. It links mv_geo (its own code, D9-portable)
#                      and nothing of the core: the core reaches it only through
#                      the host function table (v3: screens and slots). It maps
#                      its data itself (src/geo/open.cpp over io/mapped_file,
#                      compiled in), from data/places.mvgeo beside it.
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/places, signed, with the
# place database the staging step builds (tools/package/places-stage.py).
#
# Expects: mv_project_options, mv_geo (cmake/geo.cmake), Threads.

set(MV_PLACES_VERSION "1.0.0" CACHE STRING "Places add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_P "${MV_SOURCE_ROOT}/src/addons/places")

add_library(mv_places_engine STATIC
  ${_P}/map.cpp
  ${_P}/map.h
  ${_P}/places.cpp
  ${_P}/places.h
)
target_include_directories(mv_places_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_places_engine PUBLIC mv_project_options mv_geo)
set_target_properties(mv_places_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  set(_places_port ${_P}/self_dir_win.cpp ${MV_SOURCE_ROOT}/src/io/mapped_file_win.cpp)
else()
  set(_places_port ${_P}/self_dir_posix.cpp ${MV_SOURCE_ROOT}/src/io/mapped_file_posix.cpp)
endif()

add_library(mv_places MODULE
  ${_P}/addon_entry.cpp
  ${_P}/self_dir.h
  ${MV_SOURCE_ROOT}/src/geo/open.cpp
  ${_places_port}
)
target_link_libraries(mv_places PRIVATE mv_places_engine)
target_compile_definitions(mv_places PRIVATE MV_PLACES_VERSION="${MV_PLACES_VERSION}")
if(NOT WIN32)
  target_link_libraries(mv_places PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(mv_places PROPERTIES
  OUTPUT_NAME "mv_places"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/places"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/places")
if(APPLE)
  set_target_properties(mv_places PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_places PROPERTIES PREFIX "")
endif()
