#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# `swift build`'s on-disk layout under --build-path is not a stable contract
# across Swift toolchain versions (classic SwiftPM vs. the newer Xcode-style
# "Swift Build" backend land the umbrella ObjC header and static lib in
# different places — cmake/darwin.cmake's comment has the details). This
# script finds the real output under $1 and copies it to the flat paths $2
# (header) and $3 (lib) the rest of the CMake build depends on, so the exact
# generated-file layout stays a wrapper-script concern, not a CMakeLists.txt
# or main_mac.mm one.
set -eu

build_dir="$1"
header_dst="$2"
lib_dst="$3"

# Already at the flat, expected path (classic SwiftPM layout) — nothing to do.
if [ -f "$header_dst" ] && [ -f "$lib_dst" ]; then
  exit 0
fi

header_src=$(find "$build_dir" -name "*-Swift.h" -path "*GeneratedModuleMaps*" -print -quit)
if [ -z "$header_src" ]; then
  header_src=$(find "$build_dir" -name "*-Swift.h" -print -quit)
fi
if [ -z "$header_src" ]; then
  echo "collect-swift-chrome.sh: no generated ObjC header found under $build_dir" >&2
  exit 1
fi
cp "$header_src" "$header_dst"

if [ ! -f "$lib_dst" ]; then
  lib_src=$(find "$build_dir" -name "lib*.a" -path "*Products*Release*" -print -quit)
  if [ -z "$lib_src" ]; then
    lib_src=$(find "$build_dir" -name "lib*.a" ! -path "*Objects-normal*" -print -quit)
  fi
  if [ -z "$lib_src" ]; then
    echo "collect-swift-chrome.sh: no static library found under $build_dir" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$lib_dst")"
  cp "$lib_src" "$lib_dst"
fi
