// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/audio_card.h"

#include <algorithm>
#include <cmath>

namespace mv::player {
namespace {

constexpr std::uint8_t kBackground[3] = {30, 30, 32};
constexpr std::uint8_t kGlyph[3] = {200, 200, 206};

// Signed distances in card units (the card's height is 1.0): negative inside.
float ellipse(float x, float y, float cx, float cy, float rx, float ry, float angle) noexcept {
  const float c = std::cos(angle), s = std::sin(angle);
  const float dx = x - cx, dy = y - cy;
  const float u = (dx * c + dy * s) / rx, v = (-dx * s + dy * c) / ry;
  // Scaled by the smaller radius: close enough to a true distance for a 1 px edge.
  return (std::sqrt(u * u + v * v) - 1.0f) * std::min(rx, ry);
}

float box(float x, float y, float x0, float y0, float x1, float y1, float r = 0.0f) noexcept {
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  const float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
  const float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// A ring of radius r and half-thickness t, upper half only (a padlock shackle).
float upper_ring(float x, float y, float cx, float cy, float r, float t) noexcept {
  if (y > cy) {
    // Below the centre the shackle continues as two straight legs.
    const float leg = std::min(std::fabs(x - (cx - r)), std::fabs(x - (cx + r)));
    return leg - t;
  }
  const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
  return std::fabs(d - r) - t;
}

// The flag of an eighth note: a thick quarter curve off the top of the stem.
float flag(float x, float y, float sx, float sy, float size, float t) noexcept {
  float best = 1e9f;
  for (int i = 0; i <= 24; ++i) {
    const float p = static_cast<float>(i) / 24.0f;
    // A quadratic from the stem top, out and down.
    const float bx = sx + size * (0.9f * p * (2.0f - p) * 0.55f);
    const float by = sy + size * (p * 0.95f);
    best = std::min(best, std::sqrt((x - bx) * (x - bx) + (y - by) * (y - by)));
  }
  return best - t * (1.0f - 0.4f * std::clamp((y - sy) / size, 0.0f, 1.0f));
}

float music(float x, float y) noexcept {
  const float head = ellipse(x, y, -0.035f, 0.13f, 0.085f, 0.06f, -0.35f);
  const float stem = box(x, y, 0.028f, -0.22f, 0.046f, 0.12f);
  const float fl = flag(x, y, 0.037f, -0.22f, 0.2f, 0.022f);
  return std::min({head, stem, fl});
}

float padlock(float x, float y) noexcept {
  const float body = box(x, y, -0.15f, -0.02f, 0.15f, 0.22f, 0.025f);
  const float shackle = upper_ring(x, y, 0.0f, -0.06f, 0.095f, 0.022f);
  // The shackle's legs stop where the body begins.
  const float shackle_cut = y > -0.02f ? 1e9f : shackle;
  const float keyhole = std::min(ellipse(x, y, 0.0f, 0.075f, 0.03f, 0.03f, 0.0f),
                                 box(x, y, -0.011f, 0.08f, 0.011f, 0.15f));
  return std::min(std::max(body, -keyhole), shackle_cut);
}

}  // namespace

audio_card make_audio_card(audio_card_kind kind, std::uint32_t width, std::uint32_t height) {
  audio_card out;
  out.width = std::max<std::uint32_t>(2, width & ~1u);
  out.height = std::max<std::uint32_t>(2, height & ~1u);
  out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);

  const float unit = static_cast<float>(out.height);
  const float px = 1.0f / unit;  // one pixel in card units: the anti-aliasing ramp
  const float half_w = static_cast<float>(out.width) * 0.5f;
  const float half_h = static_cast<float>(out.height) * 0.5f;
  for (std::uint32_t row = 0; row < out.height; ++row) {
    std::uint8_t* p = out.rgba.data() + static_cast<std::size_t>(row) * out.width * 4u;
    const float y = (static_cast<float>(row) + 0.5f - half_h) / unit;
    for (std::uint32_t col = 0; col < out.width; ++col, p += 4) {
      const float x = (static_cast<float>(col) + 0.5f - half_w) / unit;
      float cover = 0.0f;
      if (std::fabs(x) < 0.3f && std::fabs(y) < 0.3f) {
        const float d = kind == audio_card_kind::music ? music(x, y) : padlock(x, y);
        cover = std::clamp(0.5f - d / px, 0.0f, 1.0f);
      }
      for (int c = 0; c < 3; ++c) {
        p[c] = static_cast<std::uint8_t>(std::lround(kBackground[c] +
                                                     (kGlyph[c] - kBackground[c]) * cover));
      }
      p[3] = 255;
    }
  }
  return out;
}

}  // namespace mv::player
