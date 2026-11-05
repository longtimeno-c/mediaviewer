// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S3: canvas tools. What a click or drag on the photo
// does while a tool is armed (the Remove brush, the red-eye tool, Faces
// (shell/faces_tool.h), Extend's frame (S6)), its size in screen pixels, and the pure maths
// both hosts share: the home tab a tool opens, the size steps, the spot under
// the pointer, the eye the red-eye ring snaps to, and Find eyes' test.
//
// Header-only and UI-thread only, like edit_workspace: no I/O, no allocation
// on a key, nothing the render thread reads (it draws edit_view's copy,
// shell/tool_overlay.h). Tool mode is a key mode (commands.h mode::tool,
// key_router.h blocked_in_tool); the context bar sits in the transport's
// band below the photo, never over it (docs/design/20).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "edit/adjust.h"
#include "edit/edit_stack.h"
#include "image/half.h"
#include "image/linear.h"
#include "shell/adjust_pane.h"
#include "shell/edit_workspace.h"
#include "shell/input_state.h"

namespace mv::shell {

// Wire values: chrome_cmd_canvas_tool carries tool × 1000 + size, and the
// Mac bridge's mv_tool_bar_view.tool.
enum class canvas_tool : std::uint8_t { none = 0, remove_brush = 1, red_eye = 2, wb_pick = 3, faces = 4, extend = 5 };

// Sizes are the ring's diameter in screen points (DIP): "48 px" on the bar.
inline constexpr float kToolMinPx = 4.0f;
inline constexpr float kToolMaxPx = 400.0f;
inline constexpr float kToolStep = 1.2f;  // one `[` / `]`
inline constexpr float kBrushDefaultPx = 40.0f;
inline constexpr float kRedEyeDefaultPx = 24.0f;
// The band the context bar takes below the photo while a tool is armed, in
// points (DIP): the bar is 44 high with 6 above and below. It adds to
// chrome_bottom_px and bumps refit_seq, so the picture refits above it.
inline constexpr float kToolBarBand = 56.0f;
// How long the size label stays after a change, and the red-eye pulse.
inline constexpr std::uint64_t kToolLabelMs = 800;
inline constexpr std::uint64_t kToolPulseMs = 300;
// The red-eye ring snaps to a detected eye within this many ring radii, and
// takes this much of the eye's iris.
inline constexpr float kEyeSnapReach = 1.5f;
inline constexpr float kEyeSnapIris = 1.1f;
// Find eyes fixes an eye when at least this share of its iris disc is red.
inline constexpr float kRedEyeRatio = 0.12f;

static_assert(kToolSpots == edit::kRedEyeSpots, "edit_view carries every red-eye spot");

struct tool_state {
  canvas_tool tool = canvas_tool::none;
  float brush_px = kBrushDefaultPx;
  float redeye_px = kRedEyeDefaultPx;
  bool pan_held = false;    // Space is down: a drag pans
  bool erase_held = false;  // Alt / Option is down: the brush erases
  bool erase = false;       // the bar's Erase toggle
  std::uint64_t hud_until_ms = 0;  // the size label's deadline (steady_ms)
};

// The tab a tool opens the workspace on: Remove, Faces and Extend on Enhance
// (Extend's one door is the Enhance tab's card, owner 2026-11-05), red-eye
// and the white-balance picker on Adjust.
[[nodiscard]] constexpr edit_tab home_tab(canvas_tool t) noexcept {
  switch (t) {
    case canvas_tool::remove_brush:
    case canvas_tool::faces:
    case canvas_tool::extend: return edit_tab::enhance;
    case canvas_tool::red_eye:
    case canvas_tool::wb_pick: return edit_tab::colour;
    case canvas_tool::none: break;
  }
  return edit_tab::crop;
}

// Arms `t` (none puts the tool down) and opens the workspace on its home tab,
// unfolded. Crop mode and a tool are exclusive: the host leaves crop first.
// True when the workspace changed (the host re-syncs its panes).
inline bool arm_tool(tool_state& s, edit_workspace& ws, canvas_tool t) noexcept {
  s.tool = t;
  s.pan_held = false;
  s.erase_held = false;
  if (t == canvas_tool::none) return false;
  const edit_tab tab = home_tab(t);
  const bool changed = !ws.open || ws.tab != tab || ws.collapsed;
  ws.open = true;
  ws.tab = tab;
  ws.collapsed = false;
  return changed;
}

// adjust_pick's wire values stay (the panes send them); a tool is the host's state.
[[nodiscard]] constexpr canvas_tool tool_of_pick(adjust_pick p) noexcept {
  switch (p) {
    case adjust_pick::white_balance: return canvas_tool::wb_pick;
    case adjust_pick::red_eye: return canvas_tool::red_eye;
    case adjust_pick::remove: return canvas_tool::remove_brush;
    case adjust_pick::none: break;
  }
  return canvas_tool::none;
}
[[nodiscard]] constexpr adjust_pick pick_of_tool(canvas_tool t) noexcept {
  switch (t) {
    case canvas_tool::wb_pick: return adjust_pick::white_balance;
    case canvas_tool::red_eye: return adjust_pick::red_eye;
    case canvas_tool::remove_brush: return adjust_pick::remove;
    default: break;
  }
  return adjust_pick::none;
}

// Whether the tool has a size (and so a ring, `[` `]` and the bar's slider).
[[nodiscard]] constexpr bool sized(canvas_tool t) noexcept {
  return t == canvas_tool::remove_brush || t == canvas_tool::red_eye;
}

[[nodiscard]] inline float clamp_tool_px(float px) noexcept {
  if (!std::isfinite(px)) return kBrushDefaultPx;
  return std::clamp(std::round(px), kToolMinPx, kToolMaxPx);
}

// One `[` (dir < 0) or `]` (dir > 0): ×1/1.2 or ×1.2, whole points, and at
// least one point of change so the small end still moves.
[[nodiscard]] inline float step_tool_px(float px, int dir) noexcept {
  const float now = clamp_tool_px(px);
  if (dir == 0) return now;
  float next = std::round(dir > 0 ? now * kToolStep : now / kToolStep);
  if (next == now) next = now + (dir > 0 ? 1.0f : -1.0f);
  return clamp_tool_px(next);
}

// The armed tool's size, or nullptr for one without a size.
[[nodiscard]] inline float* tool_px_ref(tool_state& s) noexcept {
  if (s.tool == canvas_tool::remove_brush) return &s.brush_px;
  if (s.tool == canvas_tool::red_eye) return &s.redeye_px;
  return nullptr;
}
[[nodiscard]] inline float tool_px(const tool_state& s) noexcept {
  if (s.tool == canvas_tool::remove_brush) return s.brush_px;
  if (s.tool == canvas_tool::red_eye) return s.redeye_px;
  return 0.0f;
}

// Sets the armed tool's size and starts the label's fade. False when the
// tool has no size or nothing changed.
inline bool set_tool_px(tool_state& s, float px, std::uint64_t now_ms) noexcept {
  float* p = tool_px_ref(s);
  if (!p) return false;
  const float v = clamp_tool_px(px);
  s.hud_until_ms = now_ms + kToolLabelMs;
  if (*p == v) return false;
  *p = v;
  return true;
}

// The brush erases with the bar's toggle on, or while Alt / Option is held
// (held inverts the toggle, as in a paint app).
[[nodiscard]] constexpr bool erasing(const tool_state& s) noexcept { return s.erase != s.erase_held; }

// The ring's radius in physical pixels: half the size, at the display scale.
[[nodiscard]] inline float ring_radius_px(float size_px, float dpi_scale) noexcept {
  return 0.5f * clamp_tool_px(size_px) * (dpi_scale > 0.0f ? dpi_scale : 1.0f);
}

// A radius in physical screen pixels as a fraction of the shown picture's long
// side (remove_brush's dab radius, edit_view's ring): the picture's long side
// on screen is canvas_map::screen_long_px.
[[nodiscard]] inline float radius_over_long(float radius_px, const canvas_map& m) noexcept {
  const float l = m.screen_long_px();
  return l > 0.0f ? radius_px / l : 0.0f;
}

// ---- Red-eye spots ----------------------------------------------------------------

// A spot (source uv, radius over the source diagonal) as the shown picture's
// uv and a radius over its long side, through the placement's map (output uv
// → source uv), inverted.
[[nodiscard]] inline std::array<float, 3> spot_to_shown(const edit::placement& p, const edit::red_eye_spot& s) noexcept {
  const edit::affine inv = edit::invert(p.map);
  const float u = inv.m[0] * s.x + inv.m[1] * s.y + inv.m[2];
  const float v = inv.m[3] * s.x + inv.m[4] * s.y + inv.m[5];
  const float sw = static_cast<float>(p.oriented.w), sh = static_cast<float>(p.oriented.h);
  const float long_side = static_cast<float>(std::max(p.cropped.w, p.cropped.h));
  const float r = long_side > 0.0f ? s.r * std::sqrt(sw * sw + sh * sh) / long_side : 0.0f;
  return {u, v, r};
}

// The other way: a shown-uv point with a radius over the shown long side as a
// spot. False off the picture.
[[nodiscard]] inline bool shown_to_spot(const edit::placement& p, float u, float v, float r_long,
                                        edit::red_eye_spot& out) noexcept {
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) return false;
  const float* m = p.map.m;
  const float sw = static_cast<float>(p.oriented.w), sh = static_cast<float>(p.oriented.h);
  const float diag = std::sqrt(sw * sw + sh * sh);
  const float long_side = static_cast<float>(std::max(p.cropped.w, p.cropped.h));
  out.x = m[0] * u + m[1] * v + m[2];
  out.y = m[3] * u + m[4] * v + m[5];
  out.r = diag > 0.0f ? std::clamp(r_long * long_side / diag, 0.001f, 0.1f) : edit::kRedEyeDefaultRadius;
  return true;
}

