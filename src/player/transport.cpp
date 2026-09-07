// SPDX-License-Identifier: GPL-2.0-or-later
// PR 5c — transport policy. See transport.h for why this is pure.
#include "player/transport.h"

#include <algorithm>
#include <cstdio>

namespace mv::player {

seek_request make_seek(time_ns target_ns, bool dragging,
                       std::uint32_t generation) noexcept {
  seek_request r;
  r.target_ns = target_ns < 0 ? 0 : target_ns;
  r.mode = dragging ? seek_mode::keyframe : seek_mode::exact;
  r.generation = generation;
  return r;
}

double clamp_rate(double rate) noexcept {
  // The !(rate > 0.0) form also catches NaN, which std::clamp would propagate.
  if (!(rate > 0.0)) return 1.0;
  return std::clamp(rate, min_rate, max_rate);
}

std::string build_atempo_chain(double rate) {
  const double clamped = clamp_rate(rate);
  if (clamped == 1.0) return {};

  // Each atempo instance is limited to 0.5-2.0, so peel off whole factors until
  // the remainder is in range. Built from the ratio rather than a fixed chain
  // length: 0.25x and 4x need two instances, 1.5x needs one, and 3x needs two
  // with an uneven split.
  double remaining = clamped;
  std::string chain;

  const auto append = [&chain](double factor) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "atempo=%.3f", factor);
    if (!chain.empty()) chain.push_back(',');
    chain.append(buffer);
  };

  while (remaining > 2.0) {
    append(2.0);
    remaining /= 2.0;
  }
  while (remaining < 0.5) {
    append(0.5);
    remaining /= 0.5;
  }
  append(remaining);
  return chain;
}

bool loop_wrap(const ab_loop& loop, time_ns position_ns, time_ns* out_seek_ns) noexcept {
  if (!out_seek_ns) return false;
  if (loop.b_ns < 0) return false;
  const time_ns lo = std::min(loop.a_ns, loop.b_ns);
  const time_ns hi = std::max(loop.a_ns, loop.b_ns);
  if (hi <= lo) return false;
  if (position_ns < hi) return false;
  *out_seek_ns = lo;
  return true;
}

time_ns step_target(time_ns current_ns, int frames, double frame_rate) noexcept {
  if (frames == 0) return current_ns;
  // A clip with no usable nominal rate still has to step. 25 fps is an
  // arbitrary but harmless stand-in: the exact seek decodes to a real frame
  // boundary regardless of what we aimed at.
  const double fps = (frame_rate > 0.0 && frame_rate < 1000.0) ? frame_rate : 25.0;
  const double frame_ns = 1'000'000'000.0 / fps;
  const double bias = (frames > 0 ? 0.5 : -0.5) * frame_ns;
  const double target =
      static_cast<double>(current_ns) + static_cast<double>(frames) * frame_ns + bias;
  return target < 0.0 ? 0 : static_cast<time_ns>(target);
}

bool should_store_resume(time_ns position_ns, time_ns duration_ns) noexcept {
  if (duration_ns <= 0) return false;
  if (position_ns < resume_min_position_ns) return false;
  // Watched to the end: storing here means reopening lands on the credits.
  if (position_ns > duration_ns - resume_tail_margin_ns) return false;
  return true;
}

time_ns resume_start_position(time_ns stored_ns, time_ns duration_ns) noexcept {
  if (stored_ns <= 0 || duration_ns <= 0) return 0;
  if (stored_ns >= duration_ns - resume_tail_margin_ns) return 0;
  return stored_ns;
}

}  // namespace mv::player
