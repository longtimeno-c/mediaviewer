// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/pipeline.h"

#include "codec/decode.h"
#include "codec/orient.h"

namespace mv::image {

result<display_image> decode_bytes(std::span<const std::uint8_t> bytes, const job_context* ctx,
                                    unsigned raw_thread_limit, std::uint32_t page) {
  auto raster = codec::decode(bytes, ctx, raw_thread_limit, page);
  if (!raster) return err(raster.error());
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const std::uint32_t at = raster->page;
  const std::uint32_t pages = raster->page_count;
  auto out = to_display(std::move(raster).value(), ctx);
  if (out) {
    out->page = at;
    out->page_count = pages;
  }
  return out;
}

result<display_image> decode_preview(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  const auto family = codec::probe(bytes);
  result<codec::raster> raster = err(status::unsupported_format);
  if (family == codec::format_family::jpeg) {
    raster = codec::decode_jpeg_display(bytes, ctx, 4);
  } else if (family == codec::format_family::raw || family == codec::format_family::tiff ||
             codec::looks_like_raw(bytes)) {
    // Embedded JPEG inside a RAW — first pixel in preview time (docs/design/04, PR 7).
    raster = codec::decode_raw_preview(bytes, ctx);
  } else if (family == codec::format_family::pdf) {
    // Page 1 at a third of its full size: first pixel and thumbnails
    // (docs/plans/audio-and-documents.md §2.4); the full render refines it.
    raster = codec::decode_pdf(bytes, 0, ctx, codec::kPdfPreviewEdge);
  } else {
    return err(status::unsupported_format);
  }
  if (!raster) return err(raster.error());
  if (raster->width < 16 || raster->height < 16) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

result<display_image> decode_first_pixel(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx) {
  if (codec::probe(bytes) != codec::format_family::heic) return decode_preview(bytes, ctx);
  auto raster = codec::decode_heic_thumbnail(bytes, ctx);
  if (!raster) return err(raster.error());
  if (raster->width < 16 || raster->height < 16) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

}  // namespace mv::image
