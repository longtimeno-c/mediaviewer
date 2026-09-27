// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The "can't open this file" card: what the canvas shows when the item on
// screen failed to open (a corrupt or unsupported file), instead of the
// empty-window welcome or the previous item.
//
// Same rules as welcome_screen.h: plain ImGui draw-list calls on the
// background list, no platform types (D9), drawn only while there is no
// picture, and it costs nothing once one lands. It names no file; the chrome
// already shows which item is selected.
#pragma once

#include <algorithm>
#include <cfloat>

#include "core/status.h"
#include "imgui.h"
#include "shell/home_theme.h"

namespace mv::shell {

// One line under the title. Never the path or the decoder's own message.
[[nodiscard]] constexpr const char* open_error_reason(status why) noexcept {
  switch (why) {
    case status::unsupported_format: return "This file is not in a format MediaViewer can open.";
    case status::corrupt:            return "The file looks damaged or incomplete.";
    case status::io:
    case status::not_found:
    case status::permission_denied:  return "The file could not be read.";
    case status::out_of_memory:      return "There was not enough memory to open it.";
    default:                         return "It may be damaged or in a format MediaViewer does not read.";
  }
}

// `hint` is the host's wording for moving on (the arrow keys are the same on
// both hosts today, but the legend is the host's to own, as for the welcome).
inline void draw_open_error(ImDrawList* bg, ImFont* font, float w, float h, float chrome,
                            float scale, status why, const char* hint,
                            const home_palette& theme) noexcept {
  if (bg == nullptr || font == nullptr || w <= 0.0f || h <= chrome) return;

  const auto centred = [&](const char* s, float fs, float cx, float y, ImU32 col) {
    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s);
    bg->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, y), col, s);
  };

  const float card_w = std::min(520.0f * scale, w - 48.0f * scale);
  const float card_h = 206.0f * scale;
  if (card_h > h - chrome) return;  // window too short: draw nothing rather than clip
  const float cx = w * 0.5f;
  const float cy = chrome + (h - chrome) * 0.5f;
  const ImVec2 lo(cx - card_w * 0.5f, cy - card_h * 0.5f);
  const ImVec2 hi(cx + card_w * 0.5f, cy + card_h * 0.5f);

  const float round = 18.0f * scale;
  bg->AddRectFilled(lo, hi, theme.fill(), round);
  bg->AddRect(lo, hi, theme.edge(), round, 0, 1.5f * scale);

  // A picture frame with a slash through it, from primitives. No asset to load.
  const float gw = 40.0f * scale;
  const float gh = 30.0f * scale;
  const ImVec2 g0(cx - gw, lo.y + 30.0f * scale);
  const ImVec2 g1(cx + gw, g0.y + gh * 2.0f);
  const ImU32 c_glyph = theme.muted();
  bg->AddRect(g0, g1, c_glyph, 8.0f * scale, 0, 2.0f * scale);
  bg->AddLine(ImVec2(g0.x + 10.0f * scale, g1.y - 8.0f * scale),
              ImVec2(g1.x - 10.0f * scale, g0.y + 8.0f * scale), c_glyph, 2.5f * scale);

  float y = g1.y + 20.0f * scale;
  centred("Can't open this file", 22.0f * scale, cx, y, theme.title());
  y += 34.0f * scale;
  centred(open_error_reason(why), 15.0f * scale, cx, y, theme.body());
  if (hint != nullptr && hint[0] != '\0') {
    y += 28.0f * scale;
    centred(hint, 13.0f * scale, cx, y, theme.muted());
  }
}

}  // namespace mv::shell
