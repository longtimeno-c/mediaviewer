// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/preview.h"

#include <algorithm>
#include <cmath>

namespace mv::enhance {
namespace {

// The ground round a letterboxed picture: near-black, as a viewer's canvas.
constexpr std::uint8_t kGround[3] = {27, 27, 31};

// The glow's colours (ScreenView.swift GlowPalette, IslandHost.Glow.cs
// GlowColors): violet, pink, amber, teal, blue.
constexpr std::uint8_t kGlow[5][3] = {{140, 92, 245}, {250, 92, 158}, {255, 158, 64}, {64, 199, 217}, {92, 133, 255}};

// The glow's colour at `t` (0..1) along it.
void glow_at(double t, double out[3]) {
  const double f = std::clamp(t, 0.0, 1.0) * 4;
  const int i = std::min(3, static_cast<int>(f));
  const double a = f - i;
  for (int c = 0; c < 3; ++c) out[c] = kGlow[i][c] * (1 - a) + kGlow[i + 1][c] * a;
}

struct fit {
  std::uint32_t x = 0, y = 0, w = 0, h = 0;
};
// A picture of fw x fh fitted and centred in ow x oh.
fit fitted(std::uint32_t fw, std::uint32_t fh, std::uint32_t ow, std::uint32_t oh) {
  const double f = std::min(static_cast<double>(ow) / fw, static_cast<double>(oh) / fh);
  fit r;
  r.w = std::max(1u, static_cast<std::uint32_t>(std::lround(fw * f)));
  r.h = std::max(1u, static_cast<std::uint32_t>(std::lround(fh * f)));
  r.x = (ow - std::min(ow, r.w)) / 2;
  r.y = (oh - std::min(oh, r.h)) / 2;
  return r;
}

std::vector<std::uint8_t> ground(std::uint32_t ow, std::uint32_t oh) {
  std::vector<std::uint8_t> out(std::size_t{ow} * oh * 4);
  for (std::size_t i = 0; i < std::size_t{ow} * oh; ++i) {
    out[i * 4] = kGround[2];  // BGRA
    out[i * 4 + 1] = kGround[1];
    out[i * 4 + 2] = kGround[0];
    out[i * 4 + 3] = 255;
  }
  return out;
}

// Whether the run has reached the picture-relative point (u, v).
bool reached(reveal_front at, double u, double v) { return v < at.y0 || (v < at.y1 && u < at.x); }

}  // namespace

preview_image shrink(std::span<const std::uint8_t> rgb, std::uint32_t w, std::uint32_t h, std::uint32_t max_edge) {
  preview_image out;
  if (w == 0 || h == 0 || max_edge == 0 || rgb.size() < std::size_t{w} * h * 3) return out;
  const double f = std::min(1.0, static_cast<double>(max_edge) / std::max(w, h));
  out.width = std::max(1u, static_cast<std::uint32_t>(std::lround(w * f)));
  out.height = std::max(1u, static_cast<std::uint32_t>(std::lround(h * f)));
  out.rgb.resize(std::size_t{out.width} * out.height * 3);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::uint64_t{y} * h / out.height);
    const std::uint32_t y1 = std::max(y0 + 1, static_cast<std::uint32_t>(std::uint64_t{y + 1} * h / out.height));
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t x0 = static_cast<std::uint32_t>(std::uint64_t{x} * w / out.width);
      const std::uint32_t x1 = std::max(x0 + 1, static_cast<std::uint32_t>(std::uint64_t{x + 1} * w / out.width));
      std::uint32_t sum[3] = {0, 0, 0};
      for (std::uint32_t sy = y0; sy < y1; ++sy) {
        const std::uint8_t* row = rgb.data() + (std::size_t{sy} * w + x0) * 3;
        for (std::uint32_t sx = x0; sx < x1; ++sx, row += 3) {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
        }
      }
      const std::uint32_t n = (y1 - y0) * (x1 - x0);
      std::uint8_t* o = out.rgb.data() + (std::size_t{y} * out.width + x) * 3;
      for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
    }
  }
  return out;
}

