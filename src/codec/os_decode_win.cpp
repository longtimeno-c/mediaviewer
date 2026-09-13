// SPDX-License-Identifier: GPL-2.0-or-later
// Windows OS-codec probe (D3): WIC first, bundled decoder if the OS has no
// codec (clean VM, no Store pack). *_win.cpp is the D9 port (plan/15).
#include "codec/decode.h"

namespace mv::codec {

result<raster> try_os_decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  (void)bytes;
  return err(status::unsupported_format);
}

}  // namespace mv::codec
