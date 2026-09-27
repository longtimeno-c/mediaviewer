// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Present-or-idle policy. Shared by the D3D11 lab and the Metal lab.
//
// plan/03-rendering.md rule 4: idle → stop presenting (0 % GPU on a still).
// Any input, animation, or video frame → present every vblank. Keep presenting
// for ~500 ms after the last input so a flick does not stutter at the tail.
// A parked cursor is not activity. No input yet is not a fake 500 ms tail.
//
// This header has no GPU types. The Windows and Darwin labs both call it so
// the idle gate cannot drift between present paths.
#pragma once

namespace mv::gfx {

inline constexpr double k_input_tail_seconds = 0.5;
inline constexpr double k_warmup_seconds = 1.0;
inline constexpr unsigned k_occlusion_poll_ms = 200;

struct present_request {
  bool window_visible = true;
  bool window_active = true;
  bool occluded = false;
  bool soak = false;  // timed soak: present even if the window is inactive
  bool animating = false;
  bool camera_moving = false;
  bool video_active = false;
  bool video_loading = false;
  bool has_still = false;
  bool redraw = false;
  bool painted_static = false;
  double elapsed_seconds = 0.0;
  double last_input_time = -1.0;  // < 0: no input yet
};

struct present_decision {
  bool wants_frame = false;
  bool live = false;
  bool pan_tail = false;
};

[[nodiscard]] inline bool pan_tail_active(const present_request& r) noexcept {
  return r.has_still && r.last_input_time >= 0.0 &&
         (r.elapsed_seconds - r.last_input_time) < k_input_tail_seconds;
}

[[nodiscard]] inline present_decision decide_present(const present_request& r) noexcept {
  present_decision d;
  d.pan_tail = pan_tail_active(r);
  d.live = r.video_active || r.video_loading || r.animating || r.camera_moving || d.pan_tail;
  const bool allowed = r.window_visible && !r.occluded && (r.soak || r.window_active);
  if (!allowed) return d;
  if (d.live) {
    d.wants_frame = true;
  } else {
    d.wants_frame = !r.painted_static || r.redraw;
  }
  return d;
}

}  // namespace mv::gfx
