// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Slice 7: the Remove brush drawn over the photo, in the same present as the
// picture (ImGui's foreground list, like crop mode's draft). One body for
// both labs (present_lab.cpp, present_lab_mac.mm); each passes its camera's
// image → screen mapping.
#pragma once

#include <algorithm>

#include "imgui.h"
#include "shell/input_state.h"

namespace mv::shell {

// `pw` x `ph` the shown picture, `zoom` screen pixels per picture pixel.
template <class ToScreen>
void draw_brush_overlay(const edit_view& v, float pw, float ph, float zoom, float scale, ToScreen to_screen) {
  if (!v.brush_on || pw <= 0.0f || ph <= 0.0f || zoom <= 0.0f) return;
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  const float long_side = std::max(pw, ph);
  // Overlapping dabs build up toward solid, as a soft brush would.
  const ImU32 paint = IM_COL32(255, 70, 90, 46);
  for (std::uint32_t i = 0; i < v.brush_count && i < kBrushDabs; ++i) {
    const brush_dab& d = v.brush[i];
    fg->AddCircleFilled(to_screen(d.u * pw, d.v * ph), d.r * long_side * zoom, paint);
  }
  if (v.brush_cursor[0] >= 0.0f && v.brush_cursor[2] > 0.0f) {
    const ImVec2 c = to_screen(v.brush_cursor[0] * pw, v.brush_cursor[1] * ph);
    const float r = v.brush_cursor[2] * long_side * zoom;
    fg->AddCircle(c, r + scale, IM_COL32(0, 0, 0, 160), 0, 1.5f * scale);
    fg->AddCircle(c, r, IM_COL32(255, 255, 255, 230), 0, 1.5f * scale);
  }
}

}  // namespace mv::shell
