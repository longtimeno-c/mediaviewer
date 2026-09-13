// SPDX-License-Identifier: GPL-2.0-or-later
// TIFF via libtiff. Multi-page: frame 0 is the still; pages are
// Ctrl+PageUp/PageDown (plan/04), not extra filmstrip stops.
#include "codec/decode.h"

namespace mv::codec {

result<raster> decode_tiff(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::tiff) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  (void)bytes;
  return err(status::unsupported_format);
}

}  // namespace mv::codec
