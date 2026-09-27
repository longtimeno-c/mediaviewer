// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder registry. Adding a format is one file plus one line in decode.cpp
// (plan/04-image-pipeline.md).
#pragma once

#include <memory>
#include <span>
#include <vector>

#include "codec/anim.h"
#include "codec/raster.h"
#include "core/job_system.h"
#include "core/result.h"

namespace mv::codec {

// `ctx` may be null (tests). When present, cancelled() is checked between
// scanline blocks so a generation bump abandons a large decode.
[[nodiscard]] result<raster> decode(std::span<const std::uint8_t> bytes,
                                    const job_context* ctx = nullptr,
                                    unsigned raw_thread_limit = 4);

// `scale_denom` is libjpeg-turbo's DCT scale: 1, 2, 4, or 8. Other values
// decode at 1:1. Preview uploads use 4; the full decode is always 1.
[[nodiscard]] result<raster> decode_jpeg(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr,
                                         int scale_denom = 1);

struct jpeg_size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Reads the JPEG header only — no pixels, no scanlines. Lets a caller choose
// `scale_denom` from the real dimensions instead of guessing.
[[nodiscard]] result<jpeg_size> jpeg_dimensions(std::span<const std::uint8_t> bytes);
[[nodiscard]] result<raster> decode_png(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_bmp(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);
// GIF / WebP as a still: frame 0 only, composited on its canvas (PR 6). Later
// frames are not decoded, so this is also the animation's first pixel (rule 3).
[[nodiscard]] result<raster> decode_gif(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_webp(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_tiff(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_ico(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_heic(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr);
[[nodiscard]] result<raster> decode_avif(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx = nullptr);
// How many LibRaw threads the image on screen may use. Leaves processors for
// the present loop; prefetch passes 1 instead. Output pixels do not depend on it.
[[nodiscard]] unsigned raw_foreground_threads() noexcept;
// Upper bound a benchmark may request. One decode never exceeds this, so a
// RAW job cannot occupy every logical processor the present thread needs.
[[nodiscard]] unsigned raw_thread_ceiling() noexcept;

[[nodiscard]] result<raster> decode_raw(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx = nullptr,
                                        unsigned thread_limit = 4);

// PR 11: the same develop as decode_raw, at 16 bits with a linear curve — the
// adjust pane's working data and the export bake's source (plan/07: the
// pane waits for this, never for the embedded preview). Same size and
// orientation as decode_raw.
[[nodiscard]] result<raster16> decode_raw_linear(std::span<const std::uint8_t> bytes,
                                                 const job_context* ctx = nullptr);

// Embedded JPEG/preview inside a RAW, if any. `unsupported_format` when the
// file is not RAW or has no usable preview. First pixel for CR2/NEF/ARW
// (plan/04). Does not write the original bytes.
[[nodiscard]] result<raster> decode_raw_preview(std::span<const std::uint8_t> bytes,
                                                const job_context* ctx = nullptr);

// True when LibRaw (or a non-TIFF RAW magic) says this buffer is a camera RAW.
// TIFF-container RAWs probe as `tiff`; decode() uses this to reclassify.
[[nodiscard]] bool looks_like_raw(std::span<const std::uint8_t> bytes) noexcept;

// OS codec (WIC on Windows) tried first (D3). `unsupported_format` means fall
// through to the bundled decoder. Implementation is codec/os_decode_win.cpp.
[[nodiscard]] result<raster> try_os_decode(std::span<const std::uint8_t> bytes,
                                           const job_context* ctx = nullptr);

// Frame-at-a-time animation (plan/04). The bytes are shared, not copied, and
// kept alive by the source. `unsupported_format` when the file is known to be
// a still (a one-frame WebP, a PNG without acTL); a one-frame GIF can only be
// told apart by asking for a second frame.
[[nodiscard]] result<std::unique_ptr<animation_source>> open_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);
[[nodiscard]] result<std::unique_ptr<animation_source>> open_gif_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);
[[nodiscard]] result<std::unique_ptr<animation_source>> open_webp_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);
[[nodiscard]] result<std::unique_ptr<animation_source>> open_apng_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);
[[nodiscard]] result<std::unique_ptr<animation_source>> open_heic_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);
[[nodiscard]] result<std::unique_ptr<animation_source>> open_avif_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes);

// Tests and tools only: every frame of one play, within `max_bytes` of RGBA.
// `unsupported_format` for a still or an animation over the budget. Playback
// never calls this — it would be the full decode before the first frame.
[[nodiscard]] result<animation_frames> decode_animation(
    std::span<const std::uint8_t> bytes, const job_context* ctx = nullptr,
    std::size_t max_bytes = kAnimationByteBudget);

// RGBA8 in, JPEG bytes out. `quality` is 1–100. Used for the filmstrip cache
// (plan/04 spec jpg512.2); not an export path.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_jpeg_rgba(
    std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, int quality);

}  // namespace mv::codec
