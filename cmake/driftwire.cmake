# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Driftwire add-on (docs/design/28): send files to a MediaViewer nearby.
# Included by the Windows root and cmake/darwin.cmake after import.cmake and
# places.cmake, so the two cannot drift.
#
#   mv_driftwire_engine  the wire, Noise, the sealed channel, the transfer, the
#                         engine and its view tree, and the network and
#                         Bluetooth ports. Links libsodium and BLAKE3; not the
#                         core's io (the tests bring mv_io, the module compiles
#                         the file port in).
#   mv_driftwire         the add-on: ONE shared library whose only export is
#                         mv_addon_get. It links nothing of the core: the core
#                         reaches it only through the host function table (v3:
#                         screens and slots).
#
# Built beside the app for tests and packing, never installed with it:
# tools/package/addon-pack.py packs build/addons/driftwire, signed.
#
# Expects: mv_project_options, MV_SODIUM_TARGET, BLAKE3::blake3, Threads.

set(MV_DRIFTWIRE_VERSION "1.0.0" CACHE STRING "Driftwire add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_S "${MV_SOURCE_ROOT}/src/addons/driftwire")

# The Bluetooth bearer goes into the module only: the tests run the engine
# over loopback and link bt_none.cpp (one Objective-C class per process).
if(WIN32)
  set(_driftwire_port ${_S}/net_win.cpp ${_S}/secrets_win.cpp ${_S}/power_win.cpp)
  set(_driftwire_bt ${_S}/bt_win.cpp)
elseif(APPLE)
  set(_driftwire_port ${_S}/net_posix.cpp ${_S}/machine_mac.cpp ${_S}/secrets_mac.cpp ${_S}/power_mac.cpp)
  set(_driftwire_bt ${_S}/bt_mac.mm)
else()
  set(_driftwire_port ${_S}/net_posix.cpp ${_S}/power_none.cpp)
  set(_driftwire_bt ${_S}/bt_none.cpp)
endif()

add_library(mv_driftwire_engine STATIC
  ${_S}/wire.cpp ${_S}/wire.h
  ${_S}/link.h
  ${_S}/noise.cpp ${_S}/noise.h
  ${_S}/cpace.cpp ${_S}/cpace.h
  ${_S}/channel.cpp ${_S}/channel.h
  ${_S}/transfer.cpp ${_S}/transfer.h
  ${_S}/stages.cpp ${_S}/stages.h
  ${_S}/presence.cpp ${_S}/presence.h
  ${_S}/bt_pipe.cpp ${_S}/bt_pipe.h
  ${_S}/net.h ${_S}/net_address.cpp
  ${_S}/invite.cpp ${_S}/invite.h
  ${_S}/mapping.cpp ${_S}/mapping.h
  ${_S}/race.h
  ${_S}/internet.cpp ${_S}/internet.h
  ${_S}/bencode.cpp ${_S}/bencode.h
  ${_S}/sha1.cpp ${_S}/sha1.h
  ${_S}/dht_item.cpp ${_S}/dht_item.h ${_S}/wordlist.inc
  ${_S}/udp_hub.cpp ${_S}/udp_hub.h
  ${_S}/dht.cpp ${_S}/dht.h
  ${_S}/meet.cpp ${_S}/meet.h
  ${_S}/handover.cpp ${_S}/handover.h
  ${_S}/secrets.h ${_S}/secrets_file.cpp
  ${_S}/known.h ${_S}/known.cpp
  ${_S}/splice.cpp ${_S}/splice.h
  ${_S}/relay.cpp ${_S}/relay.h
  ${_S}/engine.cpp ${_S}/engine.h ${_S}/engine_relay.cpp
  ${_S}/prefs.cpp ${_S}/prefs.h
  ${_S}/resume.cpp ${_S}/resume.h ${_S}/eta.h
  ${_S}/power.h
  ${_S}/tree.cpp
  ${_driftwire_port}
)
target_include_directories(mv_driftwire_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_driftwire_engine PUBLIC mv_project_options ${MV_SODIUM_TARGET} BLAKE3::blake3
  Threads::Threads)
set_target_properties(mv_driftwire_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # shell32 / ole32: the Downloads folder (SHGetKnownFolderPath, CoTaskMemFree).
  # iphlpapi: this machine's addresses and its default route (#472 PR 2).
  # advapi32: Credential Manager (#472 PR 4). crypt32: DPAPI around its values (#501).
  # PowerCreateRequest is kernel32's (#530), linked by default.
  target_link_libraries(mv_driftwire_engine PUBLIC ws2_32 shell32 ole32 iphlpapi advapi32 crypt32)
elseif(APPLE)
  # Security: the Keychain (#472 PR 4). IOKit: staying awake during a transfer (#530).
  target_link_libraries(mv_driftwire_engine PUBLIC "-framework CoreFoundation" "-framework SystemConfiguration"
    "-framework Security" "-framework IOKit")
endif()

if(WIN32)
  set(_driftwire_io ${MV_SOURCE_ROOT}/src/io/file_port_win.cpp)
else()
  set(_driftwire_io ${MV_SOURCE_ROOT}/src/io/file_port_mac.cpp)
endif()

add_library(mv_driftwire MODULE
  ${_S}/addon_entry.cpp
  ${_driftwire_bt}
  # The file port and BLAKE3 hashing, compiled in: the add-on links nothing
  # of the core (as Places carries io/mapped_file).
  ${MV_SOURCE_ROOT}/src/io/file_port.cpp
  ${MV_SOURCE_ROOT}/src/io/content_hash.cpp
  ${_driftwire_io}
)
target_link_libraries(mv_driftwire PRIVATE mv_driftwire_engine)
if(WIN32)
  # C++/WinRT for the Bluetooth bearer (Windows.Devices.Bluetooth,
  # windowsapp.lib). Its projection throws; bt_win.cpp catches every call.
  target_link_libraries(mv_driftwire PRIVATE windowsapp ole32)
  set_source_files_properties(${_S}/bt_win.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
elseif(APPLE)
  target_link_libraries(mv_driftwire PRIVATE "-framework Foundation" "-framework CoreBluetooth")
  set_source_files_properties(${_S}/bt_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
target_compile_definitions(mv_driftwire PRIVATE MV_DRIFTWIRE_VERSION="${MV_DRIFTWIRE_VERSION}")
set_target_properties(mv_driftwire PROPERTIES
  OUTPUT_NAME "mv_driftwire"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/driftwire"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/driftwire")
if(APPLE)
  set_target_properties(mv_driftwire PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_driftwire PROPERTIES PREFIX "")
endif()

# #472 relays: a relay server of the person's own (tools/driftwire-relay),
# portable, built beside the add-on and never shipped with the app. It is the
# splice alone (splice.cpp and the network port); nothing of the engine runs.
add_executable(mv_driftwire_relay "${MV_SOURCE_ROOT}/tools/driftwire-relay/main.cpp")
target_link_libraries(mv_driftwire_relay PRIVATE mv_driftwire_engine)
set_target_properties(mv_driftwire_relay PROPERTIES OUTPUT_NAME "mv-driftwire-relay")

# #540 step 1: a transfer between two real machines with no app
# (tools/driftwire-bench), the real handshake and transfer.h, timed per
# stage. Built beside the add-on, never shipped. The file port is the core's
# io, as the tests link it.
add_executable(mv_driftwire_bench "${MV_SOURCE_ROOT}/tools/driftwire-bench/main.cpp")
target_link_libraries(mv_driftwire_bench PRIVATE mv_driftwire_engine mv_io)
set_target_properties(mv_driftwire_bench PROPERTIES OUTPUT_NAME "mv-driftwire-bench")

# The engine over loopback TCP and an in-memory discovery bus, and the add-on
# itself through the real loader (tests/test_driftwire.cpp,
# tests/test_driftwire_addon.cpp). Called by each build's mv_import_tests
# block, which links mv_io (the file port) and libsodium.
function(mv_driftwire_tests target)
  target_sources(${target} PRIVATE
    "${MV_SOURCE_ROOT}/tests/test_driftwire.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_noise.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_session.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_internet.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_dht.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_stream_bench.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_eta.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_resume.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_known.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_privacy.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_relay.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_addon.cpp"
    "${MV_SOURCE_ROOT}/src/addons/driftwire/bt_none.cpp")
  target_link_libraries(${target} PRIVATE mv_driftwire_engine)
  target_compile_definitions(${target} PRIVATE
    MV_DRIFTWIRE_MODULE_PATH="$<TARGET_FILE:mv_driftwire>"
    MV_DRIFTWIRE_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/driftwire/manifest.json")
  add_dependencies(${target} mv_driftwire mv_driftwire_relay)
endfunction()
