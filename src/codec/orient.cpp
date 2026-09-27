// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/orient.h"

#include <cstring>
#include <new>
#include <vector>

#include "codec/decode.h"
#include "codec/exif.h"

namespace mv::codec {

bool apply_orientation(raster& img, d4 g) {
  if (g.identity() || img.width == 0 || img.height == 0) return true;
  const bool t = g.transposes();
  const bool fx = g.flip_x();
  const bool fy = g.flip_y();
  const std::uint32_t iw = img.width;
  const std::uint32_t ow = t ? img.height : img.width;
  const std::uint32_t oh = t ? img.width : img.height;

  std::vector<std::uint8_t> out;
  try {
    out.resize(static_cast<std::size_t>(ow) * oh * 4);
  } catch (const std::bad_alloc&) {
    return false;
  }

  const auto* src = reinterpret_cast<const std::uint32_t*>(img.rgba.data());
  auto* dst = reinterpret_cast<std::uint32_t*>(out.data());
  if (!t) {
    for (std::uint32_t y = 0; y < oh; ++y) {
      const std::uint32_t sy = fy ? oh - 1 - y : y;
      const std::uint32_t* srow = src + static_cast<std::size_t>(sy) * iw;
      std::uint32_t* drow = dst + static_cast<std::size_t>(y) * ow;
      if (!fx) {
        std::memcpy(drow, srow, static_cast<std::size_t>(ow) * 4);
      } else {
        for (std::uint32_t x = 0; x < ow; ++x) drow[x] = srow[ow - 1 - x];
      }
    }
  } else {
    // Transposing walks the source by column; 32-pixel blocks keep both
    // sides in cache on a 24 MP frame.
    constexpr std::uint32_t kBlock = 32;
    for (std::uint32_t by = 0; by < oh; by += kBlock) {
      for (std::uint32_t bx = 0; bx < ow; bx += kBlock) {
        const std::uint32_t ey = by + kBlock < oh ? by + kBlock : oh;
        const std::uint32_t ex = bx + kBlock < ow ? bx + kBlock : ow;
        for (std::uint32_t y = by; y < ey; ++y) {
          const std::uint32_t py = fy ? oh - 1 - y : y;  // stored x
          std::uint32_t* drow = dst + static_cast<std::size_t>(y) * ow;
          for (std::uint32_t x = bx; x < ex; ++x) {
            const std::uint32_t px = fx ? ow - 1 - x : x;  // stored y
            drow[x] = src[static_cast<std::size_t>(px) * iw + py];
          }
        }
      }
    }
  }
  img.rgba = std::move(out);
  img.width = ow;
  img.height = oh;
  return true;
}

result<raster> decode_jpeg_display(std::span<const std::uint8_t> bytes, const job_context* ctx,
                                   int scale_denom) {
  auto decoded = decode_jpeg(bytes, ctx, scale_denom);
  if (!decoded) return decoded;
  const int orientation = jpeg_orientation(bytes);
  if (orientation > 1 && !apply_orientation(decoded.value(), from_exif(orientation))) {
    return err(status::out_of_memory);
  }
  return decoded;
}

}  // namespace mv::codec
