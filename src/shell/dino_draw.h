// SPDX-License-Identifier: GPL-2.0-or-later
// Draws dino_game (dino_game.h) with ImGui draw-list primitives, on the
// background list, for both present labs (D9: no platform types). Nothing here
// loads an asset: the runner is a 20x21 pixel sprite, the rest is rectangles.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "shell/dino_game.h"

namespace mv::shell {

namespace dino_detail {

inline constexpr const char* kBody[17] = {
    "..........########..", ".........##########.", ".........##.########", ".........###########",
    ".........###########", ".........####.......", ".........#########..", "#.......####........",
    "#......######.#.....", "##....#########.....", "###..############...", "#################...",
    ".################...", "..###############...", "...#############....", "....###########.....",
    ".....#########......",
};
inline constexpr const char* kLegsA[4] = {"......###..###......", "......##....#.......",
                                          "......#.....###.....", "......###..........."};
inline constexpr const char* kLegsB[4] = {"......###..###......", ".......#....##......",
                                          ".......###...#......", "...........####....."};

inline ImU32 with_alpha(ImU32 c, float a) noexcept {
  const auto base = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF);
  const auto out = static_cast<ImU32>(std::clamp(base * a, 0.0f, 255.0f));
  return (c & ~IM_COL32_A_MASK) | (out << IM_COL32_A_SHIFT);
}

inline float ease_out(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Cheap stable hash for the scrolling ground texture: same cell, same pebble.
inline std::uint32_t hash_cell(std::int64_t i) noexcept {
  auto x = static_cast<std::uint32_t>(i) * 2654435761u;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return x;
}

inline void draw_rows(ImDrawList* dl, const char* const* rows, int n, float x0, float y0, float u,
                      ImU32 col) noexcept {
  for (int r = 0; r < n; ++r) {
    const char* row = rows[r];
    int c = 0;
    while (row[c] != '\0') {
      if (row[c] != '#') {
        ++c;
        continue;
      }
      int e = c;
      while (row[e] == '#') ++e;
      dl->AddRectFilled(ImVec2(x0 + static_cast<float>(c) * u, y0 + static_cast<float>(r) * u),
                        ImVec2(x0 + static_cast<float>(e) * u, y0 + static_cast<float>(r + 1) * u), col);
      c = e;
    }
  }
}

}  // namespace dino_detail

// How much of the welcome card is still visible while the game starts: 1 idle,
// falling to 0 in the first third of the intro. The host passes this to
// draw_welcome(); once the run begins it is 0 and the card is not drawn at all.
inline float welcome_alpha(const dino_game& g) noexcept {
  switch (g.state()) {
    case dino_game::phase::idle: return 1.0f;
    case dino_game::phase::intro: return 1.0f - dino_detail::ease_out(g.intro_progress() / 0.3f);
    default: return 0.0f;
  }
}

// Draws the ground, clouds, obstacles, runner and score. `w`,`h` and `chrome`
// are canvas pixels; the scene sits below the command bar. Returns without
// drawing anything while the game is idle.
inline void draw_dino(ImDrawList* dl, ImFont* font, const dino_game& g, float w, float h,
                      float chrome, float scale) noexcept {
  using namespace dino_detail;
  if (dl == nullptr || font == nullptr || !g.active() || w <= 0.0f || h <= chrome) return;

  const ImU32 ink = IM_COL32(226, 228, 234, 255);
  const ImU32 soft = IM_COL32(120, 124, 136, 255);
  const ImU32 cloud = IM_COL32(150, 156, 172, 90);
  const float u = 3.0f * scale;                       // pixels per world unit
  const float ground = chrome + (h - chrome) * 0.62f;  // y of world y = 0
  const float cx = w * 0.5f;
  const float p = g.intro_progress();
  const bool intro = g.state() == dino_game::phase::intro;

  // Ground line: in the intro it grows outwards from the centre.
  const float reveal = intro ? ease_out((p - 0.12f) / 0.5f) : 1.0f;
  const float half = (w * 0.5f) * reveal;
  if (half > 0.0f) dl->AddLine(ImVec2(cx - half, ground), ImVec2(cx + half, ground), ink, 2.0f * scale);

  // Pebbles and dashes on the ground, scrolling with the distance run.
  if (half > 0.0f) {
    const float cell = 11.0f;
    const auto first = static_cast<std::int64_t>(std::floor(g.distance() / cell));
    const int cols = static_cast<int>(w / (cell * u)) + 3;
    for (int i = 0; i < cols; ++i) {
      const std::int64_t id = first + i;
      const std::uint32_t hv = hash_cell(id);
      if ((hv & 3u) == 0u) continue;
      const float wx = (static_cast<float>(id) * cell - g.distance()) * u + static_cast<float>(hv % 7u) * u;
      if (wx < cx - half || wx > cx + half) continue;
      const float len = (1.0f + static_cast<float>((hv >> 4) % 3u)) * u;
      const float dy = (5.0f + static_cast<float>((hv >> 8) % 3u) * 3.0f) * u;
      dl->AddRectFilled(ImVec2(wx, ground + dy), ImVec2(wx + len, ground + dy + 1.4f * scale), soft);
    }
  }

  // Clouds drift slower than the ground (parallax), fading in with the intro.
  {
    const float fade = intro ? ease_out((p - 0.3f) / 0.5f) : 1.0f;
    const float span = w + 240.0f * scale;
    for (int k = 0; k < 4; ++k) {
      const float base = static_cast<float>(k) * (span / 4.0f);
      const float x = std::fmod(base - g.distance() * 0.12f * u, span);
      const float px = (x < 0.0f ? x + span : x) - 120.0f * scale;
      const float py = ground - (64.0f + static_cast<float>((k * 37) % 41)) * u * 0.9f;
      const ImU32 c = with_alpha(cloud, fade);
      dl->AddRectFilled(ImVec2(px, py), ImVec2(px + 22.0f * u, py + 3.0f * u), c, 1.5f * u);
      dl->AddRectFilled(ImVec2(px + 5.0f * u, py - 2.5f * u), ImVec2(px + 14.0f * u, py + 1.0f * u), c,
                        1.5f * u);
    }
  }

  // Obstacles: a trunk and two little arms.
  for (int i = 0; i < g.obstacle_count(); ++i) {
    const auto& o = g.obstacle_at(i);
    const float x0 = cx - (w * 0.5f) + o.x * u;
    dl->AddRectFilled(ImVec2(x0, ground - o.h * u), ImVec2(x0 + o.w * u, ground), ink, 1.5f * u);
    const float army = ground - o.h * 0.62f * u;
    dl->AddRectFilled(ImVec2(x0 - 3.0f * u, army), ImVec2(x0 + 1.0f * u, army + 2.0f * u), ink);
    dl->AddRectFilled(ImVec2(x0 - 3.0f * u, army - 4.0f * u), ImVec2(x0 - 1.0f * u, army + 2.0f * u), ink);
    dl->AddRectFilled(ImVec2(x0 + o.w * u - 1.0f * u, army - 1.0f * u),
                      ImVec2(x0 + o.w * u + 3.0f * u, army + 1.0f * u), ink);
    dl->AddRectFilled(ImVec2(x0 + o.w * u + 1.0f * u, army - 5.0f * u),
                      ImVec2(x0 + o.w * u + 3.0f * u, army + 1.0f * u), ink);
  }

  // The runner. In the intro it sprints in from the left edge and lands with a hop.
  float dx = g.dino_x();
  float dy = g.dino_y();
  const float left_edge = cx - w * 0.5f;
  if (intro) {
    const float q = ease_out((p - 0.35f) / 0.6f);
    dx = -dino_game::kDinoW - 6.0f + (g.dino_x() + dino_game::kDinoW + 6.0f) * q;
    if (q > 0.85f && q < 1.0f) dy = std::sin((q - 0.85f) / 0.15f * 3.14159f) * 5.0f;
  }
  const bool dead = g.state() == dino_game::phase::over;
  const bool legs_a = std::fmod(g.run_clock(), 2.0f) < 1.0f;
  const float sx = left_edge + dx * u;
  const float sy = ground - (dy + dino_game::kDinoH) * u;
  const ImU32 body = dead ? IM_COL32(200, 120, 120, 255) : ink;
  draw_rows(dl, kBody, 17, sx, sy, u, body);
  const bool frozen = dead || g.airborne();
  draw_rows(dl, (frozen || legs_a) ? kLegsA : kLegsB, 4, sx, sy + 17.0f * u, u, body);
  if (dead) {  // an X where the eye was
    const float ex = sx + 11.0f * u, ey = sy + 2.0f * u;
    dl->AddLine(ImVec2(ex - u, ey - u), ImVec2(ex + 1.6f * u, ey + 1.6f * u), IM_COL32(24, 26, 32, 255),
                1.5f * scale);
    dl->AddLine(ImVec2(ex + 1.6f * u, ey - u), ImVec2(ex - u, ey + 1.6f * u), IM_COL32(24, 26, 32, 255),
                1.5f * scale);
  }

  auto centred = [&](const char* s, float fs, float y, ImU32 col) {
    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s);
    dl->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, y), col, s);
  };

  // Score, top right of the canvas (below the command bar).
  if (g.state() != dino_game::phase::intro) {
    char buf[64];
    if (g.best() > 0) {
      std::snprintf(buf, sizeof buf, "HI %05d   %05d", g.best(), g.score());
    } else {
      std::snprintf(buf, sizeof buf, "%05d", g.score());
    }
    const float fs = 18.0f * scale;
    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
    dl->AddText(font, fs, ImVec2(w - sz.x - 28.0f * scale, chrome + 24.0f * scale), soft, buf);
  }

  if (g.state() == dino_game::phase::playing && g.seconds_in_phase() < 4.0f) {
    const float a = 1.0f - std::clamp((g.seconds_in_phase() - 2.5f) / 1.5f, 0.0f, 1.0f);
    centred("Space  jump        Esc  leave", 15.0f * scale, ground + 46.0f * scale, with_alpha(soft, a));
  }
  if (dead) {
    centred("G A M E   O V E R", 26.0f * scale, ground - 150.0f * scale, ink);
    centred("Space  run again        Esc  leave", 15.0f * scale, ground - 108.0f * scale, soft);
  }
}

}  // namespace mv::shell
