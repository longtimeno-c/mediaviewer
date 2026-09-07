// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/pipeline.h"

#include "codec/decode.h"

namespace mv::image {

result<display_image> decode_bytes(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  auto raster = codec::decode(bytes, ctx);
  if (!raster) return err(raster.error());
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

result<display_image> decode_preview(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  auto raster = codec::decode_jpeg(bytes, ctx, 4);
  if (!raster) return err(raster.error());
  if (raster->width < 16 || raster->height < 16) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

}  // namespace mv::image
