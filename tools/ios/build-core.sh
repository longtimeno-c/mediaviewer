#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Xcode's run-script phase for the MediaViewer target: builds the static C++ core
# (docs/plans/mobile.md 8.1) for this SDK into build-ios-<platform>/. The link flags in the
# project name those libraries. VCPKG_ROOT defaults to ~/Dev/.mv-toolchain/vcpkg.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${PLATFORM_NAME:-iphonesimulator}"
build="$root/build-ios-$sdk"
# Xcode's environment has neither Homebrew nor the Python-installed tools.
export PATH="/opt/homebrew/bin:$HOME/Library/Python/3.9/lib/python/site-packages/cmake/data/bin:$HOME/Library/Python/3.9/bin:$PATH"
vcpkg="${VCPKG_ROOT:-$HOME/Dev/.mv-toolchain/vcpkg}"
if [ ! -f "$build/build.ninja" ]; then
  # The build folder's name follows CMAKE_OSX_SYSROOT, so the SDKs do not share a tree.
  cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake"
fi
exec ninja -C "$build" mv_mobile_link mv_mobile_search mv_infer mv_model_fetch mv_driftwire_engine
