// SPDX-License-Identifier: GPL-2.0-or-later
// HEIC/HEIF via libheif + libde265 (LGPL, dynamic). No x265 — the vcpkg
// `hevc` feature is an encoder and is forbidden (plan/11, plan/12).
#include "codec/decode.h"

namespace mv::codec {

result<raster> decode_heic(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::heic) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  (void)bytes;
  return err(status::unsupported_format);
}

result<std::unique_ptr<animation_source>> open_heic_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::heic) return err(status::unsupported_format);
  return err(status::unsupported_format);
}

}  // namespace mv::codec
