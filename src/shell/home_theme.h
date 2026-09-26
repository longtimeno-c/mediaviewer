// SPDX-License-Identifier: GPL-2.0-or-later
// The native empty view is drawn by ImGui on both hosts, outside WinUI/SwiftUI.
// Its colours and clear value therefore travel in the UI-thread snapshot.
#pragma once

#include <cmath>
#include <cstdint>

#include "imgui.h"

namespace mv::shell {

inline float home_linear_channel(std::uint32_t rgb, unsigned shift) noexcept {
  const float c = static_cast<float>((rgb >> shift) & 255u) / 255.0f;
  return c <= 0.04045f ? c / 12.92f
                       : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct home_palette {
  explicit home_palette(std::uint32_t background) noexcept
      : light((((background >> 16) & 255u) * 299u + ((background >> 8) & 255u) * 587u +
               (background & 255u) * 114u) > 128000u) {}

  bool light;
  [[nodiscard]] ImU32 title() const noexcept {
    return light ? IM_COL32(28, 31, 40, 255) : IM_COL32(226, 228, 234, 255);
  }
  [[nodiscard]] ImU32 body() const noexcept {
    return light ? IM_COL32(67, 72, 82, 255) : IM_COL32(160, 164, 174, 255);
  }
  [[nodiscard]] ImU32 muted() const noexcept {
    return light ? IM_COL32(86, 92, 103, 255) : IM_COL32(146, 152, 166, 255);
  }
  [[nodiscard]] ImU32 edge() const noexcept {
    return light ? IM_COL32(20, 28, 40, 70) : IM_COL32(255, 255, 255, 34);
  }
  [[nodiscard]] ImU32 fill() const noexcept {
    return light ? IM_COL32(20, 28, 40, 12) : IM_COL32(255, 255, 255, 10);
  }
  [[nodiscard]] ImU32 cloud() const noexcept {
    return light ? IM_COL32(75, 93, 111, 115) : IM_COL32(150, 156, 172, 90);
  }
};

}  // namespace mv::shell
