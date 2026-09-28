# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# plan/23: the Final Cut Pro pieces, Mac only (D9 exception, plan/12
# 2026-09-28). Included by cmake/darwin.cmake when the AI pack is built.
# Nothing here enters MediaViewer.app: `fcp_bundle` assembles
# "MediaViewer for Final Cut Pro.app" in the build tree, the "fcp" add-on's
# payload (plan/18 "absent means absent").
#
#   MediaViewerSearchAgent   the XPC search agent (hosts the AI pack's reader)
#   mv-search-client         its test client and bench (not shipped)
#   MediaViewerSearch        the workflow extension's executable (.appex)
#   MediaViewerForFCP        the container app's executable
#
# MV_FCP_TEAM_ID names the Apple team the add-on is signed by: the Mach
# service and the extension's app group are prefixed with it (a sandboxed
# client may look up only its group's services). Empty: "dev." names that a
# sandboxed FCP extension cannot reach, for building and the in-process tests.

set(MV_FCP_VERSION "0.1.0" CACHE STRING "The fcp add-on's version")
set(MV_FCP_TEAM_ID "" CACHE STRING "Apple Team ID the fcp add-on is signed by (prefixes its Mach service and app group)")
set(MV_FCP_SIGN_IDENTITY "" CACHE STRING "codesign identity for fcp_bundle (empty: ad hoc)")
set(MV_FCP_BUNDLE_ID "io.github.longtimeno-c.mediaviewer.fcp")
if(MV_FCP_TEAM_ID)
  set(MV_FCP_APP_GROUP "${MV_FCP_TEAM_ID}.${MV_FCP_BUNDLE_ID}")
else()
  set(MV_FCP_APP_GROUP "dev.mediaviewer.fcp")
endif()
set(MV_FCP_MACH_SERVICE "${MV_FCP_APP_GROUP}.search")
set(_fcp "${CMAKE_SOURCE_DIR}/src/nle/mac")
set(_fcp_defs MV_FCP_MACH_SERVICE="${MV_FCP_MACH_SERVICE}")

add_executable(MediaViewerSearchAgent "${_fcp}/agent_mac.mm" "${_fcp}/agent_protocol.h")
target_link_libraries(MediaViewerSearchAgent PRIVATE mv_nle mv_addon mv_io
  "-framework Foundation" "-framework Security")
target_compile_definitions(MediaViewerSearchAgent PRIVATE ${_fcp_defs})
if(MV_ADDON_DEV_PUBLIC_KEY)
  # A developer build's agent, ad hoc signed, may serve local clients.
  target_compile_definitions(MediaViewerSearchAgent PRIVATE MV_FCP_DEV_ALLOW_UNSIGNED=1)
endif()

add_executable(mv-search-client "${_fcp}/client_mac.mm")
target_link_libraries(mv-search-client PRIVATE mv_nle mv_addon mv_io "-framework Foundation")
target_compile_definitions(mv-search-client PRIVATE ${_fcp_defs})

# The extension: NSExtensionMain is its entry point; only the portable
# FCPXML and wire files are linked in (no SQLite, no add-on loader: it reads
# nothing of the user's).
add_executable(MediaViewerSearch
  "${_fcp}/extension_mac.mm"
  "${CMAKE_SOURCE_DIR}/src/nle/fcpxml.cpp"
  "${CMAKE_SOURCE_DIR}/src/nle/search_wire.cpp")
target_include_directories(MediaViewerSearch PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(MediaViewerSearch PRIVATE mv_project_options "-framework Foundation" "-framework AppKit")
target_compile_definitions(MediaViewerSearch PRIVATE ${_fcp_defs})
target_link_options(MediaViewerSearch PRIVATE "-e" "_NSExtensionMain" "-fapplication-extension")

add_executable(MediaViewerForFCP "${_fcp}/container_mac.mm")
target_link_libraries(MediaViewerForFCP PRIVATE "-framework AppKit" "-framework ServiceManagement")
target_compile_definitions(MediaViewerForFCP PRIVATE MV_FCP_AGENT_PLIST="${MV_FCP_MACH_SERVICE}.plist")

foreach(_t MediaViewerSearchAgent mv-search-client MediaViewerSearch MediaViewerForFCP)
  set_source_files_properties("${_fcp}/agent_mac.mm" "${_fcp}/client_mac.mm" "${_fcp}/extension_mac.mm"
                              "${_fcp}/container_mac.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  set_target_properties(${_t} PROPERTIES OSX_ARCHITECTURES "arm64")
endforeach()
set_source_files_properties("${_fcp}/extension_mac.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc -fapplication-extension")

configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Container-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Container-Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension-Info.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/agent.plist.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Extension.entitlements.in"
               "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements" @ONLY)

set(MV_FCP_APP "${CMAKE_BINARY_DIR}/nle/MediaViewer for Final Cut Pro.app")
set(_fcp_bundle_args
  --app "${MV_FCP_APP}"
  --container "$<TARGET_FILE:MediaViewerForFCP>"
  --agent "$<TARGET_FILE:MediaViewerSearchAgent>"
  --appex "$<TARGET_FILE:MediaViewerSearch>"
  --container-plist "${CMAKE_BINARY_DIR}/packaging/fcp/Container-Info.plist"
  --appex-plist "${CMAKE_BINARY_DIR}/packaging/fcp/Extension-Info.plist"
  --agent-plist "${CMAKE_BINARY_DIR}/packaging/fcp/${MV_FCP_MACH_SERVICE}.plist"
  --appex-entitlements "${CMAKE_BINARY_DIR}/packaging/fcp/Extension.entitlements")
if(MV_FCP_SIGN_IDENTITY)
  list(APPEND _fcp_bundle_args --identity "${MV_FCP_SIGN_IDENTITY}")
endif()
if(MV_ADDON_DEV_PUBLIC_KEY)
  list(APPEND _fcp_bundle_args
       --agent-entitlements "${CMAKE_SOURCE_DIR}/packaging/macos/fcp/Agent-dev.entitlements")
endif()
add_custom_target(fcp_bundle ALL
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/mac/fcp_bundle.py" ${_fcp_bundle_args}
  DEPENDS MediaViewerForFCP MediaViewerSearchAgent MediaViewerSearch
  COMMENT "Assembling MediaViewer for Final Cut Pro.app (the fcp add-on)"
  VERBATIM)
