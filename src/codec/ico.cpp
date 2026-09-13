// SPDX-License-Identifier: GPL-2.0-or-later
// ICO: ICONDIR of BMP (no BITMAPFILEHEADER) or PNG images. Largest size is
// the still; other sizes are Ctrl+PageUp/PageDown inside one folder stop.
#include "codec/decode.h"

namespace mv::codec {

result<raster> decode_ico(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::ico) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  (void)bytes;
  return err(status::unsupported_format);
}

}  // namespace mv::codec
