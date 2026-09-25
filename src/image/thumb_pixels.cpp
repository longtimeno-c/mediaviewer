// SPDX-License-Identifier: GPL-2.0-or-later
// The pixel half of the JPEG-512 thumbnail (image/thumb.h make_thumb_rgba),
// in its own translation unit: the Explorer thumbnail handler (shellext/)
// links this and nothing of the SQLite cache in thumb.cpp.
#include <algorithm>

#include "codec/decode.h"
#include "codec/format.h"
#include "image/pipeline.h"
#include "image/thumb.h"

namespace mv::image {
namespace {

void box_fit_rgba(const display_image& src, std::uint32_t dst_w, std::uint32_t dst_h,
                  std::vector<std::uint8_t>& dst) {
  dst.assign(static_cast<std::size_t>(dst_w) * dst_h * 4u, 0);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    const std::uint32_t y0 = y * src.height / dst_h;
    const std::uint32_t y1 = ((y + 1) * src.height + dst_h - 1) / dst_h;
    const std::uint32_t yb = y1 > y0 ? y1 : y0 + 1;
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const std::uint32_t x0 = x * src.width / dst_w;
      const std::uint32_t x1 = ((x + 1) * src.width + dst_w - 1) / dst_w;
      const std::uint32_t xb = x1 > x0 ? x1 : x0 + 1;
      std::uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
      for (std::uint32_t sy = y0; sy < yb && sy < src.height; ++sy) {
        const std::uint8_t* row = src.rgba.data() + static_cast<std::size_t>(sy) * src.width * 4u;
        for (std::uint32_t sx = x0; sx < xb && sx < src.width; ++sx) {
          r += row[sx * 4u + 0];
          g += row[sx * 4u + 1];
          b += row[sx * 4u + 2];
          a += row[sx * 4u + 3];
          ++n;
        }
      }
      if (n == 0) n = 1;
      std::uint8_t* p = dst.data() + (static_cast<std::size_t>(y) * dst_w + x) * 4u;
      p[0] = static_cast<std::uint8_t>(r / n);
      p[1] = static_cast<std::uint8_t>(g / n);
      p[2] = static_cast<std::uint8_t>(b / n);
      p[3] = static_cast<std::uint8_t>(a / n);
    }
  }
}

}  // namespace

result<thumb_pixels> make_thumb_rgba(std::span<const std::uint8_t> src_bytes,
                                     std::uint32_t max_long_edge, const job_context* ctx) {
  if (max_long_edge == 0) return err(status::invalid_arg);
  result<display_image> decoded = err(status::unsupported_format);
  if (codec::probe(src_bytes) == codec::format_family::jpeg) {
    // Pick the coarsest DCT scale that still leaves at least max_long_edge to
    // downsample from. The old fixed {8, 4, 1} ladder took the first scale that
    // decoded, so a 1024px JPEG produced a 128px "512" thumb.
    int denom = 1;
    if (auto size = codec::jpeg_dimensions(src_bytes)) {
      const std::uint32_t edge = size.value().width > size.value().height ? size.value().width
                                                                          : size.value().height;
      for (int candidate : {8, 4, 2}) {
        if (edge / static_cast<std::uint32_t>(candidate) >= max_long_edge) {
          denom = candidate;
          break;
        }
      }
    }
    for (int attempt : {denom, 1}) {
      auto raster = codec::decode_jpeg_display(src_bytes, ctx, attempt);
      if (!raster) {
        if (raster.error() == status::cancelled) return err(status::cancelled);
        continue;
      }
      decoded = to_display(std::move(raster).value(), ctx);
      if (decoded) break;
      if (decoded.error() == status::cancelled) return err(status::cancelled);
    }
  }
  if (!decoded) {
    decoded = decode_bytes(src_bytes, ctx);
    if (!decoded) return err(decoded.error());
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  display_image& img = decoded.value();
  if (img.width == 0 || img.height == 0) return err(status::corrupt);
  const std::uint32_t long_edge = img.width > img.height ? img.width : img.height;
  thumb_pixels out;
  if (long_edge > max_long_edge) {
    out.width = std::max<std::uint32_t>(1, img.width * max_long_edge / long_edge);
    out.height = std::max<std::uint32_t>(1, img.height * max_long_edge / long_edge);
    box_fit_rgba(img, out.width, out.height, out.rgba);
  } else {
    out.width = img.width;
    out.height = img.height;
    out.rgba = std::move(img.rgba);
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return out;
}

}  // namespace mv::image
