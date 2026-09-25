# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone G (plan/18-import.md): the add-on host (src/addon) and the Import
# add-on (src/addons/import). Included by the Windows root, cmake/darwin.cmake
# and the headless cmake/portable build, so the three cannot drift.
#
# Expects, already defined by the includer:
#   mv_project_options   warnings and language settings
#   mv_io                with io/content_hash, file_port, verified_copy, volume
#   MV_SQLITE_TARGET     SQLite (import.db)
#   MV_SODIUM_TARGET     libsodium (Ed25519 manifest signatures, SHA-256)
#
# mv_addon is the host side and lives in the base app: manifest checks,
# install / remove, the loader and the host function table. It knows no
# add-on by name.
#
# mv_import is the add-on: ONE shared library whose only export is
# mv_addon_get. It links SQLite and nothing of the core -- the core's io,
# metadata, pairing and volume ports reach it only through the host function
# table (plan/18 "does not link the core statically"). It includes a few of
# the core's header-only pieces (result.h, status.h, json.h). It is NOT
# installed with the app: tools/package/addon-pack.py packs it separately,
# signed, and the base install tree is byte-identical without it (PR 16 verify).

set(MV_IMPORT_VERSION "1.0.0" CACHE STRING "Import add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(R "${MV_SOURCE_ROOT}")

if(WIN32)
  set(MV_ADDON_LOADER ${R}/src/addon/loader_win.cpp)
else()
  set(MV_ADDON_LOADER ${R}/src/addon/loader_mac.cpp)
endif()

add_library(mv_addon STATIC
  ${R}/src/addon/manifest.cpp
  ${R}/src/addon/manifest.h
  ${R}/src/addon/store.cpp
  ${R}/src/addon/store.h
  ${R}/src/addon/host.cpp
  ${R}/src/addon/host.h
  ${MV_ADDON_LOADER}
  ${R}/src/abi/include/mediaviewer/mediaviewer_addon.h
)
target_include_directories(mv_addon PUBLIC "${R}/src" "${R}/src/abi/include")
target_link_libraries(mv_addon PUBLIC mv_io PRIVATE ${MV_SODIUM_TARGET})
if(NOT WIN32)
  target_link_libraries(mv_addon PRIVATE ${CMAKE_DL_LIBS})
endif()
add_library(mv::addon ALIAS mv_addon)

# The engine as a static library, so tests drive it without dlopen; the
# add-on module is this plus addon_entry.cpp.
add_library(mv_import_engine STATIC
  ${R}/src/addons/import/model.cpp
  ${R}/src/addons/import/model.h
  ${R}/src/addons/import/naming.cpp
  ${R}/src/addons/import/naming.h
  ${R}/src/addons/import/host.cpp
  ${R}/src/addons/import/host.h
  ${R}/src/addons/import/library_index.cpp
  ${R}/src/addons/import/library_index.h
  ${R}/src/addons/import/scanner.cpp
  ${R}/src/addons/import/scanner.h
  ${R}/src/addons/import/planner.cpp
  ${R}/src/addons/import/planner.h
  ${R}/src/addons/import/engine.cpp
  ${R}/src/addons/import/engine.h
  ${R}/src/addons/import/paths.h
  ${R}/src/abi/include/mediaviewer/mediaviewer_import.h
)
target_include_directories(mv_import_engine PUBLIC "${R}/src" "${R}/src/abi/include")
target_link_libraries(mv_import_engine PUBLIC mv_project_options PRIVATE ${MV_SQLITE_TARGET})
find_package(Threads REQUIRED)
target_link_libraries(mv_import_engine PUBLIC Threads::Threads)
set_target_properties(mv_import_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_library(mv_import MODULE "${R}/src/addons/import/addon_entry.cpp")
target_link_libraries(mv_import PRIVATE mv_import_engine)
target_compile_definitions(mv_import PRIVATE MV_IMPORT_VERSION="${MV_IMPORT_VERSION}")
set_target_properties(mv_import PROPERTIES
  OUTPUT_NAME "mv_import"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  # Built beside the app for tests and packing, never installed with it.
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/import"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/import")
if(APPLE)
  # plan/18: libmv_import.dylib, loaded under library validation.
  set_target_properties(mv_import PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_import PROPERTIES PREFIX "")
endif()
