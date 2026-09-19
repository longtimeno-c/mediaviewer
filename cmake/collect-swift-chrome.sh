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

# Real bug, found the hard way (2026-09-18): this used to early-exit here
# whenever both destination files already existed, on the theory that meant
# "classic SwiftPM already wrote straight to the flat path, nothing to do."
# It does not mean that under the Xcode-style "Swift Build" backend this
# toolchain uses: `swift build -c release` writes a fresh libMediaViewerChrome.a
# straight to $lib_dst itself (a compatibility path apparently, since
# collect-swift-chrome.sh is never asked to touch it), but never touches
# $header_dst at all -- so a *second* build (both destination files already
# present from the first) hit the early exit and silently kept serving the
# first build's stale header forever, however many times ChromeHost.swift
# changed afterwards. Always finding and copying the header fresh, every
# time this script runs, is the fix -- `cp` is cheap, and "maybe stale" is
# a worse failure mode than "recopied a file that happened not to change."
header_src=$(find "$build_dir" -name "*-Swift.h" -path "*GeneratedModuleMaps*" -print -quit)
if [ -z "$header_src" ]; then
  header_src=$(find "$build_dir" -name "*-Swift.h" -print -quit)
fi
if [ -z "$header_src" ]; then
  echo "collect-swift-chrome.sh: no generated ObjC header found under $build_dir" >&2
  exit 1
fi
if [ "$header_src" != "$header_dst" ]; then
  mkdir -p "$(dirname "$header_dst")"
  cp "$header_src" "$header_dst"
fi

# The lib, observed empirically, already lands at $lib_dst as part of
# `swift build` itself on this toolchain -- this is the fallback for a
# toolchain where that is not true, not the common path.
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
