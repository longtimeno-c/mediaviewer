// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Crops and scales for the strip and for an export. Scaling is in linear
// light (D6): a triangle filter widened to the reduction, so a tile and an
// export are both clean, and the weights are worked out once per size, which
// a clip's every frame reuses. Worker threads only.
#pragma once

#include <cstdint>
#include <vector>

#include "addons/instagram/post.h"

namespace mv::instagram {

struct image {
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  std::vector<std::uint8_t> rgb;  // tightly packed, 8-bit sRGB
};

class scaler {
 public:
  // From `from` (fractions of a sw x sh picture) to dw x dh.
  scaler(std::uint32_t sw, std::uint32_t sh, const rect& from, std::uint32_t dw, std::uint32_t dh);
  // src is sw x sh, packed; out has room for dw x dh x 3.
  void run(const std::uint8_t* src, std::uint8_t* out);

 private:
  struct tap {
    std::uint32_t first = 0;
    std::uint32_t begin = 0;  // into weights
    std::uint32_t count = 0;
  };
  static void build(std::uint32_t size, double from, double len, std::uint32_t out, std::vector<tap>& taps,
                    std::vector<float>& weights);
  std::uint32_t sw_, dw_, dh_, first_row_ = 0, rows_ = 0;
  std::vector<tap> xt_, yt_;
  std::vector<float> xw_, yw_, mid_;
};

// A picture turned clockwise by 90, 180 or 270 degrees (a clip's display
// rotation); any other value copies.
void turned(const std::uint8_t* src, std::uint32_t w, std::uint32_t h, std::uint32_t degrees, image& out);

// The crop of `pic` at w x h, packed RGB.
[[nodiscard]] std::vector<std::uint8_t> crop_rgb(const image& pic, const rect& crop, std::uint32_t w, std::uint32_t h);
// Opaque BGRA from packed RGB, with the ring below.
[[nodiscard]] std::vector<std::uint8_t> bgra_of(const std::vector<std::uint8_t>& rgb, std::uint32_t w, std::uint32_t h,
                                                std::uint32_t ring, std::uint32_t ring_px);

// A tile's BGRA, premultiplied (opaque), from the crop of `pic`. A focused
// tile carries a ring of `ring` (0xRRGGBBAA) `ring_px` thick inside its edge.
[[nodiscard]] std::vector<std::uint8_t> tile_bgra(const image& pic, const rect& crop, std::uint32_t w, std::uint32_t h,
                                                  std::uint32_t ring, std::uint32_t ring_px);

}  // namespace mv::instagram
