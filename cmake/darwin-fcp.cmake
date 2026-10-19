# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/design/23: the Final Cut Pro pieces, Mac only (D9 exception, docs/design/12
# 2026-09-28). Included by cmake/darwin-app.cmake after the MediaViewer target
# exists, on every Mac build: the names below are compiled into the app on
# both architectures, so a universal app agrees with itself. Since 2026-10-19
# (#485, docs/design/12 "Final Cut Pro search is an add-on") the agent and the
# extension are the Final Cut Pro add-on's, built where the AI pack is built
# (arm64) into build/addons/fcp and packed by tools/package/addon-pack.py:
#
#   libmv_fcp.dylib          the search agent (src/addons/fcp/agent_mac.mm): its
#                            listener, peer checks and idle exit, mv.fcp.1
#   MediaViewer Search.app   a background-only container (src/addons/fcp/container_main.cpp)
#                            around MediaViewerSearch.appex, the workflow extension
#                            (src/nle/mac/extension_mac.mm): macOS discovers an
#                            extension only inside an app LaunchServices knows
#
# The app keeps what only it can: `MediaViewer --search-agent`
# (src/shell/fcp_host_mac.mm), which loads the verified add-on and lends it
# the AI pack's read-only reader, and the agent's ~1 KB launchd job in
# Contents/Library/LaunchAgents (SMAppService registers only a job inside the
# bundle).
#
#   mv-search-client         the agent's test client and bench (not shipped)
#
# Nothing runs until the owner installs the add-on and turns Final Cut Pro on
# in Settings > Local search (src/shell/fcp_mac.mm): the agent is not
# registered with launchd and no extension exists for FCP to list.
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

set(MV_FCP_VERSION "1.0.0" CACHE STRING "Final Cut Pro add-on version (its manifest's)")
target_compile_definitions(MediaViewer PRIVATE
  MV_FCP_ADDON_CONTAINER="MediaViewer Search.app"
  MV_FCP_ADDON_EXTENSION="MediaViewer Search.app/Contents/PlugIns/MediaViewerSearch.appex")

set(MV_FCP_ASSEMBLE_ARGS "")
set(MV_FCP_ASSEMBLE_DEPENDS "")
if(NOT TARGET mv_nle)
  return()  # an Intel build: no AI pack, so no add-on and no job
endif()
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/agent.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist" @ONLY)
# macpack.py assemble options (cmake/darwin-app.cmake appends them): the job only.
set(MV_FCP_ASSEMBLE_ARGS --fcp-agent-plist "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist")

set(_fcp "${CMAKE_SOURCE_DIR}/src/nle/mac")
set(_fcp_addon "${CMAKE_SOURCE_DIR}/src/addons/fcp")
set(_fcp_out "${CMAKE_BINARY_DIR}/addons/fcp")

# The host half, in the app: `--search-agent` (main_mac.mm).
target_sources(MediaViewer PRIVATE src/shell/fcp_host_mac.mm src/shell/fcp_host_mac.h)
target_link_libraries(MediaViewer PRIVATE mv_nle)
target_compile_definitions(MediaViewer PRIVATE MV_WITH_SEARCH_AGENT=1)
if(MV_ADDON_DEV_PUBLIC_KEY)
  # An ad hoc developer build has no team to check peers against.
  target_compile_definitions(MediaViewer PRIVATE MV_FCP_DEV_ALLOW_UNSIGNED=1)
endif()

# The add-on's library: links nothing of the core (the wire format is a
# header and one file); the host lends it Local search through mv.fcp.1.
add_library(mv_fcp MODULE
  "${_fcp_addon}/addon_entry.cpp"
  "${_fcp_addon}/agent_mac.mm"
  "${_fcp}/agent_protocol.h"
  "${CMAKE_SOURCE_DIR}/src/nle/search_wire.cpp")
