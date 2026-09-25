// SPDX-License-Identifier: GPL-2.0-or-later
// Clip editing: PR 13 (two-path trim) and PR 14 (extract & remux).
// plan/08-video-editing.md, plan/10 Milestone E, D7.
//
// Everything a host offers on a clip is one `request` run by `run()` on a
// worker (the clip job queue, edit/clip_jobs.h). Hosts only add UI: the op
// graph, the keyframe grid, the snapping and the output naming are here, so
// the same request on Windows and macOS writes the same file.
//
//   trim_keyframe  Path 1. Stream copy from the keyframe at or before `in` to
//                  the keyframe at or after `out`. No re-encode, seconds for
//                  any length, output size proportional to the range.
//   trim_reencode  Path 2. Frame-accurate [in, out): video decoded from the
//                  keyframe before `in` and re-encoded on a HARDWARE encoder
//                  from the encode port (edit/hwencode.h); audio stream-copied.
//                  Labelled slower. Smart cut is v1.1 (D7).
//   rotate         Lossless: a new display matrix, no re-encode.
//   split          Two files at the keyframe nearest `in`.
//   remove_middle  One file without [in, out), both edges on keyframes.
//   remux          MKV <-> MP4, every stream the target container can hold.
//   frame          The frame shown at `in`, as PNG or JPEG, sRGB.
//   audio          The first audio track: stream copy, WAV or FLAC.
//   animation      [in, out) as GIF (two-pass palette) or animated WebP.
//
// Rule 5: the source is opened read-only and never written. Outputs go beside
// the source (or into `out_dir`) under a name that is free when published;
// they are written to a hidden `.mvpart` sibling first, so a cancel or a
// failure leaves no partial output (plan/10 PR 13 verify).
// Rule 6: nothing here logs a path.
//
// Times are the PLAYER's timeline (player::time_ns): nanoseconds from the
// container start_time, so a position read off the transport is an `in`.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::edit::clip {

using time_ns = std::int64_t;

// ---- probe + keyframe index (the scrub-bar grid) ----------------------------

struct clip_info {
  time_ns duration_ns = 0;
  // Presentation times of the video keyframes, ascending, on the player's
  // timeline. Empty for an audio-only file.
  std::vector<time_ns> keyframes_ns;
  bool has_video = false;
  bool has_audio = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  double frame_rate = 0.0;
  // Clockwise display rotation from the container matrix: 0, 90, 180, 270.
  int rotation = 0;
  int bit_depth = 8;
  bool hdr = false;  // PQ or HLG transfer
  std::string video_codec;  // "h264", "hevc", ...
  std::string container;    // muxer family: mp4, mov, matroska, webm, avi, mpegts
};

// Opens the file, reads the demuxer's index when it is complete (MP4/MOV
// carry every sync sample) and otherwise walks the packets without decoding.
// Worker thread only. `cancel` may be null.
[[nodiscard]] result<clip_info> probe(std::string_view utf8_path,
                                      const std::atomic<bool>* cancel = nullptr);

// Snapping on a keyframe list. With no keyframes (audio only) the time is
// returned unchanged. `at_or_after` returns `end` when no keyframe follows.
[[nodiscard]] time_ns keyframe_at_or_before(const std::vector<time_ns>& kf, time_ns t) noexcept;
[[nodiscard]] time_ns keyframe_at_or_after(const std::vector<time_ns>& kf, time_ns t,
                                           time_ns end) noexcept;
[[nodiscard]] time_ns keyframe_nearest(const std::vector<time_ns>& kf, time_ns t) noexcept;

// What Path 1 will actually write for a requested [in, out): the A-B loop
// preview uses this, so the preview is exactly the output (plan/08).
struct range {
  time_ns in_ns = 0;
  time_ns out_ns = 0;
};
[[nodiscard]] range keyframe_range(const clip_info& info, time_ns in_ns, time_ns out_ns) noexcept;

// ---- operations --------------------------------------------------------------

enum class op : std::uint8_t {
  trim_keyframe = 1,
  trim_reencode = 2,
  rotate = 3,
  split = 4,
  remove_middle = 5,
  remux = 6,
  frame = 7,
  audio = 8,
  animation = 9,
};

enum class remux_target : std::uint8_t { mp4 = 1, mkv = 2 };
enum class frame_format : std::uint8_t { png = 1, jpeg = 2 };
enum class audio_format : std::uint8_t { copy = 1, wav = 2, flac = 3 };
enum class anim_format : std::uint8_t { gif = 1, webp = 2 };

// A GIF / WebP longer than this is refused: that is a clip, not an animation.
inline constexpr time_ns kMaxAnimationNs = 60'000'000'000;

struct request {
  op kind = op::trim_keyframe;
  std::string source;   // UTF-8
  std::string out_dir;  // UTF-8; empty = beside the source
  // trim / remove_middle / animation / audio: the range. out_ns < 0 means
  // the end of the clip. split: in_ns is the split point. frame: in_ns is the
  // frame's time.
  time_ns in_ns = 0;
  time_ns out_ns = -1;
  int rotate_degrees = 90;  // rotate: clockwise delta, a multiple of 90
  remux_target remux = remux_target::mp4;
  frame_format frame = frame_format::png;
  audio_format audio = audio_format::copy;
  anim_format animation = anim_format::gif;
  std::uint32_t animation_width = 480;  // long edge in pixels, 16..1920
  std::uint32_t animation_fps = 15;     // 1..50
  int jpeg_quality = 92;
};

struct outcome {
  std::vector<std::string> outputs;  // UTF-8, in order (split: part 1, part 2)
  std::string encoder;               // trim_reencode: the encoder that ran
  range written{};                   // the range actually written (snapped)
};

// Progress 0..1 from the worker. Keep it cheap: it is called per packet.
using progress_fn = void (*)(void* user, double fraction) noexcept;

struct control {
  const std::atomic<bool>* cancel = nullptr;
  progress_fn progress = nullptr;
  void* user = nullptr;
};

// Worker thread only. status::cancelled when `cancel` was raised (nothing is
// left behind); status::unsupported_format when the container, codec or
// encoder cannot do what was asked (no hardware encoder for Path 2 on this
// machine, a codec the target container refuses); status::invalid_arg for a
// range that is empty or out of the clip.
[[nodiscard]] result<outcome> run(const request& req, const control& ctl);

// The output name `run` would start from, before collision numbering:
// "IMG_0001_trimmed.mp4". Exposed for the job panel and the tests.
[[nodiscard]] std::string output_stem_suffix(op kind) noexcept;

}  // namespace mv::edit::clip
