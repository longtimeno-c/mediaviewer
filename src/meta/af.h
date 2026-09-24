// SPDX-License-Identifier: GPL-2.0-or-later
// AF-point geometry from maker-note arrays (plan/06 "Overlays that fall out of
// the read model"). Pure functions over already-parsed numbers so each
// vendor's layout is unit-tested without a camera file; still.cpp does the
// Exiv2 extraction and calls these.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "meta/meta.h"

namespace mv::meta {

// Canon AFInfo2 (Exif.Canon.AF*). Positions are the *centre* of each area,
// relative to the centre of the AF grid (image_w x image_h). Point i is in
// focus / selected when bit (i % 16) of mask short (i / 16) is set. Only
// points that are in focus or selected are returned.
//
// Sign of Y: taken as positive-down like the X axis is positive-right. That is
// how the layout reads in the ExifTool tag docs, but it has not been checked
// against a real Canon file in this repo (there is no corpus for it) — see the
// PR 9 entry in plan/12. A wrong sign mirrors the quads vertically about the
// image centre.
[[nodiscard]] std::vector<af_point> canon_af_points(
    std::uint32_t image_w, std::uint32_t image_h, std::span<const std::int64_t> widths,
    std::span<const std::int64_t> heights, std::span<const std::int64_t> xs,
    std::span<const std::int64_t> ys, std::span<const std::int64_t> in_focus_mask,
    std::span<const std::int64_t> selected_mask) noexcept;

// One area given as its centre in image pixels (top-left origin) on a grid of
// grid_w x grid_h: Nikon AFInfo2, Sony FocusLocation, Fujifilm FocusPoint and
// the standard EXIF SubjectArea all reduce to this. `w`/`h` of 0 use a default
// box (4 % of the grid width) because a point has no extent.
[[nodiscard]] std::vector<af_point> centre_af_point(double cx, double cy, double w, double h,
                                                    std::uint32_t grid_w,
                                                    std::uint32_t grid_h,
                                                    bool in_focus) noexcept;

}  // namespace mv::meta