// The spot under shown uv (u, v): the nearest whose disc (at least `min_long`
// of the long side, so a tiny fix is still clickable) holds the point; -1 for
// none. `aspect` is the shown picture's width over height.
[[nodiscard]] inline int spot_at(const edit::placement& p, const edit::colour& c, float u, float v,
                                 float min_long) noexcept {
  const float w = static_cast<float>(p.cropped.w), h = static_cast<float>(p.cropped.h);
  const float l = std::max(w, h);
  if (l <= 0.0f) return -1;
  int best = -1;
  float best_d = 0.0f;
  for (int i = 0; i < std::min<int>(c.spot_count, edit::kRedEyeSpots); ++i) {
    const auto s = spot_to_shown(p, c.spots[static_cast<std::size_t>(i)]);
    const float dx = (u - s[0]) * w / l, dy = (v - s[1]) * h / l;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= std::max(s[2], min_long) && (best < 0 || d < best_d)) {
      best = i;
      best_d = d;
    }
  }
  return best;
}

// The colour without spot `i` (the rest keep their order): set_colour lands it
// as one op, so one Ctrl+Z puts the spot back.
[[nodiscard]] inline edit::colour without_spot(edit::colour c, int i) noexcept {
  if (i < 0 || i >= c.spot_count) return c;
  for (int k = i; k + 1 < c.spot_count; ++k) {
    c.spots[static_cast<std::size_t>(k)] = c.spots[static_cast<std::size_t>(k + 1)];
  }
  --c.spot_count;
  c.spots[c.spot_count] = {};
  return c;
}