std::vector<std::uint8_t> compose_preview(const preview_image& before, const preview_image& after,
                                          std::uint32_t ow, std::uint32_t oh, double split) {
  std::vector<std::uint8_t> out = ground(ow, oh);
  const preview_image& frame = before.empty() ? after : before;
  if (frame.empty() || ow == 0 || oh == 0) return out;
  // The picture's place: fitted, centred.
  const fit r = fitted(frame.width, frame.height, ow, oh);
  const std::uint32_t pw = r.w, ph = r.h, px = r.x, py = r.y;
  const bool both = !before.empty() && !after.empty();
  const auto cut = static_cast<std::uint32_t>(std::lround(std::clamp(split, 0.0, 1.0) * pw));
  // Nearest sample of `img` at a picture-relative point (0..1).
  const auto sample = [](const preview_image& img, double u, double v, std::uint8_t out_rgb[3]) {
    const auto x = std::min(img.width - 1, static_cast<std::uint32_t>(u * img.width));
    const auto y = std::min(img.height - 1, static_cast<std::uint32_t>(v * img.height));
    const std::uint8_t* p = img.rgb.data() + (std::size_t{y} * img.width + x) * 3;
    out_rgb[0] = p[0];
    out_rgb[1] = p[1];
    out_rgb[2] = p[2];
  };
  for (std::uint32_t y = 0; y < ph && py + y < oh; ++y) {
    const double v = (y + 0.5) / ph;
    for (std::uint32_t x = 0; x < pw && px + x < ow; ++x) {
      const double u = (x + 0.5) / pw;
      std::uint8_t c[3];
      sample(both && x >= cut ? after : frame, u, v, c);
      std::uint8_t* o = out.data() + (std::size_t{py + y} * ow + px + x) * 4;
      o[0] = c[2];
      o[1] = c[1];
      o[2] = c[0];
    }
  }
  if (both && cut > 0 && cut < pw) {
    // A thin light divider, a little translucent over the picture.
    const std::uint32_t lw = std::max(1u, ow / 400);
    for (std::uint32_t y = 0; y < ph && py + y < oh; ++y) {
      for (std::uint32_t d = 0; d < lw && px + cut + d < ow; ++d) {
        std::uint8_t* o = out.data() + (std::size_t{py + y} * ow + px + cut + d) * 4;
        for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>((o[c] + 3 * 255) / 4);
      }
    }
  }
  return out;
}

preview_image partial_of(const preview_image& before, std::span<const std::uint8_t> out, std::uint32_t ow,
                         std::uint32_t oh, reveal_front at) {
  preview_image p = before;
  if (before.empty() || ow == 0 || oh == 0 || out.size() < std::size_t{ow} * oh * 3) return p;
  for (std::uint32_t y = 0; y < p.height; ++y) {
    const double v = (y + 0.5) / p.height;
    const auto sy = std::min(oh - 1, static_cast<std::uint32_t>(v * oh));
    for (std::uint32_t x = 0; x < p.width; ++x) {
      const double u = (x + 0.5) / p.width;
      if (!reached(at, u, v)) continue;
      const auto sx = std::min(ow - 1, static_cast<std::uint32_t>(u * ow));
      const std::uint8_t* s = out.data() + (std::size_t{sy} * ow + sx) * 3;
      std::uint8_t* d = p.rgb.data() + (std::size_t{y} * p.width + x) * 3;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  }
  return p;
}

std::vector<std::uint8_t> compose_reveal(const preview_image& partial, reveal_front at, std::uint32_t ow,
                                         std::uint32_t oh) {
  std::vector<std::uint8_t> out = ground(ow, oh);
  if (partial.empty() || ow == 0 || oh == 0) return out;
  const fit r = fitted(partial.width, partial.height, ow, oh);
  // The front, in output pixels: the band's lower edge left of x, its upper
  // edge right of it (a step while a row of tiles is under way).
  const double halo = std::max(2.0, oh / 40.0);
  const double fx = at.x * r.w, fy0 = at.y0 * r.h, fy1 = at.y1 * r.h;
  const bool going = at.y0 < 1;
  for (std::uint32_t y = 0; y < r.h && r.y + y < oh; ++y) {
    const double v = (y + 0.5) / r.h;
    const auto sy = std::min(partial.height - 1, static_cast<std::uint32_t>(v * partial.height));
    for (std::uint32_t x = 0; x < r.w && r.x + x < ow; ++x) {
      const double u = (x + 0.5) / r.w;
      const auto sx = std::min(partial.width - 1, static_cast<std::uint32_t>(u * partial.width));
      const std::uint8_t* s = partial.rgb.data() + (std::size_t{sy} * partial.width + sx) * 3;
      double c[3] = {static_cast<double>(s[0]), static_cast<double>(s[1]), static_cast<double>(s[2])};
      if (going) {
        // Still to come: a little dimmer, so the new part reads as new.
        if (!reached(at, u, v)) {
          for (double& k : c) k *= 0.72;
        }
        // Distance to the front, in output pixels: its horizontal edge here,
        // and the step's vertical edge within the band.
        const double edge_y = x + 0.5 < fx ? fy1 : fy0;
        double d = std::abs(y + 0.5 - edge_y);
        if (y + 0.5 >= fy0 && y + 0.5 <= fy1) d = std::min(d, std::abs(x + 0.5 - fx));
        if (d < halo * 3) {
          double g[3];
          glow_at(u, g);
          const double a = d < 1.0 ? 0.95 : 0.55 * std::exp(-d / halo);
          for (int k = 0; k < 3; ++k) c[k] = c[k] * (1 - a) + g[k] * a;
        }
      }
      std::uint8_t* o = out.data() + (std::size_t{r.y + y} * ow + r.x + x) * 4;
      o[0] = static_cast<std::uint8_t>(std::lround(std::clamp(c[2], 0.0, 255.0)));
      o[1] = static_cast<std::uint8_t>(std::lround(std::clamp(c[1], 0.0, 255.0)));
      o[2] = static_cast<std::uint8_t>(std::lround(std::clamp(c[0], 0.0, 255.0)));
    }
  }
  return out;
}

}  // namespace mv::enhance
