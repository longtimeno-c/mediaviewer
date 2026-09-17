// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/pipeline_mac.h"

#include "codec/decode.h"

namespace mv::image {

result<display_image> decode_bytes_mac(std::span<const std::uint8_t> bytes,
                                       const job_context* ctx) {
  const auto family = codec::probe(bytes);
  result<codec::raster> raster = err(status::unsupported_format);
  switch (family) {
    case codec::format_family::jpeg: raster = codec::decode_jpeg(bytes, ctx); break;
    case codec::format_family::png:  raster = codec::decode_png(bytes, ctx); break;
    case codec::format_family::bmp:  raster = codec::decode_bmp(bytes, ctx); break;
    default: return err(status::unsupported_format);
  }
  if (!raster) return err(raster.error());
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return to_display(std::move(raster).value(), ctx);
}

}  // namespace mv::image