// The colour with `add` appended (at most kRedEyeSpots in all; a spot on top
// of one already there is skipped). Find eyes lands them as one op.
[[nodiscard]] inline edit::colour with_spots(edit::colour c, std::span<const edit::red_eye_spot> add) noexcept {
  for (const edit::red_eye_spot& s : add) {
    if (c.spot_count >= edit::kRedEyeSpots) break;
    bool dup = false;
    for (int k = 0; k < c.spot_count; ++k) {
      const edit::red_eye_spot& o = c.spots[static_cast<std::size_t>(k)];
      const float dx = o.x - s.x, dy = o.y - s.y;
      if (dx * dx + dy * dy < o.r * o.r * 0.25f) dup = true;
    }
    if (!dup) c.spots[c.spot_count++] = s;
  }
  return c;
}

// ---- Eyes (the AI pack's eyes_json, People on) ------------------------------------

// An eye in source uv, with its iris radius over the source diagonal.
struct eye_point {
  float x = 0.0f, y = 0.0f, r = 0.0f;
};

// The eyes of the pack's eyes_json answer (mediaviewer_ai.h): each face's
// landmarks are right eye, left eye, nose, mouth corners (x, y; 0..1 of the
// upright still, the working image's uv). The iris is taken as a tenth of the
// distance between the eyes (an adult's ~11.7 mm iris over ~63 mm). `w` x `h`
// the upright still. Malformed faces are skipped.
[[nodiscard]] inline std::vector<eye_point> parse_eyes(std::string_view json_text, float w, float h) {
  std::vector<eye_point> out;
  auto doc = json::parse(json_text);
  if (!doc || w <= 0.0f || h <= 0.0f) return out;
  const json::value* faces = doc->find("faces");
  if (!faces || faces->k != json::kind::array) return out;
  const float diag = std::sqrt(w * w + h * h);
  for (const json::value& f : faces->a) {
    const json::value* lm = f.find("lm");
    if (!lm || lm->k != json::kind::array || lm->a.size() < 4) continue;
    const float rx = static_cast<float>(lm->a[0].d), ry = static_cast<float>(lm->a[1].d);
    const float lx = static_cast<float>(lm->a[2].d), ly = static_cast<float>(lm->a[3].d);
    if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(lx) || !std::isfinite(ly)) continue;
    const float dx = (lx - rx) * w, dy = (ly - ry) * h;
    const float iod = std::sqrt(dx * dx + dy * dy);
    if (!(iod > 0.0f)) continue;
    const float r = 0.1f * iod / diag;
    out.push_back({rx, ry, r});
    out.push_back({lx, ly, r});
  }
  return out;
}

