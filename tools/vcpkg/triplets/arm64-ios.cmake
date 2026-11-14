# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# An iPhone or iPad: everything static (no downloaded code, docs/plans/mobile.md
# section 8.1). Keep VCPKG_OSX_DEPLOYMENT_TARGET in step with
# CMAKE_OSX_DEPLOYMENT_TARGET in the root CMakeLists.txt and arm64-ios-simulator.cmake.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_SYSROOT iphoneos)
set(VCPKG_OSX_DEPLOYMENT_TARGET 18.0)

# Autotools ports (libsodium) would otherwise take this for a native build and
# try to run what they compile.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-ios;--build=aarch64-apple-darwin")
