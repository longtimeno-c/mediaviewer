// SPDX-License-Identifier: GPL-2.0-or-later
// The Windows encode port: NVENC, Quick Sync, AMF, then Media Foundation
// (whose MFT is itself usually the GPU vendor's). plan/11: the OS/GPU encoder,
// never a bundled software one. The FFmpeg port is built with the nvcodec,
// qsv and amf features (vcpkg.json); the MF encoders come with the port's
// --enable-mediafoundation. Drivers are loaded at open, so a machine without
// that vendor's GPU fails the open and clip.cpp moves on.
#include "edit/hwencode.h"

namespace mv::edit::hwencode {
namespace {

constexpr const char* kH264[] = {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
constexpr const char* kHevc[] = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf"};

}  // namespace

std::span<const char* const> candidates(codec c) noexcept {
  return c == codec::hevc ? std::span<const char* const>(kHevc) : std::span<const char* const>(kH264);
}

}  // namespace mv::edit::hwencode

#include "edit/hwencode_label.inc"
