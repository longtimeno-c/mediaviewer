// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/preview.h"

#include <algorithm>
#include <cmath>

namespace mv::enhance {
namespace {

// The ground round a letterboxed picture: near-black, as a viewer's canvas.
constexpr std::uint8_t kGround[3] = {27, 27, 31};

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
  std::vector<std::uint8_t> out(std::size_t{ow} * oh * 4);
  for (std::size_t i = 0; i < std::size_t{ow} * oh; ++i) {
    out[i * 4] = kGround[2];  // BGRA
    out[i * 4 + 1] = kGround[1];
    out[i * 4 + 2] = kGround[0];
    out[i * 4 + 3] = 255;
  }
  const preview_image& frame = before.empty() ? after : before;
  if (frame.empty() || ow == 0 || oh == 0) return out;
  // The picture's place: fitted, centred.
  const double f = std::min(static_cast<double>(ow) / frame.width, static_cast<double>(oh) / frame.height);
  const auto pw = std::max(1u, static_cast<std::uint32_t>(std::lround(frame.width * f)));
  const auto ph = std::max(1u, static_cast<std::uint32_t>(std::lround(frame.height * f)));
  const std::uint32_t px = (ow - std::min(ow, pw)) / 2, py = (oh - std::min(oh, ph)) / 2;
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

}  // namespace mv::enhance
