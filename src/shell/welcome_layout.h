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
  bool hover_remove = false;  // the pointer is on that row's remove button
  // Row 0 is a cloud place, not a recent folder: drawn with a cloud and
  // without a remove button, above the RECENT FOLDERS header. The folders
  // follow, one row down. The Mac: the iCloud Photos library (docs/design/26),
  // while it is added in Settings. Windows: OneDrive, while it is set up on
  // this PC (2026-10-07). The name is the Mac's, from before Windows had one.
  bool icloud = false;
  char label[kMax][96] = {};
  char where[kMax][192] = {};
};

// Every length is in `scale` units (DPI), as draw_welcome lays out.
struct welcome_geometry {
  float lo_x = 0.0f, lo_y = 0.0f, hi_x = 0.0f, hi_y = 0.0f;  // the card
  float rows_top = 0.0f;     // the first row's top edge
  float folders_top = 0.0f;  // the first folder row's; the RECENT FOLDERS header sits above it
  float row_x0 = 0.0f, row_x1 = 0.0f;
  float row_h = 0.0f;
  int rows = 0;       // how many rows fit (<= the count asked for), the lead row included
  bool lead = false;  // row 0 is the iCloud Photos library, above the header
  bool fits = false;  // false: the window is too short for even the plain card
};

inline constexpr float kWelcomeBaseH = 262.0f;    // the card without recents
inline constexpr float kWelcomeRecentsTop = 292.0f;  // first folder row, from the card's top
inline constexpr float kWelcomeLeadTop = 266.0f;  // the library's row, just under the hairline
inline constexpr float kWelcomeHeaderH = 34.0f;   // the header's room above the folder rows
inline constexpr float kWelcomeRowH = 32.0f;
inline constexpr float kWelcomeRowsPad = 14.0f;   // below the last row
inline constexpr float kWelcomeRemoveW = 30.0f;   // a row's remove button, at its right end

// `chrome` is the command bar covering the top of the canvas; `lift` floats the
// card up as it fades into the runner (draw_welcome's `alpha`). `lead`: the first
// of `recent_count` rows is the iCloud Photos library, drawn on its own above
// the RECENT FOLDERS header rather than as the first recent folder.
[[nodiscard]] inline welcome_geometry layout_welcome(float w, float h, float chrome, float scale,
                                                     int recent_count, float lift = 0.0f,
                                                     bool lead = false) noexcept {
  welcome_geometry g;
  if (w <= 0.0f || h <= chrome || scale <= 0.0f) return g;
  const float avail_h = h - chrome;
  const float card_w = std::min(560.0f * scale, w - 48.0f * scale);
  if (card_w <= 0.0f || kWelcomeBaseH * scale > avail_h) return g;
  // As many rows as fit with a margin; the rest wait in the Dock menu / jump list.
  int rows = std::clamp(recent_count, 0, welcome_recents::kMax);
  lead = lead && rows > 0;
  const int lead_rows = lead ? 1 : 0;
  const float folders_top = lead ? kWelcomeLeadTop + kWelcomeRowH + kWelcomeHeaderH : kWelcomeRecentsTop;
  const auto card_h_for = [&](int n) {
    if (n == 0) return kWelcomeBaseH * scale;
    const int folders = n - lead_rows;
    const float last = folders > 0 ? folders_top + static_cast<float>(folders) * kWelcomeRowH
                                   : kWelcomeLeadTop + kWelcomeRowH;
    return (last + kWelcomeRowsPad) * scale;
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
  g.lead = lead && rows > 0;
  g.row_h = kWelcomeRowH * scale;
  g.folders_top = g.lo_y + folders_top * scale;
  g.rows_top = g.lead ? g.lo_y + kWelcomeLeadTop * scale : g.folders_top;
  g.row_x0 = g.lo_x + 14.0f * scale;
  g.row_x1 = g.hi_x - 14.0f * scale;
  g.fits = true;
  return g;
}

// Row `i`'s top edge (0 <= i < g.rows).
[[nodiscard]] inline float welcome_row_top(const welcome_geometry& g, int i) noexcept {
  if (g.lead) return i == 0 ? g.rows_top : g.folders_top + static_cast<float>(i - 1) * g.row_h;
  return g.rows_top + static_cast<float>(i) * g.row_h;
}

// The row at (x, y) in the canvas's pixels, or -1.
[[nodiscard]] inline int welcome_row_at(const welcome_geometry& g, float x, float y) noexcept {
  if (!g.fits || g.rows <= 0 || x < g.row_x0 || x >= g.row_x1 || y < g.rows_top) return -1;
  if (g.lead) {
    if (y < g.rows_top + g.row_h) return 0;
    if (y < g.folders_top) return -1;  // the header between them
    const int row = 1 + static_cast<int>((y - g.folders_top) / g.row_h);
    return row < g.rows ? row : -1;
  }
  const int row = static_cast<int>((y - g.rows_top) / g.row_h);
  return row < g.rows ? row : -1;
}

// Whether x on a row (welcome_row_at >= 0) is its remove button rather than
// the folder: the hovered row draws an x there that drops it from the list.
[[nodiscard]] inline bool welcome_on_remove(const welcome_geometry& g, float x) noexcept {
  return g.fits && x >= g.row_x1 - kWelcomeRemoveW * (g.row_h / kWelcomeRowH) && x < g.row_x1;
}

}  // namespace mv::shell
