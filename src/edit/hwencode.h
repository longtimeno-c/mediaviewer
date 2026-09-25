// SPDX-License-Identifier: GPL-2.0-or-later
// The encode port (plan/10 PR 13): which HARDWARE encoders Path 2 may use.
//
// Portable header. edit/hwencode_win.cpp lists NVENC, Quick Sync, AMF and the
// Media Foundation encoder; edit/hwencode_mac.cpp lists VideoToolbox
// (FFmpeg's h264/hevc_videotoolbox, which are VTCompressionSession); the
// headless core build (cmake/portable) links edit/hwencode_none.cpp and has
// none. Candidates are FFmpeg encoder names in preference order; clip.cpp
// opens the first one that accepts the clip's size and pixel format, so a
// machine without that GPU just falls through to the next.
//
// Licence (plan/11, CLAUDE.md "Video"): no x264 / x265, no software HEVC
// encoder, no --enable-gpl. clip.cpp refuses any encoder FFmpeg does not mark
// hardware or hybrid, whatever a port lists.
#pragma once

#include <span>

namespace mv::edit::hwencode {

enum class codec : unsigned char { h264, hevc };

// Static storage; never empty strings. May be an empty span.
[[nodiscard]] std::span<const char* const> candidates(codec c) noexcept;

// For the job panel's label ("Re-encode (slower) — NVENC").
[[nodiscard]] const char* family_label(const char* encoder_name) noexcept;

}  // namespace mv::edit::hwencode
