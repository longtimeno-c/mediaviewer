# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Driftwire add-on (docs/design/28): send files to a MediaViewer nearby.
# Included by the Windows root and cmake/darwin.cmake after import.cmake and
# places.cmake, so the two cannot drift, and by cmake/portable (Linux).
#
#   mv_wire              the protocol (cmake/wire.cmake, src/wire): the wire,
#                         Noise, the sealed channel, the NAS service, the
#                         secret store and the network ports. The app's phone
#                         feature links it too.
#   mv_driftwire_engine  the transfer, the DHT, relays, the engine and its
#                         view tree, and the Bluetooth port, over mv_wire. Links libsodium and BLAKE3; not the
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
# Expects: mv_project_options, MV_SODIUM_TARGET, Threads, and BLAKE3 as
# BLAKE3::blake3 or the target named by MV_BLAKE3_TARGET (cmake/portable's
# source fallback).

set(MV_DRIFTWIRE_VERSION "1.0.0" CACHE STRING "Driftwire add-on version (its manifest's)")
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(_S "${MV_SOURCE_ROOT}/src/addons/driftwire")
if(NOT DEFINED MV_BLAKE3_TARGET)
  set(MV_BLAKE3_TARGET BLAKE3::blake3)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/wire.cmake")


# The Bluetooth bearer goes into the module only: the tests run the engine
# over loopback and link bt_none.cpp (one Objective-C class per process).
if(WIN32)
  set(_driftwire_port ${_S}/power_win.cpp)
  set(_driftwire_bt ${_S}/bt_win.cpp)
elseif(IOS)
  # No module on iOS: mv_wire carries the file port and BLAKE3 hashing
  # (cmake/wire.cmake), the engine the empty Bluetooth port (no bearer yet).
  set(_driftwire_port ${_S}/power_ios.cpp ${_S}/bt_none.cpp)
  set(_driftwire_bt "")
elseif(APPLE)
  set(_driftwire_port ${_S}/power_mac.cpp)
  set(_driftwire_bt ${_S}/bt_mac.mm)
else()
  # Linux: the Secret Service (libsecret) and logind (libsystemd / libelogind)
  # are loaded at run time, so there is nothing to build against; no
  # Bluetooth bearer yet (bt_none.cpp: Wi-Fi and the internet only).
  set(_driftwire_port ${_S}/power_linux.cpp)
  set(_driftwire_bt ${_S}/bt_none.cpp)
endif()

add_library(mv_driftwire_engine STATIC
  ${_S}/transfer.cpp ${_S}/transfer.h
  ${_S}/presence.cpp ${_S}/presence.h
  ${_S}/bt_pipe.cpp ${_S}/bt_pipe.h
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
  ${_S}/relay.cpp ${_S}/relay.h
  ${_S}/presence_keys.cpp ${_S}/presence_keys.h
  ${_S}/engine.cpp ${_S}/engine.h ${_S}/engine_relay.cpp
  ${_S}/prefs.cpp ${_S}/prefs.h
  ${_S}/resume.cpp ${_S}/resume.h ${_S}/eta.h
  ${_S}/power.h
  ${_S}/tree.cpp
  ${_driftwire_port}
)
target_include_directories(mv_driftwire_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_driftwire_engine PUBLIC mv_project_options mv_wire ${MV_SODIUM_TARGET} ${MV_BLAKE3_TARGET}
  Threads::Threads)
