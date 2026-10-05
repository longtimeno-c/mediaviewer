// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Frames sampled from a clip for the local search index (docs/design/17 "Frame
// sampling", Milestone H PR 21). A decoder instance of its own on the
// calling worker, never the player's: software decode, like every clip job
// (clip_encode.cpp), so it never takes a surface from the canvas's pool.
//
//  1. Keyframes: the decoder skips everything else (skip_frame = NONKEY),
//     so a phone clip's ~1 s GOPs give ~1 candidate a second for the cost
//     of its I-frames.
//  2. Coverage: a GOP longer than max_gap is decoded in full from its
//     keyframe (packets kept since that keyframe) and filled on a max_gap
//     grid. min_gap thins scene-cut bursts.
//  3. Pixels: 8-bit sRGB RGB, box-fitted to max_long_edge with the pixel
//     aspect applied and the display rotation turned in; PQ / HLG through
//     the same SDR tone map as the canvas (D6, docs/design/17 step 4).
//
// No FFmpeg type crosses this header.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "edit/clip.h"

namespace mv::edit::clip {

struct sample_options {
  std::uint32_t min_gap_ms = 500;
  std::uint32_t max_gap_ms = 2000;
  std::uint32_t max_long_edge = 512;
  std::int64_t start_ms = 0;  // resume: nothing before this
  // Packets kept from a keyframe for a grid fill; a longer GOP is sampled at
  // its keyframes only (bounded memory beats full coverage of a broken file).
  std::size_t max_gop_bytes = 256u << 20;
  int decoder_threads = 2;
};

struct sample_rgb {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;  // stride = width * 3
};

struct sampled {
  sample_rgb image;
  std::int64_t pts_ms = 0;  // on the player's timeline (container origin removed)
  std::int64_t pts_tb = 0;  // the stream timestamp
  std::int32_t tb_num = 0;
  std::int32_t tb_den = 1;
  bool keyframe = false;
  bool end = false;
};

struct sample_facts {
  std::int64_t duration_ms = 0;
  std::uint32_t width = 0;   // display size, rotation applied
  std::uint32_t height = 0;
  bool hdr = false;
};

class frame_sampler {
 public:
  ~frame_sampler();
  frame_sampler(const frame_sampler&) = delete;
  frame_sampler& operator=(const frame_sampler&) = delete;

  [[nodiscard]] static result<std::unique_ptr<frame_sampler>> open(
      std::string_view utf8_path, const sample_options& options,
      const std::atomic<bool>* cancel = nullptr);

  [[nodiscard]] const sample_facts& facts() const noexcept;
  // The next frame in time order; `end` set once the clip is exhausted.
  [[nodiscard]] result<sampled> next();

  struct impl;

 private:
  explicit frame_sampler(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

// A clip's soundtrack as mono float PCM at `sample_rate` (Milestone H audio
// index: CLAP wants 48 kHz, Whisper 16 kHz). Its own decoder on the calling
// worker, never the player's. Channels are averaged; the first audio stream
// the demuxer calls best is the one read.
struct audio_facts {
  std::int64_t duration_ms = 0;
  bool has_audio = false;
};

class audio_reader {
 public:
  ~audio_reader();
  audio_reader(const audio_reader&) = delete;
  audio_reader& operator=(const audio_reader&) = delete;

  // status::unsupported_format when the file has no audio stream.
  [[nodiscard]] static result<std::unique_ptr<audio_reader>> open(
      std::string_view utf8_path, std::uint32_t sample_rate, std::int64_t start_ms,
      const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] const audio_facts& facts() const noexcept;
  // Up to `max_samples` more samples; `start_ms` is the first one's time on
  // the player's timeline. An empty result is the end of the stream.
  [[nodiscard]] result<std::vector<float>> read(std::size_t max_samples, std::int64_t& start_ms);

  struct impl;

 private:
  explicit audio_reader(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

// The frame on screen at `at_ms` (the last whose pts is not after it),
// decoded forward from the keyframe before it, fitted to max_long_edge.
[[nodiscard]] result<sample_rgb> frame_rgb_at(std::string_view utf8_path, std::int64_t at_ms,
                                              std::uint32_t max_long_edge,
                                              const std::atomic<bool>* cancel = nullptr);

}  // namespace mv::edit::clip