target_include_directories(mv_fcp PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/abi/include")
target_link_libraries(mv_fcp PRIVATE mv_project_options "-framework Foundation" "-framework Security")
target_compile_definitions(mv_fcp PRIVATE MV_FCP_VERSION="${MV_FCP_VERSION}")
set_target_properties(mv_fcp PROPERTIES
  OUTPUT_NAME "mv_fcp" PREFIX "lib" SUFFIX ".dylib"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  OSX_ARCHITECTURES "arm64"
  LIBRARY_OUTPUT_DIRECTORY "${_fcp_out}")

add_executable(mv-search-client "${_fcp}/client_mac.mm")
target_link_libraries(mv-search-client PRIVATE mv_nle mv_addon mv_io "-framework Foundation")
target_compile_definitions(mv-search-client PRIVATE ${_fcp_defs})

add_executable(MediaViewerSearch
  "${_fcp}/extension_mac.mm"
  "${_fcp}/timeline_bar_mac.mm"
  "${CMAKE_SOURCE_DIR}/src/nle/fcpxml.cpp"
  "${CMAKE_SOURCE_DIR}/src/nle/search_wire.cpp")
# docs/design/29: the timeline bar reads a dropped project with the shared model.
target_link_libraries(MediaViewerSearch PRIVATE mv_nle_timeline)
target_include_directories(MediaViewerSearch PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(MediaViewerSearch PRIVATE mv_project_options "-framework Foundation" "-framework AppKit"
  "-framework AVFoundation" "-framework AVKit" "-framework CoreMedia")
target_compile_definitions(MediaViewerSearch PRIVATE ${_fcp_defs}
  MV_FCP_APP_GROUP="${MV_FCP_APP_GROUP}"       # the timeline bar's shared folder
  MV_FCP_APP_BUNDLE_ID="${MV_MAC_BUNDLE_ID}")  # "Open in MediaViewer"
# Its own main: loads FCP's ProExtension.framework, then NSExtensionMain.
target_link_options(MediaViewerSearch PRIVATE "-fapplication-extension")

set_source_files_properties("${_fcp_addon}/agent_mac.mm" "${_fcp}/client_mac.mm" src/shell/fcp_host_mac.mm
                            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
set_source_files_properties("${_fcp}/extension_mac.mm" "${_fcp}/timeline_bar_mac.mm"
                            PROPERTIES COMPILE_FLAGS "-fobjc-arc -fapplication-extension")
set_target_properties(mv-search-client MediaViewerSearch PROPERTIES OSX_ARCHITECTURES "arm64")

configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension.entitlements.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements" @ONLY)

# The container and its extension beside the library, ad hoc signed (the
# extension with its sandbox and app group; a release re-signs both with the
# team's identity before packing, keeping these entitlements: RELEASING.md).
add_executable(MediaViewerSearchHost "${_fcp_addon}/container_main.cpp")
target_link_libraries(MediaViewerSearchHost PRIVATE mv_project_options)
set_target_properties(MediaViewerSearchHost PROPERTIES OSX_ARCHITECTURES "arm64")
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Container-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Container-Info.plist" @ONLY)
set(_container "${_fcp_out}/MediaViewer Search.app")
set(_appex "${_container}/Contents/PlugIns/MediaViewerSearch.appex")
add_custom_target(mv_fcp_bundle ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_container}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_appex}/Contents/MacOS" "${_container}/Contents/MacOS"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:MediaViewerSearch>" "${_appex}/Contents/MacOS/MediaViewerSearch"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist" "${_appex}/Contents/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:MediaViewerSearchHost>"
          "${_container}/Contents/MacOS/MediaViewerSearchHost"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/packaging/fcp/Container-Info.plist" "${_container}/Contents/Info.plist"
  COMMAND /usr/bin/codesign --force --sign - --entitlements "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements"
          "${_appex}"
  COMMAND /usr/bin/codesign --force --sign - "${_container}"
  DEPENDS MediaViewerSearch MediaViewerSearchHost
  COMMENT "The Final Cut Pro add-on's container and extension (build/addons/fcp)"
  VERBATIM)
add_dependencies(mv_fcp mv_fcp_bundle)