set_target_properties(mv_driftwire_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # shell32 / ole32: the Downloads folder (SHGetKnownFolderPath, CoTaskMemFree).
  # iphlpapi: this machine's addresses and its default route (#472 PR 2).
  # advapi32: Credential Manager (#472 PR 4). crypt32: DPAPI around its values (#501).
  # PowerCreateRequest is kernel32's (#530), linked by default.
  target_link_libraries(mv_driftwire_engine PUBLIC ws2_32 shell32 ole32 iphlpapi advapi32 crypt32 dnsapi)
elseif(IOS)
  target_link_libraries(mv_driftwire_engine PUBLIC "-framework CoreFoundation" "-framework Security")
elseif(APPLE)
  # Security: the Keychain (#472 PR 4). IOKit: staying awake during a transfer (#530).
  target_link_libraries(mv_driftwire_engine PUBLIC "-framework CoreFoundation" "-framework SystemConfiguration"
    "-framework Security" "-framework IOKit")
else()
  # dlopen: libsecret (secrets_linux.cpp), libsystemd (power_linux.cpp).
  target_link_libraries(mv_driftwire_engine PUBLIC ${CMAKE_DL_LIBS})
endif()

# iOS links the engine statically: no add-on module, relay or command line.
if(IOS)
  return()
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
# io, as the tests link it; the command line's own package build
# (cmake/driftwire-cli) has no mv_io, and does not build it.
if(TARGET mv_io)
  add_executable(mv_driftwire_bench "${MV_SOURCE_ROOT}/tools/driftwire-bench/main.cpp")
  target_link_libraries(mv_driftwire_bench PRIVATE mv_driftwire_engine mv_io)
  set_target_properties(mv_driftwire_bench PROPERTIES OUTPUT_NAME "mv-driftwire-bench")
endif()

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
    "${MV_SOURCE_ROOT}/tests/test_driftwire_serve.cpp"
    "${MV_SOURCE_ROOT}/tests/test_phone_pairing.cpp"
    "${MV_SOURCE_ROOT}/tests/test_phone_relay.cpp"
    "${MV_SOURCE_ROOT}/tests/test_serve_owner.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_addon.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_cli_ui.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_cli.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_cli_update.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_nas_update.cpp"
    "${MV_SOURCE_ROOT}/tests/test_driftwire_cli_e2e.cpp"
    "${MV_SOURCE_ROOT}/src/addons/driftwire/bt_none.cpp")
  # test_phone_pairing.cpp: the desk is the app's now (src/phone, cmake/phone.cmake).
  target_link_libraries(${target} PRIVATE mv_driftwire_engine mv_driftwire_cli_core mv_phone)
  target_compile_definitions(${target} PRIVATE
    MV_DRIFTWIRE_MODULE_PATH="$<TARGET_FILE:mv_driftwire>"
    MV_DRIFTWIRE_MANIFEST_JSON="${MV_SOURCE_ROOT}/src/addons/driftwire/manifest.json"
    # The command line, run by its tests: the program (it refuses a pipe) and
    # the test build (two of them talk in the end-to-end test).
    MV_DRIFTWIRE_CLI_PATH="$<TARGET_FILE:mv_driftwire_cli>"
    MV_DRIFTWIRE_CLI_TEST_PATH="$<TARGET_FILE:mv_driftwire_cli_test>")
  add_dependencies(${target} mv_driftwire mv_driftwire_relay mv_driftwire_cli mv_driftwire_cli_test)
endfunction()

# ---- the driftwire command line (docs/design/28 "From a terminal") ----------
# A terminal host of the engine on Linux, Windows and macOS. FTXUI (MIT) lays
# out and colours what it draws: vcpkg's port where the build uses vcpkg
# (the root manifest, tools/portable/vcpkg.json), else a checkout of the
# pinned release named by MV_FTXUI_SOURCE_DIR (a bare machine, as
# MV_BLAKE3_SOURCE_DIR). Only its dom and screen parts are linked.
#
#   mv_driftwire_cli_core     everything of the command line but main(): the
#                             terminal port, keys, the line editor, paths, the
#                             look, the arguments, the host and the session.
#                             No io sources (the tests bring mv_io; the
#                             programs compile the file port in, as the module).
#   mv_driftwire_cli          the program, `driftwire` (OUTPUT_NAME): main(),
#                             no Bluetooth bearer (bt_none.cpp), the file port.
#                             Refuses to run without a terminal.
#   mv_driftwire_cli_test     the same with MV_DRIFTWIRE_CLI_TEST_HOOKS: runs
#                             without a terminal, loopback-only networking and
#                             machine-readable lines on request (args.h), for
#                             the end-to-end test. Never packaged.
#   mv_driftwire_cli_gallery  a test tool, never packaged: every card and state
#                             with made-up data, to review and screenshot the look.
#
# The version: MV_DRIFTWIRE_CLI_VERSION, else the top-level project's (the
# app's, or a packaging project's), else the add-on's.
if(NOT TARGET ftxui::dom)
  find_package(ftxui CONFIG QUIET)
endif()
if(NOT TARGET ftxui::dom)
  if(MV_FTXUI_SOURCE_DIR)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
    add_subdirectory("${MV_FTXUI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/ftxui" EXCLUDE_FROM_ALL SYSTEM)
  else()
    message(FATAL_ERROR "The driftwire command line needs FTXUI: install it (vcpkg `ftxui`) or set "
                        "MV_FTXUI_SOURCE_DIR to a checkout of its pinned release")
  endif()
endif()

set(_C "${_S}/cli")
if(WIN32)
  # *_win.cpp: may include <windows.h>
  set(_driftwire_cli_port ${_C}/term_win.cpp ${_C}/data_dir_win.cpp ${_C}/update_net_win.cpp)
