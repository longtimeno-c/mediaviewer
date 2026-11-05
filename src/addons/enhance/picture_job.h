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

// ---- A photo (docs/plans/edit-refresh.md S7, "Quality over speed") ----
//
// Enhance image's five models (spec.h kStillKinds) run here, never the
// clip's per-frame path above. Tiles of the model's fixed side overlap their
// neighbours by `overlap` input pixels; inside the photo, never past it (the
// last tile in a row or column ends at the photo's edge; only a photo smaller
// than a tile is padded, by reflection). Each tile's result is brought to the
// size asked for (an upscaler's 4x taken to 1x or 2x by a Lanczos-3 downscale
// of the model's own pixels, so "the same size" is a 4x restore averaged back
// with no aliasing), then blended into the picture by a tent weight: 1 in the
// tile's middle, falling linearly to 0 across the overlap toward a neighbour,
// and flat at the photo's own edges. Every output pixel is the weighted mean
// of the tiles over it, so tile edges (where a model sees least) never show.
// A tile whose result is not a picture is tried again mirrored and turned,
// then left as the photo (restore_options::unstable counts those).
// Rows are written as soon as no later tile reaches them: only a band of
// float sums one tile high is held, never the whole output in float.
struct restore_options {
  std::uint32_t out_scale = 1;  // 1, 2 or 4; at most the model's scale, dividing it
  std::uint32_t overlap = 48;   // input pixels neighbouring tiles share
  double strength = 1.0;        // 1: the model alone; 0: the photo, resized
  const std::atomic<bool>* cancel = nullptr;  // checked between tiles
  std::function<void(std::size_t done, std::size_t total)> progress;  // before each tile, then (total, total)
  // Set to the tiles left as the photo: the model's result was not a picture
  // (out of range or not finite) from the tile as it is, mirrored either way
  // or transposed.
  std::size_t* unstable = nullptr;
};

[[nodiscard]] expected restore_picture(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
                                       tile_model& model, const restore_options& o, std::vector<std::uint8_t>& out);

// Where a run of tiles starts along one side of `n` pixels: 0, then every
// tile - overlap, the last moved back to end at n (one tile at 0 when n <= tile).
[[nodiscard]] std::vector<std::uint32_t> tile_starts(std::uint32_t n, std::uint32_t tile, std::uint32_t overlap);
// Tiles a photo takes in restore_picture.
[[nodiscard]] std::uint64_t restore_tiles(std::uint32_t width, std::uint32_t height, std::uint32_t tile,
                                          std::uint32_t overlap);
// The tent along one side of a tile `side` long (output pixels): `ramp`
// pixels rising from the start unless `first` (the photo's edge), and falling
// to the end unless `last`. Never 0 inside the tile.
[[nodiscard]] std::vector<float> tent_weights(std::uint32_t side, std::uint32_t ramp, bool first, bool last);

}  // namespace mv::enhance
