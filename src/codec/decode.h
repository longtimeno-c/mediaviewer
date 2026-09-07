// SPDX-License-Identifier: GPL-2.0-or-later
// Decoder registry. Adding a format is one file plus one line in decode.cpp
// (plan/04-image-pipeline.md).
#pragma once

#include <span>

#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec {

// `ctx` may be null (tests). When present, cancelled() is checked between
// scanline blocks so a generation bump abandons a large decode.
[[nodiscard]] result<raster> decode(std::span<const std::uint8_t> bytes,
                                    const job_context* ctx = nullptr);

// `scale_denom` is libjpeg-turbo's DCT scale: 1, 2, 4, or 8. Other values
// decode at 1:1. Preview uploads use 4; the full decode is always 1.
[[nodiscard]] result<raster> decode_jpeg(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr,
                                         int scale_denom = 1);
[[nodiscard]] result<raster> decode_png(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_bmp(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);

// RGBA8 in, JPEG bytes out. `quality` is 1–100. Used for the filmstrip cache
// (plan/04 spec jpg512.1); not an export path.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_jpeg_rgba(
    std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, int quality);

}  // namespace mv::codec
