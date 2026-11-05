// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 Behaviour 4: crop mode's handles on the canvas. A press within
// the grab distance of the draft rect's edge or corner drags it, a press inside
// it moves it, anywhere else is the canvas's as before (a pan, a file drag).
// Both hosts hit-test here; edit_session::drag_crop lands the drag (the locked
// ratio, the straightened source's covered area).
//
// The edge bits and the cursors are Extend's frame's (shell/edit_workspace.h
// extend_handle, canvas_tool.h extend_cursor_of, kExtendGrabPx): one model for
// both frames. Crop mode shows the whole straightened frame, so the shown
// picture's uv (canvas_map::to_uv) is the uv the draft is normalised to.
//
// Pure and UI-thread only: no I/O, no allocation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "edit/edit_stack.h"
#include "shell/edit_workspace.h"

namespace mv::shell {

// The handle bits under (u, v) for the draft `r`: extend_hit on a frame whose
// sides are r's (`reach_u` / `reach_v` the grab distance in uv). Two bits at a
// corner, extend_move inside, extend_none off the rect.
[[nodiscard]] inline std::uint8_t crop_hit(const edit::rect& r, float u, float v, float reach_u,
                                           float reach_v) noexcept {
  extend_draft d;
  d.l = -r.x;
  d.t = -r.y;
  d.r = r.x + r.w - 1.0f;
  d.b = r.y + r.h - 1.0f;
  return extend_hit(d, u, v, reach_u, reach_v);
}

// The rect a drag of `handle` by (du, dv) (uv, from the press) makes of
// `start`, inside the frame (0..1) and at least `min_side` each way. `ratio` is
// a locked preset's width over height in normalised units (0: free). Free, each
// held edge follows the pointer and the others stay. Locked, a corner keeps the
// opposite corner fixed and follows the pointer projected onto its diagonal
// (so a pull along one axis still resizes); an edge resizes the other axis
// about its centre. A move keeps the size.
[[nodiscard]] inline edit::rect crop_drag_rect(const edit::rect& start, std::uint8_t handle, float du, float dv,
                                               float ratio, float min_side) noexcept {
  if (!std::isfinite(du)) du = 0.0f;
  if (!std::isfinite(dv)) dv = 0.0f;
  const float x0 = start.x, y0 = start.y, x1 = start.x + start.w, y1 = start.y + start.h;
  if (handle & extend_move) {
    edit::rect r = start;
    r.x = std::max(0.0f, std::min(x0 + du, 1.0f - start.w));
    r.y = std::max(0.0f, std::min(y0 + dv, 1.0f - start.h));
    return r;
  }
  const bool hl = (handle & extend_left) != 0, hr = (handle & extend_right) != 0;
  const bool ht = (handle & extend_top) != 0, hb = (handle & extend_bottom) != 0;
  if (!(ratio > 0.0f) || !std::isfinite(ratio)) {
    float nx0 = x0, nx1 = x1, ny0 = y0, ny1 = y1;
    if (hl) nx0 = std::max(0.0f, std::min(x0 + du, x1 - min_side));
    if (hr) nx1 = std::min(1.0f, std::max(x1 + du, x0 + min_side));
    if (ht) ny0 = std::max(0.0f, std::min(y0 + dv, y1 - min_side));
    if (hb) ny1 = std::min(1.0f, std::max(y1 + dv, y0 + min_side));
    return {nx0, ny0, nx1 - nx0, ny1 - ny0};
  }
  const float k = ratio;
  const float min_w = std::max(min_side, min_side * k);  // both sides at least min_side
  const bool horiz = hl || hr, vert = ht || hb;
  edit::rect r = start;
  if (horiz && vert) {
    // The fixed corner, and where the held one went.
    const float ax = hl ? x1 : x0, ay = ht ? y1 : y0;
    const float px = (hl ? x0 : x1) + du, py = (ht ? y0 : y1) + dv;
    const float pw = hl ? ax - px : px - ax, ph = ht ? ay - py : py - ay;
    // (pw, ph) projected onto the ratio's diagonal (k, 1).
    float w = std::max(0.0f, (pw * k + ph) / (k * k + 1.0f) * k);
    const float room = std::min(hl ? ax : 1.0f - ax, (ht ? ay : 1.0f - ay) * k);
    w = std::min(std::max(w, min_w), room);
    r.w = w;
    r.h = w / k;
    r.x = hl ? ax - r.w : ax;
    r.y = ht ? ay - r.h : ay;
    return r;
  }
  if (horiz) {
    const float ax = hl ? x1 : x0;
    const float px = (hl ? x0 : x1) + du;
    const float room = std::min(hl ? ax : 1.0f - ax, k);  // the height stays inside the frame
    r.w = std::min(std::max(hl ? ax - px : px - ax, min_w), room);
    r.h = r.w / k;
    r.x = hl ? ax - r.w : ax;
    const float cy = y0 + start.h * 0.5f;
    r.y = std::max(0.0f, std::min(cy - r.h * 0.5f, 1.0f - r.h));
    return r;
  }
  if (vert) {
    const float ay = ht ? y1 : y0;
    const float py = (ht ? y0 : y1) + dv;
    const float room = std::min(ht ? ay : 1.0f - ay, 1.0f / k);  // the width stays inside the frame
    r.h = std::min(std::max(ht ? ay - py : py - ay, min_w / k), room);
    r.w = r.h * k;
    r.y = ht ? ay - r.h : ay;
    const float cx = x0 + start.w * 0.5f;
    r.x = std::max(0.0f, std::min(cx - r.w * 0.5f, 1.0f - r.w));
    return r;
  }
  return start;
}

}  // namespace mv::shell
