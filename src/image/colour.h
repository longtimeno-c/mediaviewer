// SPDX-License-Identifier: GPL-2.0-or-later
// Display-referred colour: ICC → linear Rec.709 → sRGB encode. No tone map
// (D6, plan/03-rendering.md). Untagged JPEG/PNG/BMP is assumed sRGB.
#pragma once

#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::image {

struct display_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  codec::format_family format = codec::format_family::unknown;
  codec::transfer_intent intent = codec::transfer_intent::display_referred;
  bool icc_tagged = false;
  // Packed RGBA8, 8-bit sRGB. Row stride = width * 4.
  std::vector<std::uint8_t> rgba;
};

// Converts a source-encoded raster into display-referred sRGB 8-bit.
// Display-referred sources are never tone-mapped. Untagged files are copied
// through (already sRGB). Tagged ICC profiles go through LCMS into linear
// Rec.709, then the sRGB OETF. A broken profile is `corrupt`, not untagged.
[[nodiscard]] result<display_image> to_display(codec::raster&& src,
                                               const job_context* ctx = nullptr);

}  // namespace mv::image
