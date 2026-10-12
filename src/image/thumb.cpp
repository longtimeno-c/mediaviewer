// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/thumb.h"

#include "codec/decode.h"
#include "codec/orient.h"
#include "codec/format.h"
#include "image/pipeline.h"

namespace mv::image {

result<std::vector<std::uint8_t>> encode_thumb_rgba(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return err(status::invalid_arg);
  const std::size_t need = static_cast<std::size_t>(width) * height * 4u;
  if (rgba.size() < need) return err(status::invalid_arg);
  return codec::encode_jpeg_rgba(rgba.subspan(0, need), width, height, kThumbJpegQuality);
}

result<std::vector<std::uint8_t>> make_thumb_jpeg(std::span<const std::uint8_t> src_bytes,
                                                  const job_context* ctx) {
  MV_TRY(thumb_pixels t, make_thumb_rgba(src_bytes, kThumbLongEdge, ctx));
  composite_thumb_matte(t);
  return codec::encode_jpeg_rgba(t.rgba, t.width, t.height, kThumbJpegQuality);
}

}  // namespace mv::image
