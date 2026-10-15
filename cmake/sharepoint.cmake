# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Sharepoint add-on (docs/design/28): send files to a MediaViewer nearby.
# Included by the Windows root and cmake/darwin.cmake after import.cmake and
# places.cmake, so the two cannot drift.
#
#   mv_sharepoint_engine  the wire, the sealed channel, the transfer, the
#                         engine and its view tree, and the network and
#                         Bluetooth ports. Links libsodium and BLAKE3; not the
#                         core's io (the tests bring mv_io, the module compiles
#                         the file port in).
#   mv_sharepoint         the add-on: ONE shared library whose only export is
#                         mv_addon_get. It links nothing of the core: the core
#                         reaches it only through the host function table (v3:
#                         screens and slots).
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/sharepoint, signed.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, BLAKE3::blake3, Threads.

set(MV_SHAREPOINT_VERSION "1.0.0" CACHE STRING "Sharepoint add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_S "${MV_SOURCE_ROOT}/src/addons/sharepoint")

# The Bluetooth bearer goes into the module only: the tests run the engine
# over loopback and link bt_none.cpp (one Objective-C class per process).
if(WIN32)
  set(_sharepoint_port ${_S}/net_win.cpp)
  set(_sharepoint_bt ${_S}/bt_win.cpp)
elseif(APPLE)
  set(_sharepoint_port ${_S}/net_posix.cpp ${_S}/machine_mac.cpp)
  set(_sharepoint_bt ${_S}/bt_mac.mm)
else()
  set(_sharepoint_port ${_S}/net_posix.cpp)
  set(_sharepoint_bt ${_S}/bt_none.cpp)
endif()

add_library(mv_sharepoint_engine STATIC
  ${_S}/wire.cpp ${_S}/wire.h
  ${_S}/link.h
  ${_S}/channel.cpp ${_S}/channel.h
  ${_S}/transfer.cpp ${_S}/transfer.h
  ${_S}/presence.cpp ${_S}/presence.h
  ${_S}/bt_pipe.cpp ${_S}/bt_pipe.h
  ${_S}/net.h
  ${_S}/engine.cpp ${_S}/engine.h
  ${_S}/tree.cpp
  ${_sharepoint_port}
)
target_include_directories(mv_sharepoint_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_sharepoint_engine PUBLIC mv_project_options ${MV_SODIUM_TARGET} BLAKE3::blake3
  Threads::Threads)
set_target_properties(mv_sharepoint_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  target_link_libraries(mv_sharepoint_engine PUBLIC ws2_32)
elseif(APPLE)
  target_link_libraries(mv_sharepoint_engine PUBLIC "-framework CoreFoundation" "-framework SystemConfiguration")
endif()

if(WIN32)
  set(_sharepoint_io ${MV_SOURCE_ROOT}/src/io/file_port_win.cpp)
else()
  set(_sharepoint_io ${MV_SOURCE_ROOT}/src/io/file_port_mac.cpp)
endif()

add_library(mv_sharepoint MODULE
  ${_S}/addon_entry.cpp
  ${_sharepoint_bt}
  # The file port and BLAKE3 hashing, compiled in: the add-on links nothing
  # of the core (as Places carries io/mapped_file).
  ${MV_SOURCE_ROOT}/src/io/file_port.cpp
  ${MV_SOURCE_ROOT}/src/io/content_hash.cpp
  ${_sharepoint_io}
)
target_link_libraries(mv_sharepoint PRIVATE mv_sharepoint_engine)
if(WIN32)
  # C++/WinRT for the Bluetooth bearer (Windows.Devices.Bluetooth,
  # windowsapp.lib). Its projection throws; bt_win.cpp catches every call.
  target_link_libraries(mv_sharepoint PRIVATE windowsapp ole32)
  set_source_files_properties(${_S}/bt_win.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
elseif(APPLE)
  target_link_libraries(mv_sharepoint PRIVATE "-framework Foundation" "-framework CoreBluetooth")
  set_source_files_properties(${_S}/bt_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
target_compile_definitions(mv_sharepoint PRIVATE MV_SHAREPOINT_VERSION="${MV_SHAREPOINT_VERSION}")
set_target_properties(mv_sharepoint PROPERTIES
  OUTPUT_NAME "mv_sharepoint"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/sharepoint"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/sharepoint")
if(APPLE)
  set_target_properties(mv_sharepoint PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_sharepoint PROPERTIES PREFIX "")
endif()

# The engine over loopback TCP and an in-memory discovery bus, and the add-on
# itself through the real loader (tests/test_sharepoint.cpp,
# tests/test_sharepoint_addon.cpp). Called by each build's mv_import_tests
# block, which links mv_io (the file port) and libsodium.
function(mv_sharepoint_tests target)
  target_sources(${target} PRIVATE
    "${MV_SOURCE_ROOT}/tests/test_sharepoint.cpp"
    "${MV_SOURCE_ROOT}/tests/test_sharepoint_addon.cpp"
    "${MV_SOURCE_ROOT}/src/addons/sharepoint/bt_none.cpp")
  target_link_libraries(${target} PRIVATE mv_sharepoint_engine)
  target_compile_definitions(${target} PRIVATE
    MV_SHAREPOINT_MODULE_PATH="$<TARGET_FILE:mv_sharepoint>"
    MV_SHAREPOINT_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/sharepoint/manifest.json")
  add_dependencies(${target} mv_sharepoint)
endfunction()
