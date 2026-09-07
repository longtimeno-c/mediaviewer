// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5c — transport policy: seek mode, frame step, speed, A-B loop, resume.
//
// Everything here is pure and free of FFmpeg, D3D and threads, for the same
// reason presenter::choose() is: these are the rules that are easy to get
// subtly wrong and impossible to eyeball, so they need to be testable with no
// clip, no device and no clock.
#pragma once

#include <cstdint>
#include <string>

#include "player/video_source.h"  // time_ns

namespace mv::player {

// plan/05: "Dragging the scrubber -> seek to the nearest keyframe
// (AVSEEK_FLAG_BACKWARD, no full decode) - instant. On release, or when
// stepping -> decode forward from that keyframe to the exact frame.
// Fast-then-accurate reads to users as responsiveness."
enum class seek_mode : std::uint8_t {
  keyframe,  // scrubbing: nearest keyframe at or before the target, no decode
  exact,     // settle: decode forward from that keyframe to the frame asked for
};

struct seek_request {
  time_ns   target_ns = 0;
  seek_mode mode      = seek_mode::keyframe;
  // Bumped by the caller so in-flight pre-seek frames are discarded
  // (avcodec_flush_buffers plus the generation counter, plan/05).
  std::uint32_t generation = 0;
};

// Scrubber drag -> keyframe. Release, frame step, or a typed timecode -> exact.
[[nodiscard]] seek_request make_seek(time_ns target_ns, bool dragging,
                                     std::uint32_t generation) noexcept;

// ---------------------------------------------------------------------------
// Speed
// ---------------------------------------------------------------------------

inline constexpr double min_rate = 0.25;
inline constexpr double max_rate = 4.0;

[[nodiscard]] double clamp_rate(double rate) noexcept;

// plan/05: "atempo accepts 0.5-2.0 per instance, so the extremes need a chain
// (atempo=0.5,atempo=0.5 for 0.25x); build the chain from the ratio rather than
// assuming one filter."
//
// Returns an FFmpeg filter description, e.g. "atempo=0.500,atempo=0.500" for
// 0.25x. Empty for rate 1.0 — no filter at all, rather than a no-op atempo that
// still costs a resample pass on every block.
[[nodiscard]] std::string build_atempo_chain(double rate);

// ---------------------------------------------------------------------------
// A-B loop
// ---------------------------------------------------------------------------

struct ab_loop {
  time_ns a_ns = 0;
  time_ns b_ns = -1;  // < 0 means no loop
  [[nodiscard]] constexpr bool active() const noexcept { return b_ns > a_ns; }
};

// True, filling out_seek_ns, when playback has run past B and must wrap to A.
// A and B are normalised, so marking B before A still loops rather than
// silently doing nothing — the intent is unambiguous either way.
[[nodiscard]] bool loop_wrap(const ab_loop& loop, time_ns position_ns,
                             time_ns* out_seek_ns) noexcept;

// ---------------------------------------------------------------------------
// Frame step
// ---------------------------------------------------------------------------

// Target PTS for a step of `frames` from `current_ns` at `frame_rate`.
//
// Forward is cheap: decode the next frame. Backward is not — it is a seek to
// the prior keyframe then a decode forward to n-1 — so the caller must use
// seek_mode::exact for it. A half-frame bias in the direction of travel keeps a
// position sitting exactly on a boundary from landing on the same frame twice.
[[nodiscard]] time_ns step_target(time_ns current_ns, int frames,
                                  double frame_rate) noexcept;

// ---------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------

// plan/05: per-file resume position. Near the start there is nothing worth
// resuming; near the end the user has finished watching, and reopening two
// seconds before the credits is worse than starting over.
inline constexpr time_ns resume_min_position_ns = 15'000'000'000;  // 15 s in
inline constexpr time_ns resume_tail_margin_ns  = 10'000'000'000;  // 10 s from end

[[nodiscard]] bool should_store_resume(time_ns position_ns, time_ns duration_ns) noexcept;
[[nodiscard]] time_ns resume_start_position(time_ns stored_ns, time_ns duration_ns) noexcept;

}  // namespace mv::player
