// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pixels for add-ons (host table v2, Milestone H / plan/17 "Frame sampling").
//
// The types are portable and header-only so src/addon (the host table) knows
// them without linking a decoder. The functions that fill them are the host's:
// abi/addon_media.h (mv_addon_media), over the viewer's own still pipeline and
// the clip core's FFmpeg pieces; each host installs them into its
// host_services. addon/ may not include image/ or edit/ (plan/02).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::addon {

// 8-bit sRGB, RGB, tightly packed (stride = width * 3).
struct rgb_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
};

struct sampler_options {
  std::uint32_t min_gap_ms = 500;
  std::uint32_t max_gap_ms = 2000;
  std::uint32_t max_long_edge = 512;
  std::int64_t start_ms = 0;
};

struct video_facts {
  std::int64_t duration_ms = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool hdr = false;
};

struct sampled_frame {
  rgb_image image;
  std::int64_t pts_ms = 0;
  std::int64_t pts_tb = 0;
  std::int32_t tb_num = 0;
  std::int32_t tb_den = 1;
  bool keyframe = false;   // false: decoded forward to fill a max_gap hole
  bool end = false;        // no frame; the clip is done
};

// One clip, one decoder instance of its own (never the playback decoder).
class video_sampler {
 public:
  virtual ~video_sampler() = default;
  [[nodiscard]] virtual const video_facts& facts() const noexcept = 0;
  [[nodiscard]] virtual result<sampled_frame> next() = 0;
};

// A clip's soundtrack, mono float PCM (the audio index).
class audio_stream {
 public:
  virtual ~audio_stream() = default;
  [[nodiscard]] virtual std::int64_t duration_ms() const noexcept = 0;
  // Up to `max_samples`; empty at the end. `start_ms`: the first sample's time.
  [[nodiscard]] virtual result<std::vector<float>> read(std::size_t max_samples,
                                                        std::int64_t& start_ms) = 0;
};

}  // namespace mv::addon
