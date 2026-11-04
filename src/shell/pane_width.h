// SPDX-License-Identifier: GPL-3.0-or-later
// The right column's width (docs/plans/edit-refresh.md §2.1, docs/design/20
// "The right column"): one width for every right pane — the Edit inspector,
// Adjust, Info and Jobs — dragged by its edge, shared by both hosts. Header
// only, no platform types. Units are the host's layout units (points on the
// Mac, DIPs on Windows); the hosts scale to pixels themselves.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mv::shell {

struct pane_width_limits {
  float min = 280.0f;        // the narrowest inspector
  float def = 360.0f;        // a fresh install, a double-click on the edge
  float max = 720.0f;        // the widest inspector
  float rail = 44.0f;        // the icon rail beside it (0 = no rail)
  float canvas_min = 320.0f; // the canvas never gets narrower than this
};

// pane_wider / pane_narrower (docs/design/16): one step of the column.
inline constexpr float kPaneWidthStep = 40.0f;
// The edge's grab strip, left of the column.
inline constexpr float kPaneEdgeGrab = 6.0f;
// The breakpoints a release snaps to when it lands within this of one.
inline constexpr float kPaneSnapDistance = 6.0f;

// The narrowest the inspector may be for this theme: the limits' minimum or
// the chrome's own (density and type scale widen it), whichever is wider.
[[nodiscard]] inline float inspector_min(float theme_min, const pane_width_limits& l = {}) noexcept {
  return std::max(l.min, theme_min);
}

// The inspector width a host lays out for a wanted width in a client area
// `client` wide. 0 means the client is too narrow for any inspector beside
// the canvas: show the rail only. Never more than 60 % of the client (less
// the rail), never so wide the canvas gets under canvas_min, never less than
// the theme's minimum.
[[nodiscard]] inline float clamp_inspector(float want, float client, float theme_min,
                                           const pane_width_limits& l = {}) noexcept {
  const float min_eff = inspector_min(theme_min, l);
  if (!(client > 0.0f) || client - l.rail - min_eff < l.canvas_min) return 0.0f;
  const float max_eff =
      std::max(min_eff, std::min({l.max, 0.6f * client - l.rail, client - l.rail - l.canvas_min}));
  if (!std::isfinite(want) || want <= 0.0f) want = l.def;
  return std::clamp(want, min_eff, max_eff);
}

// What a host without a rail shows (S1, before the rail lands): the clamp,
// or, where the clamp says "rail only", the narrowest inspector that fits in
// half the client — a pane with no rail to collapse to still shows.
[[nodiscard]] inline float shown_inspector(float want, float client, float theme_min,
                                           const pane_width_limits& l = {}) noexcept {
  const float w = clamp_inspector(want, client, theme_min, l);
  if (w > 0.0f || l.rail > 0.0f) return w;
  return std::max(0.0f, std::min(inspector_min(theme_min, l), client * 0.5f));
}

// The width the docked workspace takes from the canvas: the rail and the
// inspector, the rail alone when collapsed, nothing when closed.
[[nodiscard]] inline float docked_right(float inspector, bool open, bool collapsed,
                                        const pane_width_limits& l = {}) noexcept {
  if (!open) return 0.0f;
  if (collapsed) return l.rail;
  return l.rail + std::max(0.0f, inspector);
}

// A release near a breakpoint lands on it (360 the default, 560 two columns).
[[nodiscard]] inline float snap_inspector(float w) noexcept {
  for (const float at : {360.0f, 560.0f}) {
    if (std::fabs(w - at) <= kPaneSnapDistance) return at;
  }
  return w;
}

// The stored preference: rounded to 4, 0 = the default (a fresh install).
[[nodiscard]] inline std::int32_t stored_width(float w) noexcept {
  if (!std::isfinite(w) || w <= 0.0f) return 0;
  return static_cast<std::int32_t>(std::lround(w / 4.0f)) * 4;
}
[[nodiscard]] inline float preferred_width(std::int32_t stored, const pane_width_limits& l = {}) noexcept {
  if (stored <= 0) return l.def;
  return std::clamp(static_cast<float>(stored), l.min, l.max);
}

// One pane_wider / pane_narrower step from the width on screen, as the new
// preference (the host clamps it to the client when it lays out).
[[nodiscard]] inline float step_inspector(float shown, int direction, const pane_width_limits& l = {}) noexcept {
  const float from = shown > 0.0f ? shown : l.def;
  return std::clamp(from + (direction < 0 ? -kPaneWidthStep : kPaneWidthStep), l.min, l.max);
}

// The inspector's layout class (docs/plans/edit-refresh.md §2.5). By the
// inspector's width only.
enum class width_class : std::uint8_t { compact, regular, wide, xwide };

[[nodiscard]] inline width_class class_of(float w) noexcept {
  if (w < 340.0f) return width_class::compact;
  if (w < 560.0f) return width_class::regular;
  if (w < 720.0f) return width_class::wide;
  return width_class::xwide;
}

// The Curves square's side for an inspector `w` wide (docs/plans/edit-refresh.md
// §2.6): the pane's content width, capped at 340 from regular up. The xwide
// scopes row (histogram beside Curves) is the inspector refresh's.
[[nodiscard]] inline float curve_side(float w) noexcept {
  const float content = std::max(0.0f, w - 28.0f);
  return class_of(w) == width_class::compact ? content : std::min(content, 340.0f);
}

}  // namespace mv::shell
