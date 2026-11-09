# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Instagram Post add-on (docs/design/30). Included by the Windows root and
# cmake/darwin.cmake.
#
#   mv_instagram_engine   the post, its crops and the window's trees, ABI-free;
#                         the unit tests link it.
#   mv_instagram          the add-on: ONE shared library whose only export is
#                         mv_addon_get, linking nothing of the core.
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/instagram, signed.

set(MV_INSTAGRAM_VERSION "1.0.0" CACHE STRING "Instagram Post add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_IG "${MV_SOURCE_ROOT}/src/addons/instagram")

add_library(mv_instagram_engine STATIC
  ${_IG}/engine.cpp
  ${_IG}/engine.h
  ${_IG}/pixels.cpp
  ${_IG}/pixels.h
  ${_IG}/post.cpp
  ${_IG}/post.h
  ${_IG}/view.cpp
  ${_IG}/view.h
)
target_include_directories(mv_instagram_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
find_package(Threads REQUIRED)
target_link_libraries(mv_instagram_engine PUBLIC mv_project_options Threads::Threads)
set_target_properties(mv_instagram_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_library(mv_instagram MODULE ${_IG}/addon_entry.cpp)
target_link_libraries(mv_instagram PRIVATE mv_instagram_engine)
target_compile_definitions(mv_instagram PRIVATE MV_INSTAGRAM_VERSION="${MV_INSTAGRAM_VERSION}")
set_target_properties(mv_instagram PROPERTIES
  OUTPUT_NAME "mv_instagram"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/instagram"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/instagram")
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${_cfg} _CFG)
  set_target_properties(mv_instagram PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/instagram"
    RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/addons/instagram")
endforeach()
if(APPLE)
  set_target_properties(mv_instagram PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_instagram PROPERTIES PREFIX "")
endif()

# The pure parts and the engine over a fake host, plus the manifest and the
# window's tree through the real checks. Called by each build's mv_import_tests.
function(mv_instagram_tests target)
  target_sources(${target} PRIVATE "${MV_SOURCE_ROOT}/tests/test_instagram.cpp")
  target_link_libraries(${target} PRIVATE mv_instagram_engine)
  target_compile_definitions(${target} PRIVATE
    MV_INSTAGRAM_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/instagram/manifest.json")
  add_dependencies(${target} mv_instagram)
endfunction()
