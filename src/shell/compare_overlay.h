// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Compare": the before/after split's divider and its grab
// handle, drawn over the photo in the same present (ImGui's foreground list,
// like the canvas tools' overlay). One body for both labs (present_lab.cpp,
// present_lab_mac.mm), so the D3D11 and Metal splits look the same. The
// "Before | After" words are the chromes' (owner call A: nothing but the
// divider and its handle floats over the photo).
#pragma once

#include <cstdint>

#include "imgui.h"
#include "shell/compare_source.h"

namespace mv::shell {

// A 2 px light line with a dark edge either side (it reads on any photo), from
// the top of the picture's visible span to its bottom, and a round handle at
// its middle with ◀ ▶ in it. Hovered or held, the handle's ring and the line
// take the theme's accent (`accent_rgba`, input_snapshot::home.accent).
inline void draw_compare_divider(ImDrawList* fg, const compare_frame& f, float dpi_scale,
                                 std::uint32_t accent_rgba, bool hot) noexcept {
  if (!fg || !f.valid) return;
  const float s = dpi_scale > 0.0f ? dpi_scale : 1.0f;
  const ImU32 dark = IM_COL32(0, 0, 0, 150);
  const ImU32 light = IM_COL32(245, 245, 248, 235);
  const ImU32 accent =
      IM_COL32((accent_rgba >> 24) & 0xFFu, (accent_rgba >> 16) & 0xFFu, (accent_rgba >> 8) & 0xFFu, 255);
  const ImU32 line = hot ? accent : light;
  const float x = f.divider_x;
  fg->AddRectFilled(ImVec2(x - 2.0f * s, f.y0), ImVec2(x + 2.0f * s, f.y1), dark);
  fg->AddRectFilled(ImVec2(x - 1.0f * s, f.y0), ImVec2(x + 1.0f * s, f.y1), line);
  const float r = kCompareHandleDip * s;
  const ImVec2 c(x, (f.y0 + f.y1) * 0.5f);
  fg->AddCircleFilled(c, r + 1.0f * s, dark, 32);
  fg->AddCircleFilled(c, r - 1.0f * s, IM_COL32(24, 24, 28, 200), 32);
  fg->AddCircle(c, r - 0.5f * s, line, 32, 1.5f * s);
  const float a = 4.5f * s;   // the arrowheads' half height
  const float g = 2.5f * s;   // the gap either side of the line
  const float w = 5.0f * s;   // their depth
  fg->AddTriangleFilled(ImVec2(c.x - g, c.y - a), ImVec2(c.x - g, c.y + a), ImVec2(c.x - g - w, c.y), light);
  fg->AddTriangleFilled(ImVec2(c.x + g, c.y - a), ImVec2(c.x + g + w, c.y), ImVec2(c.x + g, c.y + a), light);
}

}  // namespace mv::shell
