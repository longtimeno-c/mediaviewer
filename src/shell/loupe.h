// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Hold-Z loupe (docs/design/16 "View"): where its square sits and what it looks
// at. Both labs draw it from this, so the D3D11 and Metal loupes cannot drift.
// Pure maths on the snapshot; the render thread calls it per frame.
#pragma once

#include <algorithm>

#include "shell/input_state.h"

namespace mv::shell {

// A square at the cursor (or the canvas centre with no cursor), kept inside
// the canvas rect (x, y, w, h, in backing pixels). The blit and the frame
// drawn around it both use this.
struct loupe_box {
  float x = 0.0f;
  float y = 0.0f;
  float size = 0.0f;
  float point_x = 0.0f;
  float point_y = 0.0f;
};

[[nodiscard]] inline loupe_box loupe_rect(const input_snapshot& s, float vx, float vy, float vw,
                                          float vh) noexcept {
  const float scale = s.dpi_scale > 0.0f ? s.dpi_scale : 1.0f;
  loupe_box b;
  b.size = std::min({240.0f * scale, vw, vh});
  // Keyboard nudges (arrows while Z is held) offset the cursor or centre.
  const float step = 0.05f * std::min(vw, vh);
  const float base_x = s.mouse_in_client ? s.mouse_x : vx + vw * 0.5f;
  const float base_y = s.mouse_in_client ? s.mouse_y : vy + vh * 0.5f;
  b.point_x = std::clamp(base_x + static_cast<float>(s.loupe_steps_x) * step, vx, vx + vw);
  b.point_y = std::clamp(base_y + static_cast<float>(s.loupe_steps_y) * step, vy, vy + vh);
  b.x = std::clamp(b.point_x - b.size * 0.5f, vx, vx + vw - b.size);
  b.y = std::clamp(b.point_y - b.size * 0.5f, vy, vy + vh - b.size);
  return b;
}

// The loupe's camera: the view's pan moved to the loupe point, at 100 %, or
// twice the zoom when already past it. The same texture through a second
// viewport, never a decode.
struct loupe_camera {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
};

[[nodiscard]] inline loupe_camera loupe_view(const loupe_box& b, float vx, float vy, float vw,
                                             float vh, float pan_x, float pan_y,
                                             float zoom) noexcept {
  const float z = zoom > 0.0f ? zoom : 1.0f;
  loupe_camera c;
  c.pan_x = pan_x + (b.point_x - (vx + vw * 0.5f)) / z;
  c.pan_y = pan_y + (b.point_y - (vy + vh * 0.5f)) / z;
  c.zoom = z < 1.0f ? 1.0f : std::min(64.0f, z * 2.0f);
  return c;
}

}  // namespace mv::shell
