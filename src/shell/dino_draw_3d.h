// SPDX-License-Identifier: GPL-2.0-or-later
// Small perspective scene shared by the D3D11 and Metal hosts. World-space
// boxes are lit, projected and depth-sorted into the existing ImGui renderer;
// no extra device, assets, shaders, allocations or present path are needed.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "imgui.h"
#include "shell/dino_game.h"

namespace mv::shell::dino_3d {

struct point { float x, y, z; };
struct face {
  ImVec2 corners[4];
  float depth;
  ImU32 colour;
};

class scene {
 public:
  scene(float width, float height, float chrome, float scale, float alpha) noexcept
      : cx_(width * 0.23f), cy_(chrome + (height - chrome) * 0.68f),
        unit_(std::min({5.0f * scale, width / 230.0f, (height - chrome) / 110.0f})),
        alpha_(alpha) {}

  ImVec2 project(point p) const noexcept {
    p.x -= 38.0f;  // frame the runner, looking down the track from its left side
    const float perspective = 220.0f / depth(p);
    return {cx_ + (0.94f * p.x + 0.342f * p.z) * unit_ * perspective,
            cy_ - (0.117f * p.x + 0.94f * p.y - 0.321f * p.z) * unit_ * perspective};
  }

  void quad(point a, point b, point c, point d, ImU32 colour, float light = 1.0f) noexcept {
    if (count_ == faces_.size()) return;  // fixed scene budget, even on a very wide window
    const point points[] = {a, b, c, d};
    auto& f = faces_[count_++];
    f.depth = 0.0f;
    for (int i = 0; i < 4; ++i) {
      f.corners[i] = project(points[i]);
      point p = points[i];
      p.x -= 38.0f;
      f.depth += depth(p) * 0.25f;
    }
    // ImGui's anti-aliased convex fill expects clockwise screen winding.
    const float cross = (f.corners[1].x - f.corners[0].x) * (f.corners[2].y - f.corners[0].y) -
                        (f.corners[1].y - f.corners[0].y) * (f.corners[2].x - f.corners[0].x);
    if (cross < 0.0f) std::swap(f.corners[1], f.corners[3]);
    const auto channel = [colour, light](int shift) {
      return static_cast<int>(static_cast<float>((colour >> shift) & 255u) * light);
    };
    f.colour = IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT),
                       channel(IM_COL32_B_SHIFT),
                       static_cast<int>(static_cast<float>((colour >> IM_COL32_A_SHIFT) & 255u) * alpha_));
  }

  void box(float x, float y, float z, float w, float h, float d, ImU32 colour) noexcept {
    const point a{x, y, z}, c{x + w, y + h, z}, e{x, y + h, z};
    const point f{x, y, z + d}, g{x + w, y, z + d}, i{x + w, y + h, z + d}, j{x, y + h, z + d};
    // Camera is above, behind the runner and on the positive-Z side.
    // All boxes remain on the far side of the camera, so these are the three
    // visible faces throughout the bounded track and jump arc.
    quad(a, f, j, e, colour, 0.65f);
    quad(f, g, i, j, colour, 0.85f);
    quad(e, j, i, c, colour);
  }

  void draw(ImDrawList* dl) noexcept {
    std::sort(faces_.begin(), faces_.begin() + static_cast<std::ptrdiff_t>(count_),
              [](const face& a, const face& b) { return a.depth > b.depth; });
    for (std::size_t i = 0; i < count_; ++i) {
      const auto& f = faces_[i];
      dl->AddConvexPolyFilled(f.corners, 4, f.colour);
    }
  }

 private:
  static float depth(point p) noexcept {
    return std::max(35.0f, 220.0f + 0.321f * p.x - 0.342f * p.y - 0.883f * p.z);
  }
  float cx_, cy_, unit_, alpha_;
  std::array<face, 768> faces_{};
  std::size_t count_ = 0;
};