// The eye the ring snaps to: the nearest detected eye whose centre is within
// kEyeSnapReach ring radii of the pointer. Everything in the shown picture's
// uv with radii over its long side (`eyes` already mapped, spot_to_shown);
// `aspect` w / h of the shown picture. The snapped ring is kEyeSnapIris of the
// iris. nullopt: no eye near, the ring stays where the pointer is.
[[nodiscard]] inline std::optional<std::array<float, 3>> snap_eye(std::span<const std::array<float, 3>> eyes, float u,
                                                                  float v, float ring_long, float aspect) noexcept {
  const float ax = aspect >= 1.0f ? 1.0f : aspect, ay = aspect >= 1.0f ? 1.0f / aspect : 1.0f;
  std::optional<std::array<float, 3>> best;
  float best_d = 0.0f;
  for (const auto& e : eyes) {
    const float dx = (u - e[0]) * ax, dy = (v - e[1]) * ay;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > kEyeSnapReach * ring_long) continue;
    if (!best || d < best_d) {
      best = std::array<float, 3>{e[0], e[1], e[2] * kEyeSnapIris};
      best_d = d;
    }
  }
  return best;
}

// The share of the disc (source uv, radius over the source diagonal) of the
// working image whose pixels read as a red pupil: red well above both green
// and blue, and not dark. Find eyes fixes an eye at kRedEyeRatio or more.
// Small enough for the UI thread (an iris is a few hundred pixels); the
// host still runs it on the pool with the rest of Find eyes.
[[nodiscard]] inline float red_ratio(const image::linear_image& img, const eye_point& e) noexcept {
  if (!img.valid()) return 0.0f;
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  const float r = e.r * std::sqrt(w * w + h * h);
  if (!(r > 0.5f)) return 0.0f;
  const float cx = e.x * w, cy = e.y * h;
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - r)));
  const int x1 = std::min(static_cast<int>(img.width) - 1, static_cast<int>(std::ceil(cx + r)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - r)));
  const int y1 = std::min(static_cast<int>(img.height) - 1, static_cast<int>(std::ceil(cy + r)));
  int n = 0, red = 0;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float px = static_cast<float>(x) + 0.5f - cx, py = static_cast<float>(y) + 0.5f - cy;
      if (px * px + py * py > r * r) continue;
      const std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4;
      const float R = image::half_to_float(p[0]), G = image::half_to_float(p[1]), B = image::half_to_float(p[2]);
      ++n;
      if (R > 0.02f && R > 2.0f * G && R > 1.5f * B) ++red;
    }
  }
  return n > 0 ? static_cast<float>(red) / static_cast<float>(n) : 0.0f;
}

// Find eyes: the spots for every detected eye that is red enough, as one list
// (the host lands it as one op through with_spots). Each spot is the iris
// grown by kEyeSnapIris.
[[nodiscard]] inline std::vector<edit::red_eye_spot> find_red_eyes(const image::linear_image& img,
                                                                   std::span<const eye_point> eyes) {
  std::vector<edit::red_eye_spot> out;
  for (const eye_point& e : eyes) {
    if (red_ratio(img, e) < kRedEyeRatio) continue;
    out.push_back({std::clamp(e.x, 0.0f, 1.0f), std::clamp(e.y, 0.0f, 1.0f),
                   std::clamp(e.r * kEyeSnapIris, 0.001f, 0.1f)});
  }
  return out;
}

