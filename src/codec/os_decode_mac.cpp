// SPDX-License-Identifier: GPL-2.0-or-later
// Darwin twin of os_decode_win.cpp. The OS-codec hook (D3, policy in
// os_decode.h) offers HEIC stills to the platform codec first. On macOS that
// would be ImageIO, which is hardware-backed for HEVC; it is not wired yet, so
// this always declines and decode() falls through to the bundled libheif +
// libde265 path -- which is the "never require a Store codec pack, fall back
// silently" half of CLAUDE.md rule 7, and is what an iPhone HEIC needs on a
// clean Mac either way. Adding the ImageIO fast path later changes only this
// file.
#include <cstdlib>
#include <cstring>

#include "codec/decode.h"
#include "codec/os_decode.h"

namespace mv::codec {

bool os_codec_enabled() noexcept {
  const char* value = std::getenv("MV_OS_CODEC");
  return !(value && std::strcmp(value, "0") == 0);
}

result<raster> try_os_decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  (void)bytes;
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return err(status::unsupported_format);
}

}  // namespace mv::codec
