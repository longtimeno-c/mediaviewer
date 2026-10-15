// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in theme's ground effect on the empty window (issue #408), behind
// the welcome card. Both present labs draw it with plain ImGui draw-list calls
// (D9). It is the chromes' pane field (ThemeField.swift / ThemeField.cs)
// ported mark for mark: the same counts, speeds and sizes, in points, every
// position a function of the clock. Its seed is its own (the effect and its
// ink), since a mark on the home never lines up with a pane's anyway.
//
// It is drawn only on the home: never on a photo, a video, an error card or
// the lab sweep, and once a file is up the present loop never sees it. It
// does not change the clear. Motion rests by the panes' rule (docs/design/25
// §8): ten ticks a second, and only while the app is in front, Reduce Motion
// (Mac) / Windows animations allow it, and the pointer or a key moved in the
// last five seconds. Each tick is one static present; nothing here makes the
// loop live, so a still stays at 0 presents.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "imgui.h"
#include "shell/home_ink.h"
#include "shell/home_theme.h"  // home_ground_clock: the rest rule

namespace mv::shell {

namespace detail {

struct ground_rng {
  std::uint64_t state;
  [[nodiscard]] double next() noexcept {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / static_cast<double>(1ull << 53);
  }
};

[[nodiscard]] inline double ground_wrap(double value, double span) noexcept {
  if (span <= 1.0) return 0.0;
  const double m = std::fmod(value, span);
  return m < 0.0 ? m + span : m;
}

[[nodiscard]] inline int ground_count(double area, double per, int cap, int min) noexcept {
  if (area < 800.0) return 0;
  return std::min(cap, std::max(min, static_cast<int>(area / per)));
}

[[nodiscard]] inline double ground_noise(double x, double y) noexcept {
  const double n = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
  return n - std::floor(n);
}

[[nodiscard]] inline double ground_smooth(double x, double y) noexcept {
  const double ix = std::floor(x);
  const double iy = std::floor(y);
  const double fx = x - ix;
  const double fy = y - iy;
  const double a = ground_noise(ix, iy);
  const double b = ground_noise(ix + 1, iy);
  const double c = ground_noise(ix, iy + 1);
  const double d = ground_noise(ix + 1, iy + 1);
  const double ux = fx * fx * (3 - 2 * fx);
  const double uy = fy * fy * (3 - 2 * fy);
  return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

}  // namespace detail

// Draws the effect over the whole `w` x `h` pixel canvas (the clear is already
// the theme's canvas). `scale` is the DPI scale: the field is laid out in
// points, as the panes are. `time` is home_ground_clock::time.
inline void draw_home_ground(ImDrawList* dl, float w, float h, float scale, const home_theme& theme,
                             double time) noexcept {
  using detail::ground_count;
  using detail::ground_rng;
  using detail::ground_wrap;
  if (dl == nullptr || scale <= 0.0f || theme.effect == home_effect::none ||
      theme.effect >= home_effect::count)
    return;
  const double pw = static_cast<double>(w) / scale;
  const double ph = static_cast<double>(h) / scale;
  if (pw < 8.0 || ph < 8.0) return;
  const double area = pw * ph;
  const std::uint32_t ink = home_opaque(theme.effect_rgb);
  const double gain = theme.intensity;
  const double pace = theme.speed;
  const double size = theme.size;
  const auto s = static_cast<double>(scale);
  // FNV over the effect, then the ink: a seed per look, stable across runs.
  std::uint64_t base = 0x6a09e667f3bcc909ull;
  base = (base ^ static_cast<std::uint64_t>(theme.effect)) * 0x100000001b3ull;
  base = (base ^ static_cast<std::uint64_t>(theme.effect_rgb)) * 0x100000001b3ull;
  const auto rng = [base](int i) {
    return ground_rng{base ^ (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull)};
  };
  // Marks are pre-mixed over the canvas in sRGB (home_over), as the panes
  // blend, and drawn opaque.
  const std::uint32_t ground = theme.canvas | 0xFFu;
  const auto col = [ink, ground](double alpha) {
    return home_col(home_over(ink, ground, static_cast<float>(alpha)));
  };
  const auto pt = [s](double x, double y) {
    return ImVec2(static_cast<float>(x * s), static_cast<float>(y * s));
  };
  const auto rect = [&](double x, double y, double rw, double rh, double alpha) {
    dl->AddRectFilled(pt(x, y), pt(x + rw, y + rh), col(alpha));
  };
  const auto dot = [&](double x, double y, double r, double alpha) {
    dl->AddCircleFilled(pt(x, y), static_cast<float>(r * s), col(alpha), 12);
  };
  constexpr double kPi = 3.14159265358979323846;

  switch (theme.effect) {
    case home_effect::dots: {
      const double spacing = std::max(28.0, std::sqrt(area / 72.0));
      const double alpha = 0.16 * gain;
      for (double y = spacing * 0.5; y < ph; y += spacing)
        for (double x = spacing * 0.5; x < pw; x += spacing) dot(x, y, 1.2 * size, alpha);
      break;
    }
    case home_effect::rain: {
      const int n = ground_count(area, 9000, 64, 8);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double len = (14 + r.next() * 18) * size;
        const double speed = (32 + r.next() * 56) * pace;
        const double alpha = (0.14 + r.next() * 0.14) * gain;
        const double x = r.next() * pw;
        const double y0 = r.next() * (ph + len);
        const double y = ground_wrap(y0 + time * speed, ph + len) - len;
        rect(x, y, 1.0, len, alpha);
      }
      break;
    }
    case home_effect::starfield: {
      const int n = ground_count(area, 7000, 90, 10);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x = r.next() * pw;
        const double y0 = r.next() * ph;
        const double z = 0.18 + r.next() * 0.82;
        const double speed = (12 + z * 48) * pace;
        dot(x, ground_wrap(y0 + time * speed, ph), (0.45 + z * 1.15) * size, (0.10 + z * 0.38) * gain);
      }
      break;
    }
    case home_effect::petals: {
      const int n = ground_count(area, 26000, 18, 6);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x0 = r.next() * pw;
        const double y0 = r.next() * (ph + 16);
        const double petal = (3 + r.next() * 4) * size;
        const double vy = (16 + r.next() * 22) * pace;
        const double amp = (10 + r.next() * 16) * size;
        const double spin = (-0.5 + r.next()) * pace;
        const double phase = r.next() * kPi * 2;
        const double x = x0 + std::sin(phase + time * 0.7 * pace) * amp;
        const double y = ground_wrap(y0 + time * vy, ph + 16) - 8;
        const double rot = phase + time * spin;
        const double rx = petal * 0.7;
        const double ry = petal * 0.32;
        const double off = petal * 0.15;
        const double cs = std::cos(rot);
        const double sn = std::sin(rot);
        const ImVec2 radius(static_cast<float>(rx * s), static_cast<float>(ry * s));
        dl->AddEllipseFilled(pt(x - off * cs, y - off * sn), radius, col(0.20 * gain),
                             static_cast<float>(rot), 14);
        dl->AddEllipseFilled(pt(x + off * cs, y + off * sn), radius, col(0.13 * gain),
                             static_cast<float>(rot), 14);
      }
      break;
    }
    case home_effect::fireflies: {
      const int n = ground_count(area, 15000, 32, 7);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x0 = r.next() * pw;
        const double y0 = r.next() * ph;
        const double depth = r.next();
        const double phase = r.next() * kPi * 2;
        const double freq = (0.6 + depth * 0.9) * pace;
        const double x = ground_wrap(x0 + std::sin(time * freq + phase) * (18 + depth * 36) * size, pw);
        const double y =
            ground_wrap(y0 + std::cos(time * freq * 0.72 + phase) * (10 + depth * 22) * size, ph);
        const double pulse = std::max(0.0, std::sin(time * (1.1 + depth) * pace + phase));
        const double alpha = pulse * (0.16 + depth * 0.34) * gain;
        if (alpha > 0.02) dot(x, y, (1.1 + depth * 1.5) * size, alpha);
      }
      break;
    }
    case home_effect::synapse: {
      const double spacing = std::max(24.0, (pw + ph) / 120.0);
      const double line = 0.035 * gain;
      for (double y = 0; y < ph; y += spacing) rect(0, y, pw, 1, line);
      for (double x = 0; x < pw; x += spacing) rect(x, 0, 1, ph, line);
      const int n = ground_count(area, 28000, 14, 4);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const bool vertical = r.next() >= 0.5;
        const double speed = (36 + r.next() * 74) * pace;
        const double trail = 14 * size;
        const double head = 1.3 * size;
        const double phase = r.next();
        const int lanes = std::max(1, static_cast<int>((vertical ? pw : ph) / spacing));
        const double lane = static_cast<double>(static_cast<int>(r.next() * lanes)) * spacing;
        const double span = (vertical ? ph : pw) + trail;
        const double pos = ground_wrap(phase * span + time * speed, span) - trail;
        if (vertical) {
          rect(lane, pos, 1, trail, 0.30 * gain);
          dot(lane + 0.5, pos + trail, head, 0.55 * gain);
        } else {
          rect(pos, lane, trail, 1, 0.30 * gain);
          dot(pos + trail, lane + 0.5, head, 0.55 * gain);
        }
      }
      break;
    }
    case home_effect::embers: {
      const int n = ground_count(area, 14000, 36, 8);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x0 = r.next() * pw;
        const double y0 = r.next() * (ph + 24);
        const double phase = r.next() * kPi * 2;
        const double vy = (18 + r.next() * 36) * pace;
        const double amp = (6 + r.next() * 14) * size;
        const double radius = (1.1 + r.next() * 1.6) * size;
        const double rate = (0.18 + r.next() * 0.28) * pace;
        const double life = ground_wrap(time * rate + phase / (kPi * 2), 1);
        const double fade = std::sin(life * kPi);
        if (fade * gain <= 0.02) continue;
        const double x = ground_wrap(x0 + std::sin(phase + time * 0.8 * pace) * amp, pw);
        const double y = ground_wrap(y0 - time * vy, ph + 24);
        dot(x, y, radius * 3.2, 0.14 * fade * gain);
        dot(x, y, radius * 0.65, 0.62 * fade * gain);
      }
      break;
    }
    case home_effect::sparkles: {
      const int n = ground_count(area, 16000, 28, 6);
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x = r.next() * pw;
        const double y = r.next() * ph;
        const double radius = (2.4 + r.next() * 3.4) * size;
        const double phase = r.next() * kPi * 2;
        const double freq = (0.9 + r.next() * 1.8) * pace;
        const double twinkle = std::max(0.0, std::sin(time * freq + phase));
        const double alpha = twinkle * 0.42 * gain;
        if (alpha <= 0.02) continue;
        // A four-point star: quadratic curves pinched toward the centre.
        const double rr = radius * (0.45 + twinkle * 0.55);
        const double k = rr * 0.15;
        dl->PathLineTo(pt(x, y - rr));
        dl->PathBezierQuadraticCurveTo(pt(x + k, y - k), pt(x + rr, y), 6);
        dl->PathBezierQuadraticCurveTo(pt(x + k, y + k), pt(x, y + rr), 6);
        dl->PathBezierQuadraticCurveTo(pt(x - k, y + k), pt(x - rr, y), 6);
        dl->PathBezierQuadraticCurveTo(pt(x - k, y - k), pt(x, y - rr), 6);
        dl->PathFillConcave(col(alpha));
      }
      break;
    }
    case home_effect::perlin_flow: {
      using detail::ground_smooth;
      const int n = ground_count(area, 18000, 28, 6);
      constexpr double kLags[3] = {0.0, 0.16, 0.32};
      constexpr double kAlphas[3] = {0.22, 0.11, 0.05};
      constexpr double kRadii[3] = {1.15, 0.85, 0.60};
      for (int i = 0; i < n; ++i) {
        ground_rng r = rng(i);
        const double x0 = r.next() * pw;
        const double y0 = r.next() * ph;
        const double phase = r.next() * kPi * 2;
        const double freq = (0.12 + ground_smooth(x0 * 0.01, y0 * 0.01) * 0.22) * pace;
        const double amp = (26 + ground_smooth(x0 * 0.008 + 3, y0 * 0.008) * 46) * size;
        for (int k = 0; k < 3; ++k) {
          const double t = time - kLags[k];
          const double angle = ground_smooth(x0 * 0.004 + t * 0.05 * pace, y0 * 0.004) * kPi * 6;
          const double x =
              ground_wrap(x0 + std::cos(angle) * amp * 0.35 + std::sin(t * freq + phase) * amp, pw);
          const double y = ground_wrap(
              y0 + std::sin(angle) * amp * 0.35 + std::cos(t * freq * 0.72 + phase) * amp * 0.62, ph);
          dot(x, y, kRadii[k] * size, kAlphas[k] * gain);
        }
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace mv::shell
