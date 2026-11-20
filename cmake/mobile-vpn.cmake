# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The phone's VPN through the person's own server (docs/plans/phone-vpn.md): the C ABI in
# src/mobile/mv_mobile_vpn.h, in two static libraries so each process links only its half.
#
#   mv_mobile_vpn_code  the app's: the pasted VPN code and the knock key (Argon2id, 64 MiB).
#   mv_mobile_vpn       the packet tunnel extension's: Driftwire's shared VPN (vpn::runner,
#                       netstack, tunnel::dialer, the sealed hop; BoringSSL for the HTTPS look
#                       when the build has it) behind NEPacketTunnelFlow. Not the catalogue,
#                       search, ONNX Runtime or SQLite (§1.1: the extension's footprint).
#
# Built on iOS for src.ios, and on the Mac, where tests/test_phone_vpn.cpp runs it against a relay
# on loopback. Nothing here is Apple-only.
if(TARGET mv_mobile_vpn)
  return()
endif()
set(_MV "${MV_SOURCE_ROOT}/src/mobile")
add_library(mv_mobile_vpn_code STATIC ${_MV}/vpn_code.cpp ${_MV}/mv_mobile_vpn.h)
target_include_directories(mv_mobile_vpn_code PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_mobile_vpn_code PUBLIC mv_project_options mv_wire PRIVATE ${MV_SODIUM_TARGET})

add_library(mv_mobile_vpn STATIC ${_MV}/vpn_abi.cpp ${_MV}/mv_mobile_vpn.h)
target_include_directories(mv_mobile_vpn PUBLIC "${MV_SOURCE_ROOT}/src")
target_link_libraries(mv_mobile_vpn PUBLIC mv_project_options mv_mobile_vpn_code mv_vpn_engine PRIVATE ${MV_SODIUM_TARGET})
