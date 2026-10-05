// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The empty-window welcome: what the canvas shows before anything is open.
//
// One drawing routine for both present labs (D3D11 and Metal): plain ImGui
// draw-list calls on the background list, no platform types (D9). The host
// passes the wording that differs by OS (the open shortcut, the key legend).
// It is drawn only while there is no picture and no lab sweep, and it costs
// nothing once a file is open. Below the hints it lists the recent folders the
// host hands it (welcome_layout.h); the host hit-tests the same geometry. The
// hovered row shows an x that removes the folder from the list. On the Mac the
// first row can be the iCloud Photos library, drawn with a cloud.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cstring>

#include "imgui.h"
#include "shell/home_theme.h"
#include "shell/welcome_layout.h"

namespace mv::shell {

struct welcome_text {
  const char* open_hint;  // e.g. "or press Ctrl+O to choose a file, Ctrl+Shift+O for a folder"
  const char* keys;       // one muted line of the commonest keys
};

// `text` cut to `max_w` with a trailing ellipsis (`front` = cut the start
// instead, for a path whose tail is the useful end). UTF-8 safe.
inline const char* welcome_fit(ImFont* font, float fs, const char* text, float max_w, bool front,
                               char (&buf)[256]) noexcept {
  if (font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text).x <= max_w) return text;
  static constexpr char kEllipsis[] = "\xE2\x80\xA6";  // U+2026
  const std::size_t len = std::min(std::strlen(text), sizeof(buf) - sizeof(kEllipsis));
  const auto is_cont = [](char c) { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; };
  for (std::size_t keep = len; keep > 0; --keep) {
    if (front) {
      const char* tail = text + (std::strlen(text) - keep);
      if (is_cont(*tail)) continue;
      std::memcpy(buf, kEllipsis, sizeof(kEllipsis) - 1);
      std::memcpy(buf + sizeof(kEllipsis) - 1, tail, keep);
      buf[sizeof(kEllipsis) - 1 + keep] = '\0';
    } else {
      if (is_cont(text[keep])) continue;
      std::memcpy(buf, text, keep);
      std::memcpy(buf + keep, kEllipsis, sizeof(kEllipsis));
    }
    if (font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf).x <= max_w) return buf;
  }
  return "";
}

