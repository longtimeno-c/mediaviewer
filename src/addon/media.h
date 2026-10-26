// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pixels for add-ons (host table v2, Milestone H / docs/design/17 "Frame sampling").
//
// The types are portable and header-only so src/addon (the host table) knows
// them without linking a decoder. The functions that fill them are the host's:
// abi/addon_media.h (mv_addon_media), over the viewer's own still pipeline and
// the clip core's FFmpeg pieces; each host installs them into its
// host_services. addon/ may not include image/ or edit/ (docs/design/02).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
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

// #297 (Enhance): a file's sound at full quality, interleaved float PCM.
struct sound_facts {
  std::uint32_t channels = 1;
  std::uint32_t sample_rate = 0;
  std::uint32_t source_rate = 0;
  std::uint32_t source_channels = 0;
  bool has_picture = false;
  std::int64_t duration_ms = 0;
};

class sound_stream {
 public:
  virtual ~sound_stream() = default;
  [[nodiscard]] virtual const sound_facts& facts() const noexcept = 0;
  // Up to `max_frames` frames; empty at the end.
  [[nodiscard]] virtual result<std::vector<float>> read(std::size_t max_frames) = 0;
};

// A new file beside a source with sound the add-on made (edit/sound_writer.h).
class sound_sink {
 public:
  virtual ~sound_sink() = default;
  [[nodiscard]] virtual bool with_picture() const noexcept = 0;
  [[nodiscard]] virtual expected write(std::span<const float> interleaved) = 0;
  // Publishes; the new file's path.
  [[nodiscard]] virtual result<std::string> finish() = 0;
};

// #297 (Enhance): a clip's frames, and a new clip from new frames.
struct picture_facts {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t rotation = 0;
  bool hdr = false;
  double fps = 0;
  std::int64_t duration_ms = 0;
  std::int64_t frame_count = 0;
};

class picture_source {
 public:
  virtual ~picture_source() = default;
  [[nodiscard]] virtual const picture_facts& facts() const noexcept = 0;
  // The next frame (RGB, width x height x 3); false at the end.
  [[nodiscard]] virtual result<bool> next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us) = 0;
};

struct picture_out {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t sound_rate = 0;  // 0: the source's sound, copied
  std::uint32_t sound_channels = 0;
  std::string suffix;
};

class picture_sink {
 public:
  virtual ~picture_sink() = default;
  [[nodiscard]] virtual expected write_frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us) = 0;
  [[nodiscard]] virtual expected write_sound(std::span<const float> interleaved) = 0;
  [[nodiscard]] virtual result<std::string> finish() = 0;
};

// #354: a document's text, one paragraph (or line) at a time, in reading
// order: the page (0-based) where it starts and the document's page count.
struct document_line {
  std::uint32_t page = 0;
  std::uint32_t page_count = 1;
  std::string text;  // UTF-8
};

}  // namespace mv::addon
