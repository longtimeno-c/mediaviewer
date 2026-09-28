// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The welcome card's geometry and its recent-folder rows, with no ImGui and no
// platform types (D9), so a host's click and the render thread's drawing use
// one layout and cannot disagree about which row is where.
//
// The host owns the list and the hit test (UI thread); the render thread only
// draws the bytes it is handed in the snapshot. It never sees a path it could
// open, and a hover change is one redraw, never a repaint while idle.
#pragma once

#include <algorithm>
#include <cstdint>

namespace mv::shell {

// The recent folders the welcome card lists, as the snapshot carries them.
// POD: `label` is the folder's display name ("2026-09 Iceland"), `where` its
// parent as the user would write it ("~/Pictures"); both UTF-8, NUL-terminated,
// already cut at a character boundary by the host.
struct welcome_recents {
  static constexpr int kMax = 6;
  std::uint8_t count = 0;  // 0 = no rows: nothing opened yet, or something is open
  std::int8_t hover = -1;  // the row under the pointer, drawn highlighted
  char label[kMax][96] = {};
  char where[kMax][192] = {};
};

// Every length is in `scale` units (DPI), as draw_welcome lays out.
struct welcome_geometry {
  float lo_x = 0.0f, lo_y = 0.0f, hi_x = 0.0f, hi_y = 0.0f;  // the card
  float rows_top = 0.0f;  // first row's top edge; the header sits above it
  float row_x0 = 0.0f, row_x1 = 0.0f;
  float row_h = 0.0f;
  int rows = 0;       // how many recent rows fit (<= the count asked for)
  bool fits = false;  // false: the window is too short for even the plain card
};

inline constexpr float kWelcomeBaseH = 262.0f;    // the card without recents
inline constexpr float kWelcomeRecentsTop = 292.0f;  // first row, from the card's top
inline constexpr float kWelcomeRowH = 32.0f;
inline constexpr float kWelcomeRowsPad = 14.0f;   // below the last row

// `chrome` is the command bar covering the top of the canvas; `lift` floats the
// card up as it fades into the runner (draw_welcome's `alpha`).
[[nodiscard]] inline welcome_geometry layout_welcome(float w, float h, float chrome, float scale,
                                                     int recent_count, float lift = 0.0f) noexcept {
  welcome_geometry g;
  if (w <= 0.0f || h <= chrome || scale <= 0.0f) return g;
  const float avail_h = h - chrome;
  const float card_w = std::min(560.0f * scale, w - 48.0f * scale);
  if (card_w <= 0.0f || kWelcomeBaseH * scale > avail_h) return g;
  // As many rows as fit with a margin; the rest wait in the Dock menu / jump list.
  int rows = std::clamp(recent_count, 0, welcome_recents::kMax);
  const auto card_h_for = [&](int n) {
    return n == 0 ? kWelcomeBaseH * scale
                  : (kWelcomeRecentsTop + static_cast<float>(n) * kWelcomeRowH + kWelcomeRowsPad) * scale;
  };
  while (rows > 0 && card_h_for(rows) > avail_h - 32.0f * scale) --rows;
  const float card_h = card_h_for(rows);
  const float cx = w * 0.5f;
  const float cy = chrome + avail_h * 0.5f - lift;
  g.lo_x = cx - card_w * 0.5f;
  g.hi_x = cx + card_w * 0.5f;
  g.lo_y = cy - card_h * 0.5f;
  g.hi_y = cy + card_h * 0.5f;
  g.rows = rows;
  g.row_h = kWelcomeRowH * scale;
  g.rows_top = g.lo_y + kWelcomeRecentsTop * scale;
  g.row_x0 = g.lo_x + 14.0f * scale;
  g.row_x1 = g.hi_x - 14.0f * scale;
  g.fits = true;
  return g;
}

// The row at (x, y) in the canvas's pixels, or -1.
[[nodiscard]] inline int welcome_row_at(const welcome_geometry& g, float x, float y) noexcept {
  if (!g.fits || g.rows <= 0 || x < g.row_x0 || x >= g.row_x1 || y < g.rows_top) return -1;
  const int row = static_cast<int>((y - g.rows_top) / g.row_h);
  return row < g.rows ? row : -1;
}

}  // namespace mv::shell
