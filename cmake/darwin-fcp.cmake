# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/design/23: the Final Cut Pro pieces, Mac only (D9 exception, docs/design/12
# 2026-09-28). Included by cmake/darwin-app.cmake after the MediaViewer target
# exists, on every Mac build: the names below are compiled into the app on
# both architectures, so a universal app agrees with itself. The rest exists
# only where the AI pack is built (arm64); macpack.py puts it in
# MediaViewer.app (docs/design/12 2026-09-28, amended the same day):
#
#   the search agent         MediaViewer's own executable run by launchd with
#                            `--search-agent` (src/nle/mac/agent_mac.mm, linked
#                            into MediaViewer with mv_nle); it hosts the
#                            installed AI pack's reader. Its launchd job is in
#                            Contents/Library/LaunchAgents
#   MediaViewerSearch        Contents/PlugIns/MediaViewerSearch.appex: the
#                            workflow extension
#   mv-search-client         the agent's test client and bench (not shipped)
#
# Both stay dormant until the owner turns Final Cut Pro on in Settings > Local
# search (src/shell/fcp_mac.mm): the agent is not registered with launchd and
# the extension is hidden from FCP by a PlugInKit election.
#
# MV_FCP_TEAM_ID names the Apple team the app is signed by: the Mach service
# and the extension's app group are prefixed with it (a sandboxed client may
# look up only its group's services). Empty: "dev." names that a sandboxed FCP
# extension cannot reach, for building and the in-process tests.

set(MV_FCP_TEAM_ID "" CACHE STRING "Apple Team ID MediaViewer.app is signed by (prefixes the FCP agent's Mach service and app group)")
if(MV_FCP_TEAM_ID)
  set(MV_FCP_APP_GROUP "${MV_FCP_TEAM_ID}.${MV_MAC_BUNDLE_ID}.fcp")
else()
  set(MV_FCP_APP_GROUP "dev.mediaviewer.fcp")
endif()
set(MV_FCP_MACH_SERVICE "${MV_FCP_APP_GROUP}.search")
set(MV_FCP_EXTENSION_ID "${MV_MAC_BUNDLE_ID}.finalcut")
set(_fcp_defs MV_FCP_MACH_SERVICE="${MV_FCP_MACH_SERVICE}")

# The app turns the pieces on and off by these names (src/shell/fcp_mac.mm).
target_compile_definitions(MediaViewer PRIVATE
  MV_FCP_AGENT_PLIST="${MV_FCP_MACH_SERVICE}.plist"
  MV_FCP_EXTENSION_ID="${MV_FCP_EXTENSION_ID}"
  ${_fcp_defs})

set(MV_FCP_ASSEMBLE_ARGS "")
set(MV_FCP_ASSEMBLE_DEPENDS "")
if(NOT TARGET mv_nle)
  return()  # an Intel build: no AI pack, so no agent and no extension
endif()

set(_fcp "${CMAKE_SOURCE_DIR}/src/nle/mac")

# The agent is MediaViewer itself in agent mode (main_mac.mm): the app already
# links the add-on store, the verifier and SQLite, so a separate executable
# would carry ~1.8 MB of copies (docs/design/12 2026-09-28).
target_sources(MediaViewer PRIVATE "${_fcp}/agent_mac.mm" "${_fcp}/agent_mac.h" "${_fcp}/agent_protocol.h")
target_link_libraries(MediaViewer PRIVATE mv_nle)
target_compile_definitions(MediaViewer PRIVATE MV_WITH_SEARCH_AGENT=1)
if(MV_ADDON_DEV_PUBLIC_KEY)
  # An ad hoc developer build has no team to check peers against.
  target_compile_definitions(MediaViewer PRIVATE MV_FCP_DEV_ALLOW_UNSIGNED=1)
endif()

add_executable(mv-search-client "${_fcp}/client_mac.mm")
target_link_libraries(mv-search-client PRIVATE mv_nle mv_addon mv_io "-framework Foundation")
target_compile_definitions(mv-search-client PRIVATE ${_fcp_defs})

add_executable(MediaViewerSearch
  "${_fcp}/extension_mac.mm"
  "${CMAKE_SOURCE_DIR}/src/nle/fcpxml.cpp"
  "${CMAKE_SOURCE_DIR}/src/nle/search_wire.cpp")
target_include_directories(MediaViewerSearch PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(MediaViewerSearch PRIVATE mv_project_options "-framework Foundation" "-framework AppKit"
  "-framework AVFoundation" "-framework AVKit" "-framework CoreMedia")
target_compile_definitions(MediaViewerSearch PRIVATE ${_fcp_defs}
  MV_FCP_APP_BUNDLE_ID="${MV_MAC_BUNDLE_ID}")  # "Open in MediaViewer"
# Its own main: loads FCP's ProExtension.framework, then NSExtensionMain.
target_link_options(MediaViewerSearch PRIVATE "-fapplication-extension")

set_source_files_properties("${_fcp}/agent_mac.mm" "${_fcp}/client_mac.mm"
                            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
set_source_files_properties("${_fcp}/extension_mac.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc -fapplication-extension")
set_target_properties(mv-search-client MediaViewerSearch PROPERTIES OSX_ARCHITECTURES "arm64")

configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/agent.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension.entitlements.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements" @ONLY)

# macpack.py assemble options (cmake/darwin-app.cmake appends them). The
# entitlements are applied at assemble; `macpack.py release` keeps them.
set(MV_FCP_ASSEMBLE_ARGS
  --fcp-agent-plist "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist"
  --fcp-appex-exe "$<TARGET_FILE:MediaViewerSearch>"
  --fcp-appex-plist "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist"
  --fcp-appex-entitlements "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements")
set(MV_FCP_ASSEMBLE_DEPENDS MediaViewerSearch)
