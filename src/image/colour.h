// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Display-referred colour: ICC → linear Rec.709 → sRGB encode. No tone map
// (D6, plan/03-rendering.md). Untagged JPEG/PNG/BMP is assumed sRGB.
#pragma once

#include <memory>
#include <span>

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

// An ICC → linear Rec.709 → sRGB transform built once and applied to many
// rasters with the same profile: every frame of a tagged animation (review
// note 34 — building a LittleCMS transform costs milliseconds). It owns its own
// cmsContext, so one instance must stay on one thread at a time. to_display
// builds a fresh one per call.
// Whether `icc` is sRGB in effect: an RGB matrix/shaper profile whose colorants
// and tone curves match sRGB within an 8-bit step. Such a file displays as-is
// on the v1 8-bit sRGB swapchain, so it skips LCMS (review note 43). A corrupt
// profile is not sRGB.
[[nodiscard]] bool is_srgb_icc(std::span<const std::uint8_t> icc) noexcept;

// ICC → sRGB for the v1 8-bit display path: one LCMS 8-bit to 8-bit transform
// with its precomputed LUT (colorimetrically ICC → linear → sRGB encode, no
// tone-map; D6's FP16 working space is the edit path). An sRGB-in-effect
// profile is a copy-through.
class display_transform {
 public:
  [[nodiscard]] static result<std::unique_ptr<display_transform>> create(
      std::span<const std::uint8_t> icc);
  ~display_transform();

  display_transform(const display_transform&) = delete;
  display_transform& operator=(const display_transform&) = delete;

  // Same output as to_display for a raster carrying this profile.
  [[nodiscard]] result<display_image> apply(codec::raster&& src,
                                            const job_context* ctx = nullptr) const;

 private:
  display_transform() = default;
  void* context_ = nullptr;    // cmsContext
  void* transform_ = nullptr;  // cmsHTRANSFORM; null when the profile is sRGB
  bool passthrough_ = false;   // the profile is sRGB in effect: copy through
};

}  // namespace mv::image
