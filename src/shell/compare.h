// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Two-up compare (`|`, docs/design/16 "View"): the previous still beside the
// current one, under one camera. Both labs draw it from this, so the D3D11 and
// Metal splits cannot drift. Pure maths; the render thread calls it per frame.
//
// The split is a left-docked pane of the canvas, like PR 29's Edit pane on the
// right: the current still keeps the camera, framed in the right half, and the
// previous still is a second draw of a texture already resident on the GPU in
// the left half, in the same present. No decode, no second present path.
#pragma once

#include <algorithm>
#include <cmath>

namespace mv::shell {

// Backing pixels between the two halves, per DIP.
inline constexpr float kCompareGapDip = 8.0f;

// The canvas rect (x, w; both halves keep its y and h) cut in two. `valid` is
// false when the canvas is too narrow to split; the caller then draws one-up.
struct compare_split {
  bool valid = false;
  float left_x = 0.0f;
  float left_w = 0.0f;
  float right_x = 0.0f;
  float right_w = 0.0f;
};

[[nodiscard]] inline compare_split split_compare(float vx, float vw, float dpi_scale) noexcept {
  const float scale = dpi_scale > 0.0f ? dpi_scale : 1.0f;
  const float gap = std::round(kCompareGapDip * scale);
  compare_split s;
  // Whole pixels, so a viewport / scissor edge never lands mid-texel.
  const float x = std::round(vx);
  const float w = std::floor(vw);
  if (w < gap + 2.0f * 32.0f) return s;
  s.left_x = x;
  s.left_w = std::floor((w - gap) * 0.5f);
  s.right_x = x + s.left_w + gap;
  s.right_w = w - s.left_w - gap;
  s.valid = true;
  return s;
}

// The same point in the right half for a cursor over the left one (or the gap),
// so the wheel zooms toward the spot under the cursor in whichever half it is.
[[nodiscard]] inline float compare_cursor_x(float mouse_x, const compare_split& s) noexcept {
  if (!s.valid || mouse_x >= s.right_x) return mouse_x;
  return std::min(mouse_x + (s.right_x - s.left_x), s.right_x + s.right_w - 1.0f);
}

// The previous still's camera, linked to the current one's. Pan is carried as a
// fraction of each picture and zoom relative to each one's fit in the pane, so
// a same-size burst frame is the identical view (pan, zoom, 100 % is 100 %) and
// a different frame shows the same region at the same relative magnification.
struct compare_camera {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
};

[[nodiscard]] inline compare_camera linked_camera(float pan_x, float pan_y, float zoom,
                                                  float cur_w, float cur_h, float prev_w,
                                                  float prev_h, float pane_w,
                                                  float pane_h) noexcept {
  compare_camera c{pan_x, pan_y, zoom};
  if (cur_w <= 0.0f || cur_h <= 0.0f || prev_w <= 0.0f || prev_h <= 0.0f) return c;
  if (cur_w == prev_w && cur_h == prev_h) return c;  // a burst: exactly the same view
  c.pan_x = pan_x / cur_w * prev_w;
  c.pan_y = pan_y / cur_h * prev_h;
  if (pane_w > 0.0f && pane_h > 0.0f) {
    const float fit_cur = std::min(pane_w / cur_w, pane_h / cur_h);
    const float fit_prev = std::min(pane_w / prev_w, pane_h / prev_h);
    if (fit_cur > 0.0f) c.zoom = zoom * (fit_prev / fit_cur);
  }
  return c;
}

}  // namespace mv::shell
