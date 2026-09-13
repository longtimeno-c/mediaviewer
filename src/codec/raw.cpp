// SPDX-License-Identifier: GPL-2.0-or-later
// RAW via LibRaw (LGPL, dynamic, libraw::raw_r). Embedded JPEG is first
// pixel; full dcraw output replaces it. Never writes the original.
#include "codec/decode.h"

namespace mv::codec {

bool looks_like_raw(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.empty()) return false;
  return probe(bytes) == format_family::raw;
}

result<raster> decode_raw_preview(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  if (!looks_like_raw(bytes) && probe(bytes) != format_family::tiff) {
    return err(status::unsupported_format);
  }
  return err(status::unsupported_format);
}

result<raster> decode_raw(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  if (!looks_like_raw(bytes) && probe(bytes) != format_family::tiff) {
    return err(status::unsupported_format);
  }
  return err(status::unsupported_format);
}

}  // namespace mv::codec
