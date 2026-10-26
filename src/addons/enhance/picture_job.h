// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297 slice 5): one picture through a tiled upscaling model.
//
// The model takes a fixed square tile of RGB in [0, 1] (NCHW) and returns it
// `scale` times larger (Real-ESRGAN general x4v3: 4x). A fixed size lets
// Core ML compile the graph (it refuses unbounded dimensions). The frame is
// cut into tiles that overlap by `overlap` pixels on each side (edges
// replicated), so no seam shows; each tile's centre is averaged down to the
// size asked for (1x, 2x or 4x the original) as it is placed, so a 4x frame is
// never held whole. `strength` mixes the model's picture with a bilinear
// enlargement of the original (1 = the model alone).
//
// Portable and model-agnostic: tests drive it with a fake model.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/result.h"

namespace mv::enhance {

class tile_model {
 public:
  virtual ~tile_model() = default;
  [[nodiscard]] virtual std::uint32_t tile() const noexcept = 0;   // input side, pixels
  [[nodiscard]] virtual std::uint32_t scale() const noexcept = 0;  // output side = tile x scale
  // `in`: 3 x tile x tile, planar RGB in [0, 1]. `out`: 3 x (tile*scale)^2.
  [[nodiscard]] virtual expected run(std::span<const float> in, std::vector<float>& out) = 0;
};

struct picture_options {
  std::uint32_t out_scale = 2;  // 1, 2 or 4; at most the model's scale, dividing it
  std::uint32_t overlap = 16;   // pixels of context each side of a tile's centre
  double strength = 1.0;
  // A photo is hundreds of tiles: checked and reported between them.
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(double fraction)> progress;
};

// `rgb`: width x height x 3. `out` becomes (width*out_scale) x (height*out_scale) x 3.
[[nodiscard]] expected enhance_picture(std::span<const std::uint8_t> rgb, std::uint32_t width,
                                       std::uint32_t height, tile_model& model, const picture_options& o,
                                       std::vector<std::uint8_t>& out);

// Tiles one frame takes (for progress and time estimates).
[[nodiscard]] std::uint64_t tiles_for(std::uint32_t width, std::uint32_t height, std::uint32_t tile,
                                      std::uint32_t overlap) noexcept;

}  // namespace mv::enhance
