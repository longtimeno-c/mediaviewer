// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/picture_job.h"

#include <algorithm>
#include <cmath>

namespace mv::enhance {
namespace {

std::uint32_t core_of(std::uint32_t tile, std::uint32_t overlap) noexcept {
  return tile > 2 * overlap ? tile - 2 * overlap : 0;
}

}  // namespace

std::uint64_t tiles_for(std::uint32_t w, std::uint32_t h, std::uint32_t tile, std::uint32_t overlap) noexcept {
  const std::uint32_t core = core_of(tile, overlap);
  if (core == 0) return 0;
  return std::uint64_t{(w + core - 1) / core} * ((h + core - 1) / core);
}

expected enhance_picture(std::span<const std::uint8_t> rgb, std::uint32_t w, std::uint32_t h, tile_model& model,
                         const picture_options& o, std::vector<std::uint8_t>& out) {
  const std::uint32_t t = model.tile();
  const std::uint32_t ms = model.scale();
  const std::uint32_t s = o.out_scale;
  const std::uint32_t core = core_of(t, o.overlap);
  if (w == 0 || h == 0 || rgb.size() < std::size_t{w} * h * 3 || core == 0 || ms == 0 || s == 0 || s > ms ||
      ms % s != 0) {
    return err(status::invalid_arg);
  }
  const std::uint32_t k = ms / s;  // model pixels averaged into one output pixel, per side
  const std::uint32_t ow = w * s, oh = h * s;
  out.assign(std::size_t{ow} * oh * 3, 0);
  const float wet = static_cast<float>(std::clamp(o.strength, 0.0, 1.0));
  const float dry = 1.0f - wet;
  const std::size_t plane = std::size_t{t} * t;
  const std::uint32_t mt = t * ms;  // model output side
  const std::size_t mplane = std::size_t{mt} * mt;
  std::vector<float> in(plane * 3);
  std::vector<float> res;
  const auto px = [&](std::int64_t x, std::int64_t y, int c) {
    x = std::clamp<std::int64_t>(x, 0, w - 1);
    y = std::clamp<std::int64_t>(y, 0, h - 1);
    return rgb[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)];
  };
  // The original, enlarged bilinearly to the output grid (the dry side of the mix).
  const auto base = [&](std::uint32_t X, std::uint32_t Y, int c) -> float {
    if (s == 1) return px(X, Y, c) / 255.0f;
    const double fx = (X + 0.5) / s - 0.5, fy = (Y + 0.5) / s - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(fx)), y0 = static_cast<std::int64_t>(std::floor(fy));
    const double ax = fx - static_cast<double>(x0), ay = fy - static_cast<double>(y0);
    const double top = px(x0, y0, c) * (1 - ax) + px(x0 + 1, y0, c) * ax;
    const double bot = px(x0, y0 + 1, c) * (1 - ax) + px(x0 + 1, y0 + 1, c) * ax;
    return static_cast<float>((top * (1 - ay) + bot * ay) / 255.0);
  };
  const std::uint64_t total = tiles_for(w, h, t, o.overlap);
  std::uint64_t done = 0;
  for (std::uint32_t y0 = 0; y0 < h; y0 += core) {
    for (std::uint32_t x0 = 0; x0 < w; x0 += core) {
      if (o.cancel && o.cancel->load()) return err(status::cancelled);
      if (o.progress) o.progress(static_cast<double>(done++) / static_cast<double>(total));
      // The tile: this centre and `overlap` around it, edges replicated.
      const std::int64_t sx = static_cast<std::int64_t>(x0) - o.overlap;
      const std::int64_t sy = static_cast<std::int64_t>(y0) - o.overlap;
      for (std::uint32_t y = 0; y < t; ++y) {
        for (std::uint32_t x = 0; x < t; ++x) {
          for (int c = 0; c < 3; ++c) {
            in[static_cast<std::size_t>(c) * plane + std::size_t{y} * t + x] = px(sx + x, sy + y, c) / 255.0f;
          }
        }
      }
      MV_TRY_VOID(model.run(in, res));
      if (res.size() != mplane * 3) return err(status::corrupt);
      // Its centre, averaged k x k into the output.
      const std::uint32_t cw = std::min(core, w - x0), ch = std::min(core, h - y0);
      const std::uint32_t ox0 = x0 * s, oy0 = y0 * s;
      const std::uint32_t mx0 = o.overlap * ms, my0 = o.overlap * ms;
      const float inv = 1.0f / static_cast<float>(k * k);
      for (std::uint32_t Y = 0; Y < ch * s; ++Y) {
        for (std::uint32_t X = 0; X < cw * s; ++X) {
          for (int c = 0; c < 3; ++c) {
            const float* p = res.data() + static_cast<std::size_t>(c) * mplane;
            float sum = 0;
            for (std::uint32_t dy = 0; dy < k; ++dy) {
              const float* row = p + std::size_t{my0 + Y * k + dy} * mt + mx0 + X * k;
              for (std::uint32_t dx = 0; dx < k; ++dx) sum += row[dx];
            }
            float v = sum * inv;
            if (dry > 0) v = wet * v + dry * base(ox0 + X, oy0 + Y, c);
            out[(std::size_t{oy0 + Y} * ow + ox0 + X) * 3 + static_cast<std::size_t>(c)] =
                static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
          }
        }
      }
      if (o.placed) o.placed(oy0, oy0 + ch * s, ox0 + cw * s);
    }
  }
  return {};
}

}  // namespace mv::enhance
