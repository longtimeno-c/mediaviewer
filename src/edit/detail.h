// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 4: Detail — definition, brilliance, sharpen
// and noise reduction. They need a pixel's neighbours, so unlike the rest of
// the Adjust list they are not a term in the per-pixel kernel: they are one
// CPU pass over the linear working image, before the kernel.
//
//   * The canvas: the host runs apply_detail on a worker over the preview
//     working image (kWorkingPreviewEdge) when a detail slider settles, and
//     uploads the result as the working texture the kernel reads. While it
//     runs the canvas shows the last finished pass.
//   * Export: the same pass at full resolution before the bake. Radii are
//     fractions of the long edge (or scaled from the preview's), so the
//     export looks like the preview at any size.
//
// On perceptual lightness L = luma^(1/2.2), applied back to RGB as a ratio
// (hue kept), in this order:
//   noise      L toward an edge-preserving (range-weighted) neighbourhood
//              mean, and colour toward its wide box mean: noise / 100 of
//              the way
//   definition (L − base) × midtone weight added: local contrast, base a
//              wide blur of L (1 % of the long edge)
//   brilliance lifts what sits in a dark neighbourhood and holds back what
//              sits in a bright one, plus a little local contrast
//   sharpen    (L − a small blur) added, each step capped so edges do not
//              ring (an unsharp mask)
// Worker only (rule 1).
#pragma once

#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "image/linear.h"

namespace mv::edit {

struct detail_params {
  float definition = 0.0f;  // −1 .. +1
  float brilliance = 0.0f;  // −1 .. +1
  float sharpen = 0.0f;     // 0 .. 1
  float noise = 0.0f;       // 0 .. 1
  [[nodiscard]] bool identity() const noexcept {
    return definition == 0.0f && brilliance == 0.0f && sharpen == 0.0f && noise == 0.0f;
  }
  friend constexpr bool operator==(const detail_params&, const detail_params&) = default;
};

[[nodiscard]] detail_params detail_of(const colour& c) noexcept;

// `src` with the detail pass applied (a new image of the same size).
// Identity params return a copy.
[[nodiscard]] result<image::linear_image> apply_detail(const image::linear_image& src,
                                                       const detail_params& d,
                                                       const job_context* ctx = nullptr);

}  // namespace mv::edit
