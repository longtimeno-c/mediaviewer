// SPDX-License-Identifier: GPL-2.0-or-later
// The snapshot the UI thread publishes and the render thread consumes.
//
// plan/02-architecture.md: "The UI thread publishes a state snapshot; the
// render thread consumes one. They never share a mutable object. UI logic
// running long can never stall a frame."
//
// This is POD on purpose. It is also why the ImGui Win32 backend is not used:
// that backend mutates ImGuiIO from inside the window procedure, which puts the
// UI thread inside the render thread's ImGui context. Feeding ImGui from this
// snapshot on the render thread costs about fifteen lines and keeps the
// threading model the one the plan describes.
#pragma once

#include <cstdint>

namespace mv::shell {

enum class mouse_button : std::uint32_t { left = 0, right = 1, middle = 2, count = 3 };

struct input_snapshot {
  // Client-area size in physical pixels, and the DPI scale to divide by for
  // layout. PerMonitorV2, so both change on WM_DPICHANGED.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float dpi_scale = 1.0f;

  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  // Cumulative native wheel units. Coalescing publications cannot lose deltas.
  std::int64_t wheel_total = 0;
  std::uint64_t activity_seq = 0;
  bool mouse_down[3] = {false, false, false};
  bool mouse_in_client = false;

  // Latched edge-triggered commands. The render thread compares the counter to
  // the one it last saw, so a keypress can never be missed between frames and
  // can never be double-counted.
  std::uint32_t toggle_overlay_seq = 0;   // F3
  std::uint32_t toggle_animation_seq = 0; // Space
  std::uint32_t reset_stats_seq = 0;      // R
  std::uint32_t fit_seq = 0;              // 0
  std::uint32_t one_to_one_seq = 0;       // 1

  bool window_visible = true;
  bool window_active = true;

  // Bumped by the UI thread when the swapchain must be rebuilt: a resize, a DPI
  // change, or the window moving to a monitor on a different adapter.
  std::uint32_t resize_seq = 0;
  std::uint32_t display_change_seq = 0;
};

// Consumer-owned state: repeated reads of a snapshot never replay input.
struct input_cursor {
  std::int64_t wheel_total = 0;
  std::uint64_t activity_seq = 0;

  float consume_wheel(const input_snapshot& s) noexcept {
    const auto delta = s.wheel_total - wheel_total;
    wheel_total = s.wheel_total;
    return static_cast<float>(delta) / 120.0f;
  }
  bool consume_activity(const input_snapshot& s) noexcept {
    const bool changed = s.activity_seq != activity_seq;
    activity_seq = s.activity_seq;
    return changed;
  }
};

}  // namespace mv::shell
