// SPDX-License-Identifier: GPL-2.0-or-later
// PR 30 (the Video Editor, plan/21): what a timeline draws -- a strip of
// thumbnails and a waveform -- for one clip. Worker thread only: both decode.
// Neither writes anything or logs a path (rules 5, 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "edit/clip.h"

namespace mv::edit::clip {

struct strip_frame {
  time_ns at_ns = 0;     // the time asked for
  time_ns shown_ns = 0;  // the frame's own time (the keyframe at or before `at`)
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // sRGB, display-rotated, width * height * 4
};

// One thumbnail per time, `height` pixels tall (the width follows the
// display aspect), from the keyframe at or before each time: a strip is for
// finding a place, so it never decodes forward. HDR is tone-mapped like the
// canvas. Times must ascend. At most 512 per call.
[[nodiscard]] result<std::vector<strip_frame>> thumbnails(std::string_view utf8_path,
                                                          std::span<const time_ns> times, std::uint32_t height,
                                                          const std::atomic<bool>* cancel = nullptr);

// The first audio track as `buckets` peaks across the whole clip, each the
// largest |sample| (0..1, all channels) in its slice of time. Empty for a
// clip with no audio. At most 65536 buckets.
[[nodiscard]] result<std::vector<float>> audio_peaks(std::string_view utf8_path, std::uint32_t buckets,
                                                     const std::atomic<bool>* cancel = nullptr);

}  // namespace mv::edit::clip
