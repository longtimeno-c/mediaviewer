// SPDX-License-Identifier: GPL-2.0-or-later
// Trim mode and the clip tools flyout, host-side (PR 13 / 14; plan/08,
// plan/16). One model both hosts drive, so `[` `]`, the keyframe walk, the
// A-B preview range and the request a key submits are the same on Windows
// and macOS; each host only draws it and calls the core.
//
// UI thread only. No I/O: the keyframe index arrives from a clip job
// (edit::clip::probe on a worker) through set_index.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit/clip.h"

namespace mv::shell {

class trim_state {
 public:
  // Enters trim on `path`. Markers survive leaving and re-entering trim on
  // the same clip; another clip starts clean.
  void arm(std::string path, std::int64_t duration_ns);
  void disarm() noexcept;
  // The current item changed: trim ends and the markers go with the clip.
  void forget() noexcept;

  [[nodiscard]] bool armed() const noexcept { return armed_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // The probe's answer. Ignored unless it is for the clip armed (a late
  // answer for a clip already left).
  void set_index(const std::string& path, std::vector<std::int64_t> keyframes_ns, std::int64_t duration_ns);
  [[nodiscard]] bool index_ready() const noexcept { return index_ready_; }
  [[nodiscard]] const std::vector<std::int64_t>& keyframes() const noexcept { return keyframes_; }
  [[nodiscard]] std::int64_t duration_ns() const noexcept { return duration_ns_; }

  // `[` / `]`. A marker that would cross the other one clears the other.
  void mark_in(std::int64_t position_ns) noexcept;
  void mark_out(std::int64_t position_ns) noexcept;
  void clear() noexcept;
  [[nodiscard]] std::int64_t in_ns() const noexcept { return in_; }    // -1: unset
  [[nodiscard]] std::int64_t out_ns() const noexcept { return out_; }  // -1: unset
  [[nodiscard]] bool has_marker() const noexcept { return in_ >= 0 || out_ >= 0; }

  // The requested range (an unset marker is the clip's start or end), and
  // what Path 1 will actually write (snapped). Without the index yet, the
  // snapped range is the requested one.
  [[nodiscard]] edit::clip::range requested() const noexcept;
  [[nodiscard]] edit::clip::range keyframe_range() const noexcept;

  // P: the A-B loop over the keyframe range. Returns the new state.
  bool toggle_preview() noexcept;
  [[nodiscard]] bool previewing() const noexcept { return preview_; }

  // Ctrl+Left / Right. Strictly before / after `position_ns` by more than a
  // millisecond, so a repeat walks on. `position_ns` when there is none.
  [[nodiscard]] std::int64_t prev_keyframe(std::int64_t position_ns) const noexcept;
  [[nodiscard]] std::int64_t next_keyframe(std::int64_t position_ns) const noexcept;

  // A request on the armed clip for trim_keyframe / trim_reencode /
  // remove_middle over the markers.
  [[nodiscard]] edit::clip::request request(edit::clip::op kind) const;

  // "In 0:12.345 · Out 0:40.000 · saves 0:10.010–0:41.041": the chip the
  // transport shows while trim is armed.
  [[nodiscard]] std::string label() const;

 private:
  std::string path_;
  bool armed_ = false;
  bool index_ready_ = false;
  bool preview_ = false;
  std::vector<std::int64_t> keyframes_;
  std::int64_t duration_ns_ = 0;
  std::int64_t in_ = -1;
  std::int64_t out_ = -1;
};

// "1:02.345", "1:01:02.345" past an hour.
[[nodiscard]] std::string format_clip_time(std::int64_t ns);

// ---- the clip tools flyout (Ctrl+S on a clip) ----------------------------------

// The flyout greys out what the clip cannot do; the host passes these as the
// popup's mode_mask.
inline constexpr std::int32_t kClipFlagHasRange = 1 << 0;  // trim markers set
inline constexpr std::int32_t kClipFlagHasAudio = 1 << 1;
inline constexpr std::int32_t kClipFlagHasVideo = 1 << 2;

// Its answer, one integer across the island wire: the op in bits 0-7, the
// option in 8-15 (rotate: 1 = 90° right, 2 = 90° left, 3 = 180°; remux: 1 MP4,
// 2 MKV; frame: 1 PNG, 2 JPEG; audio: 1 copy, 2 WAV, 3 FLAC; animation: 1 GIF,
// 2 WebP).
[[nodiscard]] constexpr std::int32_t pack_clip_choice(edit::clip::op kind, int option) noexcept {
  return static_cast<std::int32_t>(kind) | ((option & 0xFF) << 8);
}

// The request a flyout answer means, on `path` at `playhead_ns`, over the
// trim markers when there are any (frame: the playhead; split: the playhead;
// animation and audio: the markers, else the whole clip). False for an
// answer that is not a clip tool.
[[nodiscard]] bool clip_tool_request(std::int32_t packed, const std::string& path, std::int64_t playhead_ns,
                                     const trim_state* markers, edit::clip::request& out);

}  // namespace mv::shell
