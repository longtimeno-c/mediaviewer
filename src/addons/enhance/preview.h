// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's live preview (2026-10-28): while a clip or photo runs, the sheet
// shows the latest frame split down the middle, the original on the left and
// the enhanced picture on the right, inside a surface the host draws a slow
// glow round (contribution API 9). The runner hands over small copies about
// once a second; the engine letterboxes them to the surface's size.
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

}  // namespace mv::enhance
