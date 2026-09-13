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
  std::uint32_t chrome_height_px = 0;  // command-bar strip; overlay sits below it
  std::uint32_t chrome_bottom_px = 0;  // filmstrip strip; canvas sits above it
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
  std::uint32_t toggle_overlay_seq = 0;   // F / F3
  std::uint32_t toggle_animation_seq = 0; // Space
  std::uint32_t reset_stats_seq = 0;      // R
  std::uint32_t fit_seq = 0;              // 0
  std::uint32_t one_to_one_seq = 0;       // 1
  std::uint32_t zoom_in_seq = 0;          // +
  std::uint32_t zoom_out_seq = 0;         // -
  std::uint32_t zoom_preset_seq = 0;
  float zoom_preset = 1.0f;               // applied when zoom_preset_seq bumps
  std::uint32_t fill_seq = 0;             // 4
  // Keyboard pan, in steps. Cumulative like the wheel, so coalesced key-repeat
  // publications lose none (plan/16: ↑ ↓ when zoomed, Shift+arrows).
  std::int64_t pan_steps_x = 0;
  std::int64_t pan_steps_y = 0;

  // plan/16 view state. Levels, not edges: the render thread draws what these
  // say, and redraws once when any of them changes.
  std::uint8_t background = 0;  // B: 0 canvas, 1 gray, 2 white, 3 checkerboard
  bool sticky_zoom = false;     // S
  bool clipping = false;        // C
  bool loupe = false;           // held Z
  // Arrow nudges since Z went down, in steps of a twentieth of the canvas.
  std::int32_t loupe_steps_x = 0;
  std::int32_t loupe_steps_y = 0;
  bool hold_previous = false;   // held backslash
  bool info_overlay = false;    // O
  // For the info overlay, filled by the UI thread when the selection changes.
  // The render thread never calls into the folder model.
  std::uint32_t item_index = 0;
  std::uint32_t item_count = 0;
  char item_name[260] = {};     // UTF-8, NUL-terminated
  // Marks (plan/16): whether the current item is marked, and how many are.
  bool item_marked = false;
  std::uint32_t marked_count = 0;

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
  std::int64_t pan_steps_x = 0;
  std::int64_t pan_steps_y = 0;

  // True when there are pan steps to apply; `dx` / `dy` are the steps since
  // the last call.
  bool consume_pan(const input_snapshot& s, std::int64_t& dx, std::int64_t& dy) noexcept {
    dx = s.pan_steps_x - pan_steps_x;
    dy = s.pan_steps_y - pan_steps_y;
    pan_steps_x = s.pan_steps_x;
    pan_steps_y = s.pan_steps_y;
    return dx != 0 || dy != 0;
  }
  bool consume_activity(const input_snapshot& s) noexcept {
    const bool changed = s.activity_seq != activity_seq;
    activity_seq = s.activity_seq;
    return changed;
  }
};

}  // namespace mv::shell
