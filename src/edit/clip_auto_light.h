// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's Auto light (docs/plans/video-editor-look.md, slice 2):
// #289's auto tone on one frame of the clip, the one at the playhead.
//
// The frame's 8-bit codes go through the look's colour minus exposure and
// contrast (auto_tone_basis) as the canvas would show them (BT.1886 decode,
// clip_look.h colour_table), are binned as displayed sRGB codes, and
// auto_tone fits exposure and contrast to that histogram. Pure; a worker
// decodes the frame (clip::frame_rgb_at), never the UI thread.
#pragma once

#include <cstdint>
#include <span>

#include "edit/adjust.h"
#include "edit/histogram.h"

namespace mv::edit::clip {

// `rgb`: width x height x 3, tightly packed.
[[nodiscard]] histogram displayed_histogram(std::span<const std::uint8_t> rgb, const colour& basis);

// `current` with exposure and contrast set for this frame; `changed` false
// when the frame is flat or already fills the range.
[[nodiscard]] colour auto_light(std::span<const std::uint8_t> rgb, const colour& current, bool& changed);

}  // namespace mv::edit::clip