// ---- The context bar ----------------------------------------------------------------

// What the bar below the photo shows (both chromes: the Mac's mv_tool_bar_view,
// Windows' chrome_tool_bar_args mirror it field for field).
struct tool_bar {
  std::int32_t visible = 0;
  std::int32_t tool = 0;     // canvas_tool
  float size_px = 0.0f;      // the ring's diameter, points
  std::int32_t erase = 0;    // the Erase toggle (not the held Alt)
  std::int32_t strokes = 0;  // the brush's strokes
  std::int32_t spots = 0;    // red-eye fixes on the photo
  std::int32_t can_apply = 0;  // Remove / Find eyes can run
  std::int32_t busy = 0;       // Find eyes is reading the photo
  std::int32_t progress_permille = 0;
  std::int32_t extend_w = 0;  // S6: the extended frame, pixels
  std::int32_t extend_h = 0;
  std::int32_t full = 0;      // the brush holds all it can: "Brush full: Remove, then brush more"
  std::int32_t can_redo = 0;
  // red-eye: eyes found on the photo (People on), 0 none / not known. S8:
  // with Faces armed, `eyes` is the faces found, `spots` the chosen ones and
  // `busy` a read or a fix on the pool.
  std::int32_t eyes = 0;
  std::int32_t can_undo = 0;  // S4a: a stroke or a Clear to undo (a cleared brush still undoes)
  // S6: Extend's readout "+18 % · 6048×4032" (extend_w x extend_h above),
  // the growth in permille, a side past the 25 % warning, and the preset chip
  // the frame is (shell::extend_preset_label; -1 a frame of the person's own).
  std::int32_t extend_growth = 0;
  std::int32_t extend_warn = 0;
  std::int32_t extend_preset = 0;
};
static_assert(sizeof(tool_bar) == 72);

// The bar's buttons, as both chromes send them back (mv_chrome_tool_action,
// chrome_cmd_canvas_tool's actions are commands; these are the bar's own).
enum class tool_bar_action : std::int32_t {
  done = 0,       // put the tool down (Esc)
  undo = 1,       // one stroke / spot (tool_undo)
  redo = 2,
  clear = 3,      // tool_clear
  apply = 4,      // Remove ↵ (tool_apply)
  find_eyes = 5,  // red-eye: fix every red eye found, one undo step
  // docs/plans/edit-refresh.md S8: the Faces bar, on the chosen faces.
  faces_open_eyes = 6,
  faces_improve = 7,
  faces_do_all = 8,
  // S6: Extend's preset chip i is extend_preset + i (shell/edit_workspace.h
  // kExtendPresetCount; 0 Original). Its Fill is apply, its Cancel done.
  extend_preset = 16,
};
// The bar's action for Extend's preset chip `i`, and back (-1 not one).
[[nodiscard]] constexpr std::int32_t extend_preset_action(int i) noexcept {
  return static_cast<std::int32_t>(tool_bar_action::extend_preset) + i;
}
[[nodiscard]] constexpr int extend_preset_of_action(std::int32_t action) noexcept {
  const int i = action - static_cast<std::int32_t>(tool_bar_action::extend_preset);
  return i >= 0 && i < kExtendPresetCount ? i : -1;
}

// The pointer over Extend's frame: what its cursor says (both hosts map these
// to their own resize cursors).
enum class extend_cursor : std::uint8_t { cross, left_right, up_down, nw_se, ne_sw, move };
[[nodiscard]] constexpr extend_cursor extend_cursor_of(std::uint8_t handle) noexcept {
  if (handle & extend_move) return extend_cursor::move;
  const bool h = (handle & (extend_left | extend_right)) != 0, v = (handle & (extend_top | extend_bottom)) != 0;
  if (h && v) {
    const bool nw = ((handle & extend_left) != 0) == ((handle & extend_top) != 0);
    return nw ? extend_cursor::nw_se : extend_cursor::ne_sw;
  }
  if (h) return extend_cursor::left_right;
  if (v) return extend_cursor::up_down;
  return extend_cursor::cross;
}
static_assert(static_cast<int>(canvas_tool::extend) == 5, "tool_overlay.h kToolExtendWire");
// Screen pixels (points times the display's scale) from an edge a press grabs it.
inline constexpr float kExtendGrabPx = 12.0f;