// `chrome` is the height (px) of the host's command bar covering the top of the
// canvas; `scale` is the DPI scale. Everything is laid out in `scale` units so it
// reads the same on a 100 % and a 200 % display. `recents` may be null.
inline void draw_welcome(ImDrawList* bg, ImFont* font, float w, float h, float chrome,
                         float scale, const welcome_text& text, const home_palette& theme,
                         float alpha = 1.0f, const welcome_recents* recents = nullptr) noexcept {
  if (bg == nullptr || font == nullptr || w <= 0.0f || h <= chrome || alpha <= 0.01f) return;
  // `alpha` < 1 is the hand-off to the runner (dino_draw.h): the card fades and
  // floats up as the game starts.
  const auto fade = [alpha](ImU32 c) {
    const auto a = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
  };

  const ImU32 c_title = fade(theme.title());
  const ImU32 c_body = fade(theme.body());
  const ImU32 c_mute = fade(theme.muted());
  const ImU32 c_edge = fade(theme.edge());
  const ImU32 c_fill = fade(theme.fill());
  const ImU32 c_glyph = fade(theme.muted());

  const auto measure = [&](const char* s, float fs) {
    return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s);
  };
  const auto centred = [&](const char* s, float fs, float cx, float y, ImU32 col) {
    const ImVec2 sz = measure(s, fs);
    bg->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, y), col, s);
  };

  const int want_rows = recents != nullptr ? recents->count : 0;
  const welcome_geometry g =
      layout_welcome(w, h, chrome, scale, want_rows, (1.0f - alpha) * 28.0f * scale);
  if (!g.fits) return;  // window too short: draw nothing rather than clip
  const float cx = w * 0.5f;
  const ImVec2 lo(g.lo_x, g.lo_y);
  const ImVec2 hi(g.hi_x, g.hi_y);

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

  if (g.rows <= 0) return;
  // Recent folders: a hairline, a quiet header, then one row per folder. The
  // hovered row gets the card's fill again so it reads as a button.
  const float rule_y = lo.y + (kWelcomeBaseH - 6.0f) * scale;
  bg->AddLine(ImVec2(lo.x + 24.0f * scale, rule_y), ImVec2(hi.x - 24.0f * scale, rule_y), c_edge,
              1.0f * scale);
  bg->AddText(font, 12.0f * scale, ImVec2(g.row_x0 + 10.0f * scale, g.rows_top - 20.0f * scale), c_mute,
              "RECENT FOLDERS");
  char label_buf[256];
  char where_buf[256];
  for (int i = 0; i < g.rows; ++i) {
    const float top = g.rows_top + static_cast<float>(i) * g.row_h;
    if (recents->hover == i) {
      bg->AddRectFilled(ImVec2(g.row_x0, top + 1.0f * scale), ImVec2(g.row_x1, top + g.row_h - 1.0f * scale),
                        c_fill, 8.0f * scale);
      bg->AddRect(ImVec2(g.row_x0, top + 1.0f * scale), ImVec2(g.row_x1, top + g.row_h - 1.0f * scale),
                  c_edge, 8.0f * scale, 0, 1.0f * scale);
    }
    const bool icloud_row = recents->icloud && i == 0;
    const float fx = g.row_x0 + 10.0f * scale;
    const float fy = top + g.row_h * 0.5f - 6.0f * scale;
    if (icloud_row) {
      // A cloud: a flat base and three puffs, the middle one tallest.
      const float base_y = fy + 12.0f * scale;
      bg->AddRectFilled(ImVec2(fx + 1.0f * scale, base_y - 5.0f * scale), ImVec2(fx + 17.0f * scale, base_y),
                        c_glyph, 2.5f * scale);
      bg->AddCircleFilled(ImVec2(fx + 4.5f * scale, base_y - 4.0f * scale), 3.5f * scale, c_glyph);
      bg->AddCircleFilled(ImVec2(fx + 9.5f * scale, base_y - 6.5f * scale), 5.0f * scale, c_glyph);
      bg->AddCircleFilled(ImVec2(fx + 14.0f * scale, base_y - 4.5f * scale), 3.5f * scale, c_glyph);
    } else {
      // A folder glyph: tab and body.
      bg->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + 7.0f * scale, fy + 3.0f * scale), c_glyph, 1.0f * scale);
      bg->AddRectFilled(ImVec2(fx, fy + 2.0f * scale), ImVec2(fx + 16.0f * scale, fy + 12.0f * scale), c_glyph,
                        2.0f * scale);
    }

    const float fs_label = 15.0f * scale;
    const float fs_where = 13.0f * scale;
    const float text_x = fx + 26.0f * scale;
    // The remove button's room is kept on every row, so hovering never shifts the text.
    const float right = g.row_x1 - kWelcomeRemoveW * scale;
    const float room = right - text_x;
    const char* label = welcome_fit(font, fs_label, recents->label[i], room * 0.6f, false, label_buf);
    const float label_w = measure(label, fs_label).x;
    bg->AddText(font, fs_label, ImVec2(text_x, top + (g.row_h - fs_label) * 0.5f - 1.0f * scale),
                recents->hover == i ? c_title : c_body, label);
    const float where_room = room - label_w - 20.0f * scale;
    if (where_room > 24.0f * scale && recents->where[i][0] != '\0') {
      const char* where = welcome_fit(font, fs_where, recents->where[i], where_room, true, where_buf);
      const float where_w = measure(where, fs_where).x;
      bg->AddText(font, fs_where, ImVec2(right - where_w, top + (g.row_h - fs_where) * 0.5f), c_mute, where);
    }
    // The hovered row's remove button: an x, ringed while the pointer is on it.
    // The iCloud row has none: it leaves the card when the library is removed
    // in Settings, not from here.
    if (recents->hover == i && !icloud_row) {
      const ImVec2 c(g.row_x1 - kWelcomeRemoveW * 0.5f * scale, top + g.row_h * 0.5f);
      if (recents->hover_remove) bg->AddCircleFilled(c, 10.0f * scale, c_edge);
      const ImU32 c_x = recents->hover_remove ? c_title : c_mute;
      const float r = 4.0f * scale;
      bg->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), c_x, 1.5f * scale);
      bg->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), c_x, 1.5f * scale);
    }
  }
}

}  // namespace mv::shell
