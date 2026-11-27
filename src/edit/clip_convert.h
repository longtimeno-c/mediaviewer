// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip the phone cannot play, as HLS segments the phone can (docs/plans/
// phone-clip-convert.md). The server's helper (MediaViewerClipJob
// --convert-*) runs one call per process, at background priority.
//
// The timeline is cut into fixed segments of `segment_ms` (the last shorter);
// segment k is the source's [k * segment_ms, (k + 1) * segment_ms), decoded
// from the keyframe at or before its start and encoded on a HARDWARE H.264
// encoder (edit/hwencode.h, the rules of Path 2) starting on an IDR, with no
// B-frames. Sound is the first audio stream as stereo AAC from the OS's
// encoder (clip_audio_enc.h: AudioToolbox, Media Foundation; never FFmpeg's
// own, docs/design/11); a machine with no AAC encoder gives silent segments.
// Each segment is a self-contained MPEG-TS whose timestamps are the source's
// timeline plus kTimestampOffsetMs, so segments made by different runs play
// one after another.
//
// Rule 5: the source is opened read-only. Rule 6: nothing here logs a path.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::edit::clip::convert {

inline constexpr std::uint32_t kDefaultSegmentMs = 4000;
// Every output timestamp is the timeline plus this: never negative (AAC
// priming), and the same for every segment.
inline constexpr std::int64_t kTimestampOffsetMs = 10000;
inline constexpr std::uint32_t kMaxLongSide = 1920;

struct plan_info {
  std::int64_t duration_ms = 0;
  std::uint32_t segment_ms = 0;
  std::uint32_t segments = 0;
  std::uint32_t width = 0, height = 0;  // the output's, square pixels
  std::string video_codec, audio_codec;  // the source's ("mpeg4", "mp3"; "" for none)
};

// status::unsupported_format: no video stream, or no duration to cut.
[[nodiscard]] result<plan_info> plan(std::string_view utf8_path, std::uint32_t segment_ms = kDefaultSegmentMs);
// {"duration_ms","segment_ms","segments","width","height"}
[[nodiscard]] std::string plan_json(const plan_info& p);

// Writes segment k to `out_path` (created or replaced). status::invalid_arg
// for k past the plan's last; status::unsupported_format for what plan()
// refuses; status::not_found when no hardware encoder opens.
[[nodiscard]] expected segment(std::string_view utf8_path, std::uint32_t k, std::uint32_t segment_ms,
                               const std::string& out_path, const std::atomic<bool>* cancel = nullptr);

struct encoder_found {
  std::string name;    // "h264_videotoolbox"
  std::string family;  // "VideoToolbox"
};
// The hardware H.264 encoder that opens at 1280x720 here, or status::not_found.
[[nodiscard]] result<encoder_found> check();

// FFmpeg says nothing on stderr (the server's log): its messages can name the file.
void quiet_logs() noexcept;

namespace detail {
// The tests: other encoders (a software one on a runner without a GPU).
struct encoders {
  std::vector<std::string> names;
  bool allow_software = false;
};
[[nodiscard]] expected segment_with(std::string_view utf8_path, std::uint32_t k, std::uint32_t segment_ms,
                                    const std::string& out_path, const std::atomic<bool>* cancel,
                                    const encoders& with);
// The output size for a source picture: square pixels, long side at most
// kMaxLongSide, both even.
void output_size(int width, int height, int sar_num, int sar_den, std::uint32_t& out_w, std::uint32_t& out_h) noexcept;
}  // namespace detail

}  // namespace mv::edit::clip::convert