// ---- Onto the canvas -------------------------------------------------------------------

// What the host publishes of its tool into the canvas's edit_view (beside
// apply_brush's dabs). `ring_px` the ring's radius in physical pixels (0: no
// ring), `hover_spot` the spot a click would remove, `pulse` a fix just made
// (shown uv and radius over the long side; u < 0 none).
struct tool_view_in {
  canvas_tool tool = canvas_tool::none;
  bool erase = false;
  bool dim = false;
  float ring_px = 0.0f;
  float label_px = 0.0f;
  std::uint64_t label_until_ms = 0;
  int hover_spot = -1;
  std::array<float, 3> pulse{-1.0f, -1.0f, 0.0f};
  std::uint64_t pulse_until_ms = 0;
  // S6: Extend's frame and the edge(s) under the pointer or held.
  extend_draft extend{};
  std::uint8_t extend_handle = 0;
};

// The red-eye tool shows the photo's fixes, so a click on one can take it off.
inline void apply_tool(edit_view& v, const tool_view_in& in, const edit::placement& p, const edit::colour& c) noexcept {
  v.tool = static_cast<std::uint8_t>(in.tool);
  v.tool_erase = in.erase ? 1 : 0;
  v.tool_dim = in.dim ? 1 : 0;
  v.tool_ring_px = in.ring_px;
  v.tool_label_px = in.label_px;
  v.tool_label_until_ms = in.label_until_ms;
  v.tool_hover_spot = static_cast<std::int8_t>(std::clamp(in.hover_spot, -1, kToolSpots - 1));
  v.tool_spot_count = 0;
  if (in.tool == canvas_tool::red_eye) {
    for (int i = 0; i < std::min<int>(c.spot_count, kToolSpots); ++i) {
      const auto s = spot_to_shown(p, c.spots[static_cast<std::size_t>(i)]);
      v.tool_spots[i][0] = s[0];
      v.tool_spots[i][1] = s[1];
      v.tool_spots[i][2] = s[2];
      ++v.tool_spot_count;
    }
  }
  v.tool_pulse[0] = in.pulse[0];
  v.tool_pulse[1] = in.pulse[1];
  v.tool_pulse[2] = in.pulse[2];
  v.tool_pulse_until_ms = in.pulse_until_ms;
  // S6: Extend's frame, its warning and its readout (the shown picture's
  // pixels through the add-on's sums: the file is the size it reads).
  if (in.tool == canvas_tool::extend) {
    v.tool_extend[0] = in.extend.l;
    v.tool_extend[1] = in.extend.t;
    v.tool_extend[2] = in.extend.r;
    v.tool_extend[3] = in.extend.b;
    v.tool_extend_handle = in.extend_handle;
    v.tool_extend_warn = extend_warns(in.extend) ? 1 : 0;
    const extend_size_t sz = extend_size(in.extend, p.cropped.w, p.cropped.h);
    v.tool_extend_w = sz.width;
    v.tool_extend_h = sz.height;
    v.tool_extend_growth = sz.growth_permille;
  } else {
    v.tool_extend[0] = v.tool_extend[1] = v.tool_extend[2] = v.tool_extend[3] = 0.0f;
    v.tool_extend_handle = 0;
    v.tool_extend_warn = 0;
    v.tool_extend_w = v.tool_extend_h = 0;
    v.tool_extend_growth = 0;
  }
}

// S6: Extend's part of the bar: the readout and the chip the frame is.
inline void apply_extend_bar(tool_bar& b, const extend_draft& d, const edit::placement& p) noexcept {
  const extend_size_t sz = extend_size(d, p.cropped.w, p.cropped.h);
  b.extend_w = static_cast<std::int32_t>(sz.width);
  b.extend_h = static_cast<std::int32_t>(sz.height);
  b.extend_growth = sz.growth_permille;
  b.extend_warn = extend_warns(d) ? 1 : 0;
  b.extend_preset = extend_preset_of(d, p.cropped.w, p.cropped.h);
}

}  // namespace mv::shell
