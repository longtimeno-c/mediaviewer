// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's Remove (docs/plans/edit-adjust.md slice 7, owner 2026-11-03): what
// the person brushed over, filled in from its surroundings by an inpainting
// model (LaMa, a fixed square input) on a crop of the full-resolution photo.
//
//   1. remove_box: the mask's bounds, grown for context and made square where
//      the photo allows.
//   2. The crop and its mask scaled to the model's side, filled, scaled back.
//   3. paste_back: only the brushed pixels (feathered a few pixels outward)
//      replaced; everything outside the box is the photo's own.
//
// The model is not here: these are the steps around it, so they are tested
// without it. The runner's thread only (rule 1).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "addons/enhance/diffusion.h"

namespace mv::enhance {

// 8-bit, the photo's size; over 127 is brushed.
struct mask8 {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> a;  // width * height
};

struct pixel_box {
  std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Context on each side: half the brushed area's longer side, at least
// kRemoveMinContext. nullopt when nothing is brushed.
inline constexpr std::uint32_t kRemoveMinContext = 128;
[[nodiscard]] std::optional<pixel_box> remove_box(const mask8& mask);

[[nodiscard]] rgb8 crop_rgb(const rgb8& src, const pixel_box& box);
// The mask's crop scaled to width x height, nearest, then 0 or 255.
[[nodiscard]] mask8 crop_mask(const mask8& src, const pixel_box& box, std::uint32_t width, std::uint32_t height);

// `filled` (the box's size) over `photo` where the mask is brushed, blended
// over `feather` pixels outward. Pixels farther than 2 * feather from a
// brushed one are left exactly as they were.
void paste_back(rgb8& photo, const pixel_box& box, const rgb8& filled, const mask8& mask, std::uint32_t feather);

}  // namespace mv::enhance
