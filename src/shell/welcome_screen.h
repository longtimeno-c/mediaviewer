// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The empty-window welcome: what the canvas shows before anything is open.
//
// One drawing routine for both present labs (D3D11 and Metal): plain ImGui
// draw-list calls on the background list, no platform types (D9). The host
// passes the wording that differs by OS (the open shortcut, the key legend).
// It is drawn only while there is no picture and no lab sweep, and it costs
// nothing once a file is open.
#pragma once

#include <algorithm>
#include <cfloat>

#include "imgui.h"

namespace mv::shell {

struct welcome_text {
  const char* open_hint;  // e.g. "or press Ctrl+O to choose a file, Ctrl+Shift+O for a folder"
  const char* keys;       // one muted line of the commonest keys
};

// `chrome` is the height (px) of the host's command bar covering the top of the
// canvas; `scale` is the DPI scale. Everything is laid out in `scale` units so it
// reads the same on a 100 % and a 200 % display.
inline void draw_welcome(ImDrawList* bg, ImFont* font, float w, float h, float chrome,
                         float scale, const welcome_text& text, float alpha = 1.0f) noexcept {
  if (bg == nullptr || font == nullptr || w <= 0.0f || h <= chrome || alpha <= 0.01f) return;
  // `alpha` < 1 is the hand-off to the runner (dino_draw.h): the card fades and
  // floats up as the game starts.
  const auto fade = [alpha](ImU32 c) {
    const auto a = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
  };

  const ImU32 c_title = fade(IM_COL32(226, 228, 234, 255));
  const ImU32 c_body = fade(IM_COL32(160, 164, 174, 255));
  const ImU32 c_mute = fade(IM_COL32(112, 116, 128, 255));
  const ImU32 c_edge = fade(IM_COL32(255, 255, 255, 34));
  const ImU32 c_fill = fade(IM_COL32(255, 255, 255, 10));
  const ImU32 c_glyph = fade(IM_COL32(150, 156, 172, 255));

  const auto measure = [&](const char* s, float fs) {
    return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s);
  };
  const auto centred = [&](const char* s, float fs, float cx, float y, ImU32 col) {
    const ImVec2 sz = measure(s, fs);
    bg->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, y), col, s);
  };

  const float avail_w = w - 48.0f * scale;
  const float card_w = std::min(560.0f * scale, avail_w);
  const float card_h = 262.0f * scale;
  const float cx = w * 0.5f;
  const float cy = chrome + (h - chrome) * 0.5f - (1.0f - alpha) * 28.0f * scale;
  const ImVec2 lo(cx - card_w * 0.5f, cy - card_h * 0.5f);
  const ImVec2 hi(cx + card_w * 0.5f, cy + card_h * 0.5f);
  if (card_h > h - chrome) return;  // window too short: draw nothing rather than clip

  const float round = 18.0f * scale;
  bg->AddRectFilled(lo, hi, c_fill, round);
  bg->AddRect(lo, hi, c_edge, round, 0, 1.5f * scale);

  // A picture glyph from primitives: frame, sun, two hills. No asset to load.
  const float gs = 46.0f * scale;
  const ImVec2 g0(cx - gs, lo.y + 34.0f * scale);
  const ImVec2 g1(cx + gs, g0.y + gs * 1.5f);
  bg->AddRect(g0, g1, c_glyph, 8.0f * scale, 0, 2.0f * scale);
  bg->AddCircleFilled(ImVec2(g0.x + gs * 0.55f, g0.y + gs * 0.5f), 7.0f * scale, c_glyph);
  const float base = g1.y - 5.0f * scale;
  bg->AddTriangleFilled(ImVec2(g0.x + 6.0f * scale, base), ImVec2(g0.x + gs * 0.75f, g0.y + gs * 0.85f),
                        ImVec2(g0.x + gs * 1.2f, base), c_glyph);
  bg->AddTriangleFilled(ImVec2(g0.x + gs * 0.85f, base), ImVec2(g0.x + gs * 1.4f, g0.y + gs * 0.95f),
                        ImVec2(g1.x - 6.0f * scale, base), c_glyph);

  float y = g1.y + 22.0f * scale;
  centred("Drop photos, videos or a folder here", 22.0f * scale, cx, y, c_title);
  y += 36.0f * scale;
  centred(text.open_hint, 15.0f * scale, cx, y, c_body);
  y += 34.0f * scale;
  centred("JPEG  PNG  GIF  WebP  TIFF  HEIC  AVIF  RAW  ·  MP4  MOV  MKV  WebM", 13.0f * scale, cx, y,
          c_mute);
  y += 22.0f * scale;
  centred(text.keys, 13.0f * scale, cx, y, c_mute);
}

}  // namespace mv::shell