inline void draw(ImDrawList* dl, const dino_game& g, float w, float h, float chrome, float scale) noexcept {
  const float fade = std::clamp((g.intro_progress() - 0.25f) / 0.5f, 0.0f, 1.0f);
  scene world(w, h, chrome, scale, fade);
  dl->PushClipRect(ImVec2(0.0f, chrome), ImVec2(w, h), true);

  // A long, solid track with a visible near edge, not a flat sprite backdrop.
  const ImU32 sand = IM_COL32(75, 88, 104, 255);
  world.box(-75.0f, -4.0f, -17.0f, 1900.0f, 4.0f, 34.0f, sand);
  world.quad({-75, 0.08f, -16}, {1825, 0.08f, -16}, {1825, 0.08f, -15}, {-75, 0.08f, -15},
             IM_COL32(153, 176, 192, 255));
  world.quad({-75, 0.08f, 15}, {1825, 0.08f, 15}, {1825, 0.08f, 16}, {-75, 0.08f, 16},
             IM_COL32(153, 176, 192, 255));

  const float scroll = std::fmod(g.distance(), 24.0f);
  for (int i = 0; i < 55; ++i) {
    const float x = static_cast<float>(i) * 24.0f - scroll - 55.0f;
    // Ties at the far shoulder make speed and perspective easy to read.
    world.quad({x, 0.1f, -13}, {x + 7, 0.1f, -13}, {x + 7, 0.1f, -12}, {x, 0.1f, -12},
               IM_COL32(113, 133, 151, 255));
  }

  // Contact shadows stay on the road as the dinosaur jumps above them.
  const auto shadow = [&world](float x, float width, float z, float depth) {
    world.quad({x, 0.15f, z}, {x + width, 0.15f, z},
               {x + width + 3, 0.15f, z + depth}, {x + 3, 0.15f, z + depth},
               IM_COL32(30, 37, 46, 255));
  };
  for (int n = 0; n < g.obstacle_count(); ++n) {
    const auto& o = g.obstacle_at(n);
    shadow(o.x - 1, o.w + 3, -3, 12);
    const ImU32 cactus = IM_COL32(118, 179, 146, 255);
    world.box(o.x, 0, -3, o.w, o.h, 6, cactus);
    world.box(o.x - 3, o.h * 0.52f, -2, 3, 2, 4, cactus);
    world.box(o.x - 3, o.h * 0.52f, -2, 2, 6, 4, cactus);
    world.box(o.x + o.w, o.h * 0.65f, -2, 3, 2, 4, cactus);
    world.box(o.x + o.w + 1, o.h * 0.65f, -2, 2, 5, 4, cactus);
  }

  const bool dead = g.state() == dino_game::phase::over;
  const bool frozen = dead || g.airborne();
  const float stride = frozen ? 0.0f : std::sin(g.run_clock() * 3.14159f);
  float x = g.dino_x();
  const float y = g.dino_y();
  if (g.state() == dino_game::phase::intro) x -= (1.0f - fade) * 50.0f;
  shadow(x + 2, 19, -4, 12);
  const ImU32 body = dead ? IM_COL32(222, 130, 132, 255) : IM_COL32(225, 232, 241, 255);
  const ImU32 belly = dead ? IM_COL32(185, 96, 109, 255) : IM_COL32(157, 188, 204, 255);
  // Chunky T-Rex silhouette: tapered tail, haunches, neck, big snout and tiny arms.
  world.box(x - 3, y + 8, -2, 8, 3, 4, body);
  world.box(x - 6, y + 10, -1, 4, 2, 2, body);
  world.box(x + 3, y + 5, -4, 10, 9, 8, body);
  world.box(x + 10, y + 10, -3, 5, 8, 6, body);
  world.box(x + 10, y + 15, -4, 10, 6, 8, body);
  world.box(x + 14, y + 13, -3, 6, 2, 6, belly);
  world.box(x + 10, y + 8, 4, 5, 2, 2, belly);
  world.box(x + 13, y + 7, 4, 2, 2, 2, body);
  world.box(x + 6 + stride * 1.6f, y + std::max(0.0f, stride), -3.5f, 3, 6, 2.5f, belly);
  world.box(x + 6 - stride * 1.6f, y + std::max(0.0f, -stride), 1.5f, 3, 6, 2.5f, body);
  world.box(x + 6 + stride * 1.6f, y + std::max(0.0f, stride), -3.5f, 5, 1.5f, 2.5f, belly);
  world.box(x + 6 - stride * 1.6f, y + std::max(0.0f, -stride), 1.5f, 5, 1.5f, 2.5f, body);
  // The eye is actual geometry on the visible side of the head.
  world.box(x + 13, y + 18, 4, 1.5f, 1.5f, 0.2f, IM_COL32(25, 32, 43, 255));
  world.draw(dl);
  dl->PopClipRect();
}

}  // namespace mv::shell::dino_3d
