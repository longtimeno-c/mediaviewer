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

// PR 9: what the info overlay and the AF quads need from the property model,
// formatted by the UI thread when the selection's metadata arrives. The render
// thread only draws these bytes — it never sees a `metadata`, a path or a file,
// which is what makes toggling the overlays free of I/O (plan/16).
struct meta_overlay {
  static constexpr int kMaxAf = 16;
  char camera_line[160] = {};    // "Canon EOS R5  |  RF85mm F1.2 L USM"
  char exposure_line[128] = {};  // "1/250 s   f/2.8   ISO 400   85 mm"
  char date_line[96] = {};       // "2024-05-01 14:03:22   48.85837 N, 2.29448 E"
  // AF quads, normalised 0..1 in the *displayed* image (orientation already
  // applied by the host): x, y, w, h, in_focus (1 / 0).
  std::uint8_t af_count = 0;
  float af[kMaxAf][5] = {};
};

// PR 10: the edit geometry for one opened item, as the render thread needs it
// (edit::geometry flattened to POD). The render thread places it against the
// texture it actually holds, so the UI never needs the decoded size to turn a
// picture. `item` names the image the geometry belongs to: the id
// present_lab_mac::open_item returned on Mac, the path's item_key on Windows,
// where `generation` (the session's view generation after the select) tells a
// rewritten file's new pixels from the old texture still on screen.
// shell/edit_view.h matches a texture to a slot.
struct edit_view {
  std::uint64_t item = 0;  // 0 = no geometry
  std::uint32_t generation = 0;
  std::int8_t d4[4] = {1, 0, 0, 1};
  float straighten = 0.0f;
  float crop[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // x, y, w, h in the straightened frame
  bool keep_frame = false;                   // crop mode: whole frame, no auto-crop
  bool crop_overlay = false;                 // crop mode: draw the draft rect
  float overlay[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // normalised to the output frame
  // PR 11: the item's colour adjust as the blit's uniforms
  // (edit::adjust_uniforms a0 / a1). `adjust` false = no colour op: the blit
  // skips the kernel. A change is a redraw, never a refit or a decode.
  bool adjust = false;
  float adjust0[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float adjust1[4] = {1.0f, 0.18f, 0.0f, 0.0f};
};

struct input_snapshot {
  // Client-area size in physical pixels, and the DPI scale to divide by for
  // layout. PerMonitorV2, so both change on WM_DPICHANGED.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t chrome_height_px = 0;  // command-bar strip; overlay sits below it
  std::uint32_t chrome_bottom_px = 0;  // filmstrip strip; canvas sits above it
  // Folder tree strip (plan/16: a left island, hidden by default). 0 until the
  // tree lands (PR 8); the canvas maths already takes it.
  std::uint32_t chrome_left_px = 0;
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
  std::uint32_t game_exit_seq = 0;        // Esc with nothing to close: leave the empty-window runner
  std::uint32_t game_view_seq = 0;        // 3: toggle the runner's 2D/3D camera
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
  bool af_points = false;       // Shift+O: quads from `meta`, no file read
  bool eyedropper = false;      // Shift+I: one-pixel readout at the cursor
  meta_overlay meta;
  // PR 10: [0] is the item on the canvas, [1] the one before it. A lossless
  // rotate reloads the file under a new item id; until its pixels land the
  // old texture keeps the old geometry, so the turn never flashes back.
  edit_view edit[2];
  // Bumped by the UI thread whenever `meta` is replaced, so an idle render
  // thread draws one frame for it (the struct is too big to compare).
  std::uint32_t meta_seq = 0;
  // For the info overlay, filled by the UI thread when the selection changes.
  // The render thread never calls into the folder model.
  std::uint32_t item_index = 0;
  std::uint32_t item_count = 0;
  char item_name[260] = {};     // UTF-8, NUL-terminated
  // Marks (plan/16): whether the current item is marked, and how many are.
  bool item_marked = false;
  std::uint32_t marked_count = 0;
  // Slideshow `.`: the canvas goes black and idles; nothing is drawn over it.
  bool blackout = false;
  // An animated item: Space toggles play / pause, `,` `.` step (cumulative).
  std::uint32_t anim_toggle_seq = 0;
  std::int64_t anim_steps = 0;
  // Clip transport beyond play/pause and frame step (plan/16 "Video"): skips in
  // milliseconds (cumulative, so coalesced key-repeat loses none), speed-ladder
  // rungs (cumulative), and a mute edge. Space and `,` `.` reuse the two fields
  // above, exactly as an animation does.
  std::int64_t video_skip_ms = 0;
  std::int32_t video_speed_steps = 0;
  std::uint32_t video_mute_seq = 0;
  // Volume in 10 % steps (cumulative, like the speed rungs): Up / Down on a clip.
  std::int32_t video_volume_steps = 0;
  // Absolute volume from the transport strip's slider (plan/16 "More" panel).
  // Edge-triggered like the seek pair: seq bumps, value is read alongside it.
  std::uint32_t video_volume_set_seq = 0;
  float video_volume_set_value = 1.0f;
  // Absolute seek from the transport strip's scrubber. `exact` is false while the
  // thumb is being dragged (nearest keyframe, instant) and true on release.
  std::uint32_t video_seek_seq = 0;
  std::int64_t video_seek_ms = 0;
  bool video_seek_exact = true;
  // PR 13: a seek to an exact nanosecond (a keyframe, trim's in point); used
  // instead of video_seek_ms when >= 0. A keyframe at 433.333 ms sought as
  // 433 ms would show the frame before it.
  std::int64_t video_seek_ns = -1;
  // PR 13: the A-B loop trim previews with (plan/08 "Preview the cut").
  // Edge-triggered like the seek pair; b < 0 clears it.
  std::uint32_t video_loop_seq = 0;
  std::int64_t video_loop_a_ns = 0;
  std::int64_t video_loop_b_ns = -1;

  bool window_visible = true;
  bool window_active = true;
  // Gallery jump: drop the still on the canvas so the previous item does not
  // flash under the closing grid. Sequential A/D still keep the last frame.
  std::uint32_t discard_media_seq = 0;

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
