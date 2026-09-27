// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The macOS encode port: VideoToolbox (plan/10 PR 13, plan/15). FFmpeg's
// h264_videotoolbox / hevc_videotoolbox drive a VTCompressionSession; the
// port is built with --enable-videotoolbox on Darwin. Both Apple Silicon and
// the Intel Macs in the universal app (plan/12 2026-09-24) have a hardware
// H.264 encoder; HEVC is hardware on every Mac this app supports.
#include "edit/hwencode.h"

namespace mv::edit::hwencode {
namespace {

constexpr const char* kH264[] = {"h264_videotoolbox"};
constexpr const char* kHevc[] = {"hevc_videotoolbox"};

}  // namespace

std::span<const char* const> candidates(codec c) noexcept {
  return c == codec::hevc ? std::span<const char* const>(kHevc) : std::span<const char* const>(kH264);
}

}  // namespace mv::edit::hwencode

#include "edit/hwencode_label.inc"