elseif(APPLE)
  set(_driftwire_cli_port ${_C}/term_posix.cpp ${_C}/data_dir_posix.cpp ${_C}/update_net_mac.mm)
  set_source_files_properties(${_C}/update_net_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
  set(_driftwire_cli_port ${_C}/term_posix.cpp ${_C}/data_dir_posix.cpp ${_C}/update_net_linux.cpp)
endif()
# Where `driftwire` asks whether a newer version is out, and whose installers
# `driftwire update` runs (cli/update.h): the site release.yml deploys.
set(MV_DRIFTWIRE_SITE_URL "https://longtimeno-c.github.io/mediaviewer/driftwire" CACHE STRING
  "The driftwire command line's site: latest.json, install.sh, install.ps1")
add_library(mv_driftwire_cli_core STATIC
  ${_C}/theme.h
  ${_C}/text.cpp ${_C}/text.h
  ${_C}/keys.cpp ${_C}/keys.h
  ${_C}/line_editor.cpp ${_C}/line_editor.h
  ${_C}/paths_arg.cpp ${_C}/paths_arg.h
  ${_C}/complete.cpp ${_C}/complete.h
  ${_C}/render.cpp ${_C}/render.h
  ${_C}/inline_view.cpp ${_C}/inline_view.h
  ${_C}/args.cpp ${_C}/args.h
  ${_C}/host.cpp ${_C}/host.h
  ${_C}/session.cpp ${_C}/session.h
  ${_C}/serve_cmd.cpp ${_C}/serve_cmd.h
  ${_C}/update.cpp ${_C}/update.h
  ${_C}/nas_update.cpp ${_C}/nas_update.h
  ${_C}/term.h ${_C}/data_dir.h ${_driftwire_cli_port}
)
target_link_libraries(mv_driftwire_cli_core PUBLIC mv_driftwire_engine ftxui::dom ftxui::screen)
# The NAS package's update key (docs/plans/nas-setup.md §6): 64 hex digits of
# the Ed25519 public key that signs <site>/nas/latest.json. Empty (the default,
# every build of one's own): `driftwire serve` never updates itself.
set(MV_NAS_UPDATE_KEY "" CACHE STRING "Ed25519 public key (hex) pinned for the NAS service's self-update")
target_compile_definitions(mv_driftwire_cli_core PRIVATE MV_DRIFTWIRE_SITE_URL="${MV_DRIFTWIRE_SITE_URL}"
                                                         MV_NAS_UPDATE_KEY="${MV_NAS_UPDATE_KEY}")
if(WIN32)
  # winhttp: the update check (update_net_win.cpp), Windows' own DLL.
  target_link_libraries(mv_driftwire_cli_core PUBLIC winhttp)
elseif(APPLE)
  # Foundation: NSURLSession for the update check (update_net_mac.mm).
  target_link_libraries(mv_driftwire_cli_core PUBLIC "-framework Foundation")
endif()

if(NOT DEFINED MV_DRIFTWIRE_CLI_VERSION OR MV_DRIFTWIRE_CLI_VERSION STREQUAL "")
  if(CMAKE_PROJECT_VERSION)
    set(MV_DRIFTWIRE_CLI_VERSION "${CMAKE_PROJECT_VERSION}")
  else()
    set(MV_DRIFTWIRE_CLI_VERSION "${MV_DRIFTWIRE_VERSION}")
  endif()
endif()

function(_mv_driftwire_cli_program target)
  add_executable(${target}
    ${_C}/main.cpp
    ${_S}/bt_none.cpp
    # The file port and BLAKE3 hashing, compiled in (as the module does).
    ${MV_SOURCE_ROOT}/src/io/file_port.cpp
    ${MV_SOURCE_ROOT}/src/io/content_hash.cpp
    ${_driftwire_io})
  target_link_libraries(${target} PRIVATE mv_driftwire_cli_core)
  target_compile_definitions(${target} PRIVATE MV_DRIFTWIRE_CLI_VERSION="${MV_DRIFTWIRE_CLI_VERSION}")
  # The Windows ASan leg: a spawned program needs the runtime beside it
  # (the helper exists only in the Windows root build).
  if(COMMAND mv_asan_runtime_beside)
    mv_asan_runtime_beside(${target})
  endif()
endfunction()
_mv_driftwire_cli_program(mv_driftwire_cli)
set_target_properties(mv_driftwire_cli PROPERTIES OUTPUT_NAME "driftwire")
_mv_driftwire_cli_program(mv_driftwire_cli_test)
target_compile_definitions(mv_driftwire_cli_test PRIVATE MV_DRIFTWIRE_CLI_TEST_HOOKS=1)

add_executable(mv_driftwire_cli_gallery "${MV_SOURCE_ROOT}/tools/driftwire-cli-gallery/main.cpp")
target_link_libraries(mv_driftwire_cli_gallery PRIVATE mv_driftwire_cli_core)

