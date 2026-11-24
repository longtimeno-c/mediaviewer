# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The VPN add-on (docs/plans/vpn-addon.md, owner 2026-11-19): the Tunnel, All
# traffic and Connect to a VPN host, which were Driftwire's, and hosting the
# VPN on this computer or the claimed server, which was Phone access's card.
# Included by cmake/driftwire.cmake (the driftwire program's /vpn links the
# engine), so every build that has Driftwire has this.
#
#   mv_vpn_engine  the proxy (tunnel.*), the packet stack and the VPN's runner
#                  (netstack.*, vpn.*), the system proxy's record and port
#                  (sysproxy*), the service and its page (service.*, tree.cpp,
#                  prefs.*), over mv_wire. No io sources: the tests bring mv_io,
#                  the module and the program compile the file port in.
#   mv_vpn         the add-on: ONE shared library whose only export is
#                  mv_addon_get, with the OS's virtual interface (vpn_<os>);
#                  build/addons/vpn, packed and signed by tools/package/addon-pack.py.
#
# The Mac's packet tunnel provider, which macOS loads only from the app's own
# bundle, is cmake/darwin-tunnel.cmake. iOS has no add-ons: the engine only
# (the phone's own VPN, src/mobile, uses the proxy, the stack and the runner).

if(TARGET mv_vpn_engine)
  return()
endif()
if(NOT DEFINED MV_SOURCE_ROOT)
  set(MV_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/wire.cmake")
set(_V "${MV_SOURCE_ROOT}/src/addons/vpn")
set(MV_VPN_VERSION "1.0.0" CACHE STRING "VPN add-on version (its manifest's)")

if(WIN32)
  set(_vpn_sysproxy ${_V}/sysproxy_win.cpp)
  set(_vpn_os ${_V}/vpn_win.cpp)
elseif(IOS)
  set(_vpn_sysproxy ${_V}/sysproxy_none.cpp)
  set(_vpn_os "")
elseif(APPLE)
  set(_vpn_sysproxy ${_V}/sysproxy_mac.cpp)
  set(_vpn_os ${_V}/vpn_mac.mm)
else()
  set(_vpn_sysproxy ${_V}/sysproxy_none.cpp)
  set(_vpn_os ${_V}/vpn_none.cpp)
endif()

add_library(mv_vpn_engine STATIC
  ${_V}/tunnel.cpp ${_V}/tunnel.h
  ${_V}/netstack.cpp ${_V}/netstack.h
  ${_V}/vpn.cpp ${_V}/vpn.h
  ${_V}/sysproxy.h ${_V}/sysproxy_record.cpp ${_vpn_sysproxy}
  ${_V}/prefs.cpp ${_V}/prefs.h
  ${_V}/service.cpp ${_V}/service.h
  ${_V}/tree.cpp
)
target_include_directories(mv_vpn_engine PUBLIC "${MV_SOURCE_ROOT}/src" "${MV_SOURCE_ROOT}/src/abi/include")
target_link_libraries(mv_vpn_engine PUBLIC mv_project_options mv_wire ${MV_SODIUM_TARGET} Threads::Threads)
set_target_properties(mv_vpn_engine PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(IOS)
  return()  # no add-on module on iOS
endif()

if(WIN32)
  # wininet: the Tunnel's system proxy, this user's setting (sysproxy_win.cpp).
  target_link_libraries(mv_vpn_engine PUBLIC wininet)
elseif(APPLE)
  # SystemConfiguration: the system proxy (sysproxy_mac.cpp).
  target_link_libraries(mv_vpn_engine PUBLIC "-framework CoreFoundation" "-framework SystemConfiguration")
endif()

if(WIN32)
  set(_vpn_io ${MV_SOURCE_ROOT}/src/io/file_port_win.cpp)
else()
  set(_vpn_io ${MV_SOURCE_ROOT}/src/io/file_port_mac.cpp)
endif()
add_library(mv_vpn MODULE
  ${_V}/addon_entry.cpp
  ${_vpn_os}
  # The file port, compiled in: the add-on links nothing of the core.
  ${MV_SOURCE_ROOT}/src/io/file_port.cpp
  ${_vpn_io}
)
target_link_libraries(mv_vpn PRIVATE mv_vpn_engine)
if(WIN32)
  # iphlpapi: the VPN's address, routes and DNS on the Wintun adapter (vpn_win.cpp).
  # fwpuclnt: the VPN's kill switch (WFP filters, vpn_win.cpp).
  target_link_libraries(mv_vpn PRIVATE ole32 iphlpapi fwpuclnt)
elseif(APPLE)
  # NetworkExtension, SystemExtensions: the VPN (vpn_mac.mm).
  target_link_libraries(mv_vpn PRIVATE "-framework Foundation" "-framework NetworkExtension"
    "-framework SystemExtensions" "-framework Security")
  set_source_files_properties(${_V}/vpn_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
target_compile_definitions(mv_vpn PRIVATE MV_VPN_VERSION="${MV_VPN_VERSION}")
set_target_properties(mv_vpn PROPERTIES
  OUTPUT_NAME "mv_vpn"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/vpn"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/vpn")
if(APPLE)
  set_target_properties(mv_vpn PROPERTIES PREFIX "lib" SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(mv_vpn PROPERTIES PREFIX "")
endif()
