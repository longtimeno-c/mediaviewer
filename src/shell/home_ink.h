// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// home_theme tokens as ImGui colours, for the cards and the ground the present
// labs draw on the empty window (welcome_screen.h, error_screen.h, dino_draw*.h,
// home_ground.h). Kept apart from home_theme.h so the snapshot needs no ImGui.
#pragma once

#include <cmath>
#include <cstdint>

#include "imgui.h"
#include "shell/home_theme.h"

namespace mv::shell {

// A 0xRRGGBBAA token as an ImGui colour, its alpha scaled by `alpha` (0..1).
// Both labs draw ImGui into an _SRGB target, which treats a vertex colour as
// linear and encodes it on write, so the token is decoded here: drawn as is,
// Midnight's #161b22 surface came out #555c65. Eight linear bits band a little
// in the deepest shadows (a dark token lands within a step or two of itself);
// the clear, which carries floats, is exact.
[[nodiscard]] inline ImU32 home_col(std::uint32_t rgba, float alpha = 1.0f) noexcept {
  const float k = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
  const auto a = static_cast<unsigned>(std::lround(static_cast<float>(rgba & 255u) * k));
  const auto lin = [rgba](unsigned shift) {
    return static_cast<unsigned>(std::lround(home_linear_channel(rgba, shift) * 255.0f));
  };
  return IM_COL32(lin(24), lin(16), lin(8), a);
}

}  // namespace mv::shell
