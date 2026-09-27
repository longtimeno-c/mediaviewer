// SPDX-License-Identifier: GPL-2.0-or-later
// Pixels for add-ons (host table v2, Milestone H / plan/17 "Frame sampling").
//
// The types are portable and header-only so src/addon (the host table) knows
// them without linking a decoder. The implementation that fills them lives in
// mv_addon_media (media.cpp), over the viewer's own still pipeline and the
// clip core's FFmpeg pieces; each host installs it into its host_services.
// Worker threads only: every call reads and decodes.
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

// The implementation (media.cpp, mv_addon_media). Portable: FFmpeg, the
// viewer's still decoders and its JPEG-512 cache; no GPU, no OS UI.
namespace media {
[[nodiscard]] result<rgb_image> decode_still(const std::string& path, std::uint32_t max_long_edge);
[[nodiscard]] result<std::unique_ptr<video_sampler>> open_sampler(const std::string& path,
                                                                  const sampler_options& options);
[[nodiscard]] result<rgb_image> video_frame(const std::string& path, std::int64_t pts_ms,
                                            std::uint32_t max_long_edge);
// The JPEG-512 cache entry for a moment: stores `image` when given, else
// looks one up (status::io on a miss). Returns the JPEG's path.
[[nodiscard]] result<std::string> moment_thumbnail(const std::string& path, std::int64_t pts_ms,
                                                   const rgb_image* image);
}  // namespace media

}  // namespace mv::addon
