// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"

#include <cstring>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

std::uint16_t u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int32_t i32(const std::uint8_t* p) noexcept { return static_cast<std::int32_t>(u32(p)); }

}  // namespace

result<raster> decode_bmp(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::bmp) return err(status::unsupported_format);
  if (bytes.size() < 14 + 16) return err(status::corrupt);

  const std::uint8_t* p = bytes.data();
  const std::uint32_t pixel_off = u32(p + 10);
  const std::uint32_t dib = u32(p + 14);
  if (dib < 16 || 14 + dib > bytes.size()) return err(status::corrupt);

  const std::uint32_t width = u32(p + 18);
  const std::int32_t height_s = i32(p + 22);
  const std::uint16_t planes = u16(p + 26);
  const std::uint16_t bpp = u16(p + 28);
  const std::uint32_t compression = dib >= 40 ? u32(p + 30) : 0;

  if (width == 0 || height_s == 0 || planes != 1) return err(status::corrupt);
  if (compression != 0) return err(status::unsupported_format);  // BI_RGB only
  if (bpp != 24 && bpp != 32) return err(status::unsupported_format);

  const bool top_down = height_s < 0;
  const auto height = static_cast<std::uint32_t>(height_s < 0 ? -height_s : height_s);
  if (width > kMaxDim || height > kMaxDim ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    return err(status::unsupported_format);
  }

  const std::uint32_t bytes_pp = bpp / 8;
  const std::uint32_t row_unpadded = width * bytes_pp;
  const std::uint32_t row_stride = (row_unpadded + 3u) & ~3u;
  const std::uint64_t need = static_cast<std::uint64_t>(pixel_off) +
                             static_cast<std::uint64_t>(row_stride) * height;
  if (need > bytes.size() || pixel_off >= bytes.size()) return err(status::corrupt);

  raster out;
  out.format = format_family::bmp;
  out.intent = transfer_intent::display_referred;
  out.width = width;
  out.height = height;
  try {
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  for (std::uint32_t y = 0; y < height; ++y) {
    if (ctx && ctx->cancelled() && (y & 63u) == 0) return err(status::cancelled);
    const std::uint32_t src_y = top_down ? y : (height - 1 - y);
    const std::uint8_t* src = p + pixel_off + static_cast<std::size_t>(src_y) * row_stride;
    std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
    for (std::uint32_t x = 0; x < width; ++x) {
      dst[x * 4 + 0] = src[x * bytes_pp + 2];
      dst[x * 4 + 1] = src[x * bytes_pp + 1];
      dst[x * 4 + 2] = src[x * bytes_pp + 0];
      dst[x * 4 + 3] = (bytes_pp == 4) ? src[x * bytes_pp + 3] : 255;
    }
  }
  return out;
}

}  // namespace mv::codec
