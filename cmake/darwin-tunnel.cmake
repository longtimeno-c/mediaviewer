# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Tunnel's slice 5 (docs/design/28 "The Tunnel"): Driftwire VPN's packet
# tunnel provider, a thin Swift pump that macOS loads only from the app that
# installs it, so it sits in MediaViewer.app's
# Contents/Library/SystemExtensions (docs/design/12, 2026-11-12). Included by
# cmake/darwin-app.cmake.
#
# Off by default: the Network Extension entitlement it needs is Apple's to
# grant to the signing team (owed; src/addons/driftwire/mac_tunnel/README.md).
# -DMV_DRIFTWIRE_PACKET_TUNNEL=ON builds it and macpack.py embeds it; an ad hoc
# assemble signs it without the entitlement (macOS will not load that copy),
# `macpack.py release --team-id` with it.

option(MV_DRIFTWIRE_PACKET_TUNNEL "Build and embed Driftwire VPN's packet tunnel provider (needs Apple's entitlement to run)" OFF)
set(MV_TUNNEL_ASSEMBLE_ARGS "")
set(MV_TUNNEL_ASSEMBLE_DEPENDS "")
if(NOT MV_DRIFTWIRE_PACKET_TUNNEL)
  return()
endif()

set(_tun "${CMAKE_SOURCE_DIR}/src/addons/driftwire/mac_tunnel")
if(NOT DEFINED MV_DRIFTWIRE_VERSION)
  set(MV_DRIFTWIRE_VERSION "1.0.0")
endif()
# The provider's Mach service is prefixed with the team (a "dev" stand-in
# without one: built, not loadable).
if(MV_FCP_TEAM_ID)
  set(MV_TUNNEL_MACH_PREFIX "${MV_FCP_TEAM_ID}")
else()
  set(MV_TUNNEL_MACH_PREFIX "dev")
endif()
configure_file("${_tun}/Info.plist.in" "${CMAKE_BINARY_DIR}/packaging/tunnel/Info.plist" @ONLY)

# swiftc directly (as the FCP extension's objc is built by the C++ toolchain):
# one module, the system's frameworks only, the app's architectures.
set(_tun_exe "${CMAKE_BINARY_DIR}/tunnel/DriftwireTunnel")
set(_tun_archs ${CMAKE_OSX_ARCHITECTURES})
if(NOT _tun_archs)
  set(_tun_archs ${CMAKE_SYSTEM_PROCESSOR})
endif()
set(_tun_slices "")
foreach(arch IN LISTS _tun_archs)
  if(arch STREQUAL "aarch64")
    set(arch arm64)
  endif()
  set(_slice "${CMAKE_BINARY_DIR}/tunnel/DriftwireTunnel-${arch}")
  add_custom_command(OUTPUT "${_slice}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tunnel"
    COMMAND xcrun swiftc -O -module-name DriftwireTunnel
            -target ${arch}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}
            -framework NetworkExtension -o "${_slice}"
            "${_tun}/main.swift" "${_tun}/PacketTunnelProvider.swift"
    DEPENDS "${_tun}/main.swift" "${_tun}/PacketTunnelProvider.swift"
    COMMENT "Driftwire VPN provider (${arch})"
    VERBATIM)
  list(APPEND _tun_slices "${_slice}")
endforeach()
add_custom_command(OUTPUT "${_tun_exe}"
  COMMAND lipo -create ${_tun_slices} -output "${_tun_exe}"
  DEPENDS ${_tun_slices}
  COMMENT "DriftwireTunnel"
  VERBATIM)
add_custom_target(mv_driftwire_tunnel_sysext ALL DEPENDS "${_tun_exe}")

set(MV_TUNNEL_ASSEMBLE_ARGS
  --tunnel-sysext-exe "${_tun_exe}"
  --tunnel-sysext-plist "${CMAKE_BINARY_DIR}/packaging/tunnel/Info.plist")
set(MV_TUNNEL_ASSEMBLE_DEPENDS mv_driftwire_tunnel_sysext)
