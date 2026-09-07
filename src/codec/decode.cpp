// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"

namespace mv::codec {

result<raster> decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  switch (probe(bytes)) {
    case format_family::jpeg: return decode_jpeg(bytes, ctx);
    case format_family::png:  return decode_png(bytes, ctx);
    case format_family::bmp:  return decode_bmp(bytes, ctx);
    case format_family::unknown:
      return err(status::unsupported_format);
  }
  return err(status::unsupported_format);
}

}  // namespace mv::codec
