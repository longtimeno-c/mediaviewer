# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# The stock x64-windows triplet, minus compiler tracking.
#
# GitHub's windows-latest pool mixes image versions with different MSVC
# builds. With tracking on, a job whose MSVC differs from the one that filled
# the shared dependency cache gets different package ABIs and rebuilds every
# port (FFmpeg included) inside Configure, which blows the 40-minute job
# limit. Ports are pinned by the vcpkg baseline and the cache key; the
# compiler patch level is not part of what we ship.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_DISABLE_COMPILER_TRACKING ON)

# GitHub's Windows images moved to Visual Studio 2026 (2026-11), whose MSVC
# 14.5x cannot build the pinned dav1d 1.5.4 (C7780: __has_include_next in its
# stdatomic.h shim): the 0.1.46 release failed in Configure. Those images
# still carry Visual Studio 2022's last toolset, so the ports build with it on
# every image until the baseline moves to a dav1d that builds with the new one.
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.44)

# BoringSSL (the Driftwire Tunnel's TLS, slice 4) links into the Driftwire
# engine statically, as on the Mac and Linux: no ssl.dll / crypto.dll for the
# add-on pack or the driftwire zip to carry, and none for anything else to
# pick up by name. Apache-2.0; its notice ships in the packages' copyright.
if(PORT STREQUAL "boringssl")
  set(VCPKG_LIBRARY_LINKAGE static)
endif()
