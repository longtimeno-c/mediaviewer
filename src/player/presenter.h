// SPDX-License-Identifier: GPL-2.0-or-later
// The seam between 5a (owns the texture ring) and 5b (owns the clock).
//
// choose() is pure, non-blocking and D3D-free: given where the master clock is
// and how long a vblank lasts, it decides SHOW / HOLD / DROP. That makes the
// hardest logic in the player unit-testable headlessly, with no device, no
// audio endpoint and no clip — which is the only way the drop/hold rules get
// tested at all.
//
// Called from the render thread. It must never take a lock a decode worker
// holds (CLAUDE.md rule 1).
#pragma once

#include <cstdint>

#include "player/video_source.h"

namespace mv::player {

enum class present_action : std::uint8_t {
  show,          // dequeue and present the next frame
  hold_cadence,  // next frame is not due yet — normal for 24p/30p on 60 Hz
  hold_starved,  // queue empty — fault, keep showing the previous frame
  drop,          // next frame is late by > one interval — discard, re-evaluate
};

struct present_decision {
  present_action action    = present_action::hold_starved;
  time_ns        target_ns = 0;  // the deadline used, for the overlay
  double         err_ms    = 0.0;
};

struct presenter_input {
  time_ns master_clock_ns = 0;   // audio clock, or the host clock in fallback
  time_ns vblank_ns       = 0;   // one refresh interval
  time_ns next_pts_ns     = 0;   // from video_source::peek_next_pts
  bool    has_next        = false;
  double  playback_rate   = 1.0;
};

// plan/05: present the frame whose PTS is closest to master + one vblank; if the
// next frame is already late by more than a frame interval, drop it; if the
// queue is starved, hold rather than stall.
[[nodiscard]] present_decision choose(const presenter_input& in) noexcept;

}  // namespace mv::player
