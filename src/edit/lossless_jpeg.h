// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Lossless JPEG rotate / flip / MCU-aligned crop (plan/07 "Export",
// plan/10 PR 10): the DCT coefficients are rearranged, never decoded to
// pixels, so there is no generation loss. jpegtran's transform, on the plain
// libjpeg coefficient API the decoder already links (no transupp, no
// TurboJPEG transform API, so it builds on the vcpkg and system ports alike).
//
// Only *perfect* transforms are done: an edge that would move a partial MCU
// into the image refuses (`unsupported_format`) rather than trim pixels, and
// the caller falls back to re-encoding or to writing the Orientation tag.
// Camera frames (6000x4000, 4032x3024 …) are MCU-aligned, so the fallback is
// for odd-sized files.
//
// The output is always upright: the file's own EXIF orientation is baked
// into the transform and the tag is written as 1 (EXIF and XMP), the pixel
// dimensions are patched, and a stale EXIF thumbnail is unlinked.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/orientation.h"
#include "core/result.h"
#include "edit/metadata_policy.h"

namespace mv::edit {

struct jpeg_layout {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mcu_w = 8;  // iMCU size in pixels (16x16 for 4:2:0)
  std::uint32_t mcu_h = 8;
  int components = 0;
  bool progressive = false;
  int orientation = 1;  // EXIF, 1 when absent
};

[[nodiscard]] result<jpeg_layout> read_layout(std::span<const std::uint8_t> jpeg);

struct lossless_request {
  // Stored pixels → output: the file's orientation composed with the user's
  // rotate/flip (placement::total with base = from_exif(orientation)).
  codec::d4 transform{};
  // Crop in output pixels, after the transform. Top-left must sit on the
  // output's iMCU grid; width/height are free. w == 0 means no crop.
  std::uint32_t crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
  metadata_policy policy = metadata_policy::all;
};

// Can `req` be done without touching a pixel?
[[nodiscard]] bool lossless_possible(const jpeg_layout& layout, const lossless_request& req) noexcept;

// Runs the transform. `unsupported_format` when !lossless_possible; `corrupt`
// for a JPEG libjpeg refuses.
[[nodiscard]] result<std::vector<std::uint8_t>> transform(std::span<const std::uint8_t> jpeg,
                                                          const lossless_request& req);

// The viewer's `[` `]` `H` `V` (plan/16): rotate/flip what is displayed by
// `op`, losslessly, returning the new file's bytes. Tries the coefficient
// transform (baking the old orientation, tag → 1); when the frame is not
// MCU-aligned, falls back to rewriting only the Orientation tag (in place in
// EXIF, or a new minimal EXIF APP1 when the file had none) — the pixels are
// not touched either way. `used_tag` reports which one ran.
[[nodiscard]] result<std::vector<std::uint8_t>> rotate_in_viewer(std::span<const std::uint8_t> jpeg,
                                                                 codec::d4 op, bool* used_tag = nullptr);

}  // namespace mv::edit
