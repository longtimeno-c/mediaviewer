// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip's picture, frame by frame, and a new clip from new frames (Enhance,
// #297 slice 5): the host's side of the add-on table's frames_* and
// clip_write_* rows.
//
// frame_reader: every frame in display order as 8-bit sRGB RGB, at the coded
// size (the display rotation is carried to the output, not applied), on a
// decoder of its own on the calling worker (never the player's).
//
// picture_writer: "<stem>-<suffix>.<ext>" beside the source, its frames
// encoded on a HARDWARE encoder (edit/hwencode.h, docs/design/11: no x264,
// no software HEVC) at the size given, BT.709, the source's frame times and
// rotation. Its sound is the source's, packet-copied, or new sound the caller
// pushes (the OS's AAC, else FLAC: clip_audio_enc.h). MP4 and MOV keep their
// family, anything else is Matroska. Hidden until finish() publishes it under
// a free name; destroyed before that it leaves nothing. Rule 5: the source is
// only read. Rule 6: nothing here logs a path. Worker threads only.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::edit::clip {

struct frames_facts {
  std::uint32_t width = 0;   // coded size: what read() returns
  std::uint32_t height = 0;
  std::uint32_t rotation = 0;  // clockwise display rotation, carried to the output
  bool hdr = false;            // PQ / HLG
  double fps = 0;
  std::int64_t duration_ms = 0;
  std::int64_t frame_count = 0;  // from the container, else duration x rate; an estimate
};

class frame_reader {
 public:
  ~frame_reader();
  frame_reader(const frame_reader&) = delete;
  frame_reader& operator=(const frame_reader&) = delete;

  // unsupported_format when the file has no picture.
  [[nodiscard]] static result<std::unique_ptr<frame_reader>> open(std::string_view utf8_path,
                                                                  const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] const frames_facts& facts() const noexcept;
  // The next frame into `rgb` (width x height x 3, tightly packed);
  // `pts_us` is its time on the player's timeline. false at the end.
  [[nodiscard]] result<bool> next(std::vector<std::uint8_t>& rgb, std::int64_t& pts_us);

  struct impl;

 private:
  explicit frame_reader(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

struct picture_out_options {
  std::uint32_t width = 0;   // even; at most 8192
  std::uint32_t height = 0;
  // 0: the source's own sound is packet-copied. Else new sound at this rate
  // and channel count comes through write_sound().
  std::uint32_t sound_rate = 0;
  std::uint32_t sound_channels = 0;
  std::string suffix = "enhance";  // [a-z0-9_-]{1,32}
  // Tests only: these FFmpeg encoders instead of the hardware list, and one
  // FFmpeg does not mark hardware may open. The product never sets them.
  std::vector<std::string> encoders;
  bool allow_software = false;
};

class picture_writer {
 public:
  ~picture_writer();
  picture_writer(const picture_writer&) = delete;
  picture_writer& operator=(const picture_writer&) = delete;

  // unsupported_format when no hardware encoder opens at that size (or the
  // new sound has no encoder).
  [[nodiscard]] static result<std::unique_ptr<picture_writer>> open(std::string_view source_utf8,
                                                                    const picture_out_options& options,
                                                                    const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] const char* encoder() const noexcept;
  // One frame, RGB, width x height x 3 with `stride` bytes a row, at
  // `pts_us` on the source's timeline (frames in order).
  [[nodiscard]] expected write_frame(const std::uint8_t* rgb, std::uint32_t stride, std::int64_t pts_us);
  // New sound (when sound_rate was set), interleaved float, in order. Keep it
  // near the frames written: the muxer interleaves what it is given.
  [[nodiscard]] expected write_sound(std::span<const float> interleaved);
  // Flushes, writes the trailer and publishes. Returns the published path.
  [[nodiscard]] result<std::string> finish();

  struct impl;

 private:
  explicit picture_writer(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

}  // namespace mv::edit::clip
