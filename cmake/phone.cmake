# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Phone access add-on (docs/plans/phone-access-addon.md, owner 2026-11-14):
# Phone access out of the base app, as before it the app took it from Local
# search and Driftwire (docs/plans/phone-in-the-app.md).
#
#   mv_phone         the feature (src/addons/phone): the desk that claims and
#                    manages `driftwire serve` and serves nearby (desk.*), the
#                    state machine behind the card (link.*), the process's one
#                    link (service.*), this computer as the server
#                    (self_host.*), the card as a view tree (screens.*) and its
#                    QR (qr.*). Static: the module and the tests link it. No
#                    mv_io: the module compiles in the io pieces it uses, the
#                    tests link mv_io.
#   mv_phone_access  the add-on, libmv_phone_access.dylib / mv_phone_access.dll
#                    in build/addons/phone-access, packed and signed separately
#                    (tools/package/addon-pack.py); never in the app.
#
# Portable (D9): over mv_wire (the protocol, its OS ports) and mv_catalogue.
# Included by the app builds (the Windows root, cmake/darwin.cmake,
# cmake/portable) after cmake/driftwire.cmake.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, MV_BLAKE3_TARGET, Threads.

if(TARGET mv_phone)
  return()
endif()
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/wire.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/catalogue.cmake")
set(_P "${MV_SOURCE_ROOT}/src/addons/phone")
set(MV_PHONE_ACCESS_VERSION "1.0.0" CACHE STRING "Phone access add-on version (its manifest's)")

if(WIN32)
  set(_phone_dir ${_P}/feature_dir_win.cpp)
else()
  set(_phone_dir ${_P}/feature_dir_posix.cpp)
endif()

add_library(mv_phone STATIC
  ${_P}/desk.cpp ${_P}/desk.h
  ${_P}/link.cpp ${_P}/link.h
  ${_P}/service.cpp ${_P}/service.h
  ${_P}/self_host.cpp ${_P}/self_host.h
  ${_P}/screens.cpp ${_P}/screens.h
  ${_P}/qr.cpp ${_P}/qr.h
  ${_P}/bundled.cpp ${_P}/bundled.h
  ${_phone_dir}
)
target_include_directories(mv_phone PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_phone PUBLIC mv_project_options mv_wire mv_catalogue Threads::Threads
  PRIVATE ${MV_SODIUM_TARGET})
set_target_properties(mv_phone PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(IOS)
  return()  # no add-ons on iOS
endif()

if(WIN32)
  set(_phone_io ${MV_SOURCE_ROOT}/src/io/file_port_win.cpp ${MV_SOURCE_ROOT}/src/io/child_process_win.cpp)
else()
  set(_phone_io ${MV_SOURCE_ROOT}/src/io/file_port_mac.cpp ${MV_SOURCE_ROOT}/src/io/child_process_posix.cpp)
endif()
add_library(mv_phone_access MODULE
  ${_P}/addon_entry.cpp
  # The file port (the nearby copy's server reads through it) and running the
  # driftwire program, compiled in: the add-on links nothing of the core.
  ${MV_SOURCE_ROOT}/src/io/file_port.cpp
  ${MV_SOURCE_ROOT}/src/io/content_hash.cpp
  ${_phone_io}
)
target_link_libraries(mv_phone_access PRIVATE mv_phone ${MV_BLAKE3_TARGET})
if(WIN32)
  target_link_libraries(mv_phone_access PRIVATE shell32)
elseif(APPLE)
  target_link_libraries(mv_phone_access PRIVATE "-framework Foundation")
endif()
target_compile_definitions(mv_phone_access PRIVATE MV_PHONE_ACCESS_VERSION="${MV_PHONE_ACCESS_VERSION}")
set_target_properties(mv_phone_access PROPERTIES
  OUTPUT_NAME "mv_phone_access"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/phone-access"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/phone-access")
if(APPLE)
  set_target_properties(mv_phone_access PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_phone_access PROPERTIES PREFIX "")
endif()
