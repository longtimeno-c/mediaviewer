// SPDX-License-Identifier: GPL-2.0-or-later
// PR 11 — the edit working space (D6: linear FP16, non-negotiable; plan/07).
//
// A linear_image is linear-light Rec.709 / sRGB primaries, stored as IEEE
// half floats (image/half.h), RGBA with straight (not premultiplied) alpha.
// Both hosts upload it unchanged as an immutable FP16 texture, and the export
// bake reads the same words, which is what lets "export matches the preview"
// be a property of one data type rather than two code paths agreeing.
//
//   RAW  → codec::decode_raw_linear (LibRaw, 16-bit linear) → FP16. The real
//          linear data, never the embedded preview (plan/07).
//   else → the viewer's own display path (codec::decode + to_display: ICC →
//          sRGB) → sRGB decode → FP16. With every slider at zero the working
//          image is exactly what the viewer already shows.
//
// Worker threads only: every function here decodes or walks every pixel.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/format.h"
#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::image {

struct linear_image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  codec::format_family format = codec::format_family::unknown;
  // True when the pixels came from a RAW's linear develop (not an 8-bit
  // display image run back through the sRGB curve).
  bool from_raw = false;
  std::vector<std::uint16_t> rgba;  // FP16 bits, width * height * 4

  [[nodiscard]] bool valid() const noexcept {
    return width != 0 && height != 0 &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4;
  }
};

// An 8-bit display image (sRGB-encoded RGBA8, e.g. to_display's output) to
// linear FP16.
[[nodiscard]] result<linear_image> linear_from_srgb8(std::span<const std::uint8_t> rgba,
                                                     std::uint32_t width, std::uint32_t height,
                                                     codec::format_family format,
                                                     const job_context* ctx = nullptr);

// A RAW's 16-bit linear develop to FP16, converted in place (the two are the
// same size, and a 45 MP develop is 360 MB either way).
[[nodiscard]] result<linear_image> linear_from_raster16(codec::raster16&& src,
                                                        const job_context* ctx = nullptr);

// The full-resolution working image of a file (the table above). This is the
// seconds-long step for a RAW; the adjust pane stays disabled until it lands.
[[nodiscard]] result<linear_image> decode_linear(std::span<const std::uint8_t> bytes,
                                                 const job_context* ctx = nullptr);

// Box-filtered, alpha-weighted downscale so the long edge is at most
// `max_edge` (plan/07: evaluate the interactive preview at viewport
// resolution, ~3000 x 2000, and the full-resolution chain once, on export).
// Returns a copy when the image already fits.
[[nodiscard]] result<linear_image> downsample(const linear_image& src, std::uint32_t max_edge,
                                              const job_context* ctx = nullptr);

// The preview working texture's long-edge cap.
inline constexpr std::uint32_t kWorkingPreviewEdge = 3072;

}  // namespace mv::image
