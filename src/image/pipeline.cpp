// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/pipeline.h"

#include "codec/decode.h"
#include "codec/orient.h"

namespace mv::image {

result<display_image> decode_bytes(std::span<const std::uint8_t> bytes, const job_context* ctx,
                                    unsigned raw_thread_limit) {
  auto raster = codec::decode(bytes, ctx, raw_thread_limit);
  if (!raster) return err(raster.error());
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

result<display_image> decode_preview(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  const auto family = codec::probe(bytes);
  result<codec::raster> raster = err(status::unsupported_format);
  if (family == codec::format_family::jpeg) {
    raster = codec::decode_jpeg_display(bytes, ctx, 4);
  } else if (family == codec::format_family::raw || family == codec::format_family::tiff ||
             codec::looks_like_raw(bytes)) {
    // Embedded JPEG inside a RAW — first pixel in preview time (plan/04, PR 7).
    raster = codec::decode_raw_preview(bytes, ctx);
  } else {
    return err(status::unsupported_format);
  }
  if (!raster) return err(raster.error());
  if (raster->width < 16 || raster->height < 16) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

}  // namespace mv::image
