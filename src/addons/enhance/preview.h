// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's live preview (2026-10-28): while a clip or photo runs, the sheet
// shows the latest frame split down the middle, the original on the left and
// the enhanced picture on the right, inside a surface the host draws a slow
// glow round (contribution API 10). The runner hands over small copies about
// once a second; the engine letterboxes them to the surface's size.
//
// A photo is one picture, so while it runs (2026-10-29) the sheet shows it
// being redrawn instead: the enhanced tiles replace the original as they are
// made, behind a glowing front, about four times a second; the split once
// it is done.
//
// Pure: no I/O, no host. The surface wants 8-bit BGRA, premultiplied, sRGB;
// every pixel here is opaque.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace mv::enhance {

struct preview_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;  // width x height x 3
  [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

// `rgb` (width x height) box-averaged down so its long edge is at most
// `max_edge`; a copy when it already fits.
[[nodiscard]] preview_image shrink(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
                                   std::uint32_t max_edge);

// The sheet's picture, out_w x out_h BGRA: `before` fitted and centred on a
// dark ground, with `after` (the same picture enhanced, any size) over its
// right half from `split` (0..1 of the picture's width) and a thin light
// divider. With no `after`, `before` alone; with no `before`, `after` alone.
[[nodiscard]] std::vector<std::uint8_t> compose_preview(const preview_image& before, const preview_image& after,
                                                        std::uint32_t out_w, std::uint32_t out_h, double split = 0.5);

// How far a photo's run has come, as fractions of the picture: every row
// above `y0` is enhanced, and the band from `y0` to `y1` left of `x`.
struct reveal_front {
  double y0 = 0, y1 = 0, x = 0;
};

// `out` (ow x oh RGB, being written) sampled at `before`'s size: the enhanced
// pixels where the run has been, `before`'s elsewhere. Nearest samples: it is
// made several times a second on the runner's thread.
[[nodiscard]] preview_image partial_of(const preview_image& before, std::span<const std::uint8_t> out,
                                       std::uint32_t ow, std::uint32_t oh, reveal_front at);

// The sheet's picture while a photo runs, out_w x out_h BGRA: `partial`
// fitted as compose_preview fits it, the part still to come a little dimmer,
// and a line in the glow's colours along the front, with a soft halo.
[[nodiscard]] std::vector<std::uint8_t> compose_reveal(const preview_image& partial, reveal_front at,
                                                       std::uint32_t out_w, std::uint32_t out_h);

}  // namespace mv::enhance
