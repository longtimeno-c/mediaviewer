// SPDX-License-Identifier: GPL-2.0-or-later
#include "player/presenter.h"

namespace mv::player {

present_decision choose(const presenter_input& in) noexcept {
  present_decision out;

  // plan/03 has the render loop wait on the frame-latency waitable BEFORE
  // recording, so the frame we decide now reaches the glass about one refresh
  // from now. plan/05: present the frame closest to master + one vblank.
  const double rate = in.playback_rate > 0.0 ? in.playback_rate : 1.0;
  const time_ns interval = static_cast<time_ns>(static_cast<double>(in.vblank_ns) * rate);
  const time_ns target = in.master_clock_ns + interval;
  out.target_ns = target;

  if (!in.has_next) {
    // Starved. plan/05: "if the queue is starved, hold the current frame rather
    // than stalling." This is a fault and is counted separately from cadence.
    out.action = present_action::hold_starved;
    out.err_ms = 0.0;
    return out;
  }

  const time_ns delta = in.next_pts_ns - target;
  out.err_ms = static_cast<double>(delta) / 1'000'000.0;

  // A frame still in the future is simply not due. On 24p or 30p content at
  // 60 Hz this is the common case and is entirely correct — it is 3:2 cadence,
  // not a defect, which is why it never lands in the same counter as a drop.
  if (delta > 0) {
    out.action = present_action::hold_cadence;
    return out;
  }

  // Both delta and interval are STREAM time. At 4x, one display interval
  // covers four times as much stream time; dividing here drops on-time frames.

  if (-delta > interval) {
    // plan/05: "if the next frame is already late by more than a frame
    // interval, drop it." Caller discards and re-evaluates against the frame
    // behind it, so a long stall unwinds in one pass rather than one per vblank.
    out.action = present_action::drop;
    return out;
  }

  out.action = present_action::show;
  return out;
}

}  // namespace mv::player
