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

  // A 30p frame remains useful for 33 ms, not one 60 Hz refresh (16 ms).
  // Dropping on display-interval lateness made timer jitter throw away the
  // only due frame and hold an even older one. Only discard when a queued
  // replacement is due, using its actual PTS so VFR and playback rate work.
  if (in.has_following && in.following_pts_ns >= in.next_pts_ns &&
      in.following_pts_ns <= target) {
    out.action = present_action::drop;
    return out;
  }

  out.action = present_action::show;
  return out;
}

}  // namespace mv::player
