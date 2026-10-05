// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/dib.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace mv::codec::dib {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
constexpr std::uint32_t kProfileEmbedded = 0x4D424544;  // 'MBED'
constexpr std::uint32_t kLcsSrgb = 0x73524742;          // 'sRGB'
constexpr std::uint32_t kLcsWindows = 0x57696E20;       // 'Win '

std::uint16_t u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int32_t i32(const std::uint8_t* p) noexcept { return static_cast<std::int32_t>(u32(p)); }

// One mask channel, scaled to 8 bits. A mask wider than 8 bits keeps its top
// 8; a narrower one is stretched so its maximum is 255.
struct channel {
  std::uint32_t mask = 0;
  unsigned shift = 0;
  unsigned bits = 0;

  explicit channel(std::uint32_t m) noexcept : mask(m) {
    if (m == 0) return;
    shift = static_cast<unsigned>(std::countr_zero(m));
    bits = static_cast<unsigned>(std::bit_width(m >> shift));
  }
  [[nodiscard]] std::uint8_t get(std::uint32_t px) const noexcept {
    // An empty mask is a channel the file does not carry (a 2-channel
    // BI_BITFIELDS image, or BI_ALPHABITFIELDS with no alpha bits): it reads
    // 0, never divides by its zero maximum (the ICO fuzzer found this).
    if (bits == 0) return 0;
    const std::uint32_t v = (px & mask) >> shift;
    if (bits >= 8) return static_cast<std::uint8_t>(v >> (bits - 8));
    const std::uint32_t max = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((v * 255u + max / 2u) / max);
  }
};

struct palette_view {
  const std::uint8_t* base = nullptr;
  std::uint32_t count = 0;
  std::uint32_t entry = 4;

  // Out-of-range indices are black, as GDI draws them.
  void put(std::uint32_t i, std::uint8_t* d) const noexcept {
    if (i < count) {
      const std::uint8_t* c = base + static_cast<std::size_t>(i) * entry;
      d[0] = c[2];
      d[1] = c[1];
      d[2] = c[0];
    } else {
      d[0] = d[1] = d[2] = 0;
    }
    d[3] = 255;
  }
};

std::uint8_t* row_ptr(const header& h, std::uint8_t* rgba, std::uint32_t stored_row) noexcept {
  const std::uint32_t y = h.bottom_up ? h.height - 1 - stored_row : stored_row;
  return rgba + static_cast<std::size_t>(y) * h.width * 4;
}

status unpacked(const header& h, const palette_view& pal, std::span<const std::uint8_t> bits,
                std::uint8_t* rgba, const job_context* ctx, bool& alpha_seen) {
  const std::uint64_t stride = (static_cast<std::uint64_t>(h.width) * h.bpp + 31) / 32 * 4;
  if (stride * h.height > bits.size()) return status::corrupt;

  const bool masked = h.compression == bi_bitfields || h.compression == bi_alphabitfields;
  // BI_RGB 16-bit is 5-5-5; BI_RGB 32-bit is read bytewise below.
  const std::uint32_t m[4] = {masked ? h.mask[0] : 0x7C00u, masked ? h.mask[1] : 0x03E0u,
                              masked ? h.mask[2] : 0x001Fu, masked ? h.mask[3] : 0u};
  const channel cr(m[0]), cg(m[1]), cb(m[2]), ca(m[3]);
  const bool alpha = h.alpha_channel();

  for (std::uint32_t sy = 0; sy < h.height; ++sy) {
    if ((sy & 63u) == 0 && ctx && ctx->cancelled()) return status::cancelled;
    const std::uint8_t* src = bits.data() + sy * stride;
    std::uint8_t* dst = row_ptr(h, rgba, sy);
    for (std::uint32_t x = 0; x < h.width; ++x) {
      std::uint8_t* d = dst + static_cast<std::size_t>(x) * 4;
      switch (h.bpp) {
        case 1:
        case 2:
        case 4:
        case 8: {
          const std::uint32_t bit = x * h.bpp;
          const unsigned shift = 8u - h.bpp - (bit & 7u);
          pal.put((static_cast<std::uint32_t>(src[bit >> 3]) >> shift) & ((1u << h.bpp) - 1u), d);
          break;
        }
        case 16:
        case 32: {
          if (h.bpp == 32 && !masked) {
            d[0] = src[x * 4 + 2];
            d[1] = src[x * 4 + 1];
            d[2] = src[x * 4 + 0];
            d[3] = src[x * 4 + 3];
          } else {
            const std::uint32_t px = h.bpp == 16 ? u16(src + x * 2) : u32(src + x * 4);
            d[0] = cr.get(px);
            d[1] = cg.get(px);
            d[2] = cb.get(px);
            d[3] = alpha ? ca.get(px) : 255;
          }
          alpha_seen = alpha_seen || (alpha && d[3] != 0);
          break;
        }
        default:  // 24
          d[0] = src[x * 3 + 2];
          d[1] = src[x * 3 + 1];
          d[2] = src[x * 3 + 0];
          d[3] = 255;
          break;
      }
    }
  }
  return status::ok;
}

// RLE8 / RLE4. Runs, absolute runs, end-of-line, delta, end-of-bitmap. The
// raster starts transparent: pixels a delta or early end skips stay clear.
status rle(const header& h, const palette_view& pal, std::span<const std::uint8_t> bits,
           std::uint8_t* rgba, const job_context* ctx) {
  std::memset(rgba, 0, static_cast<std::size_t>(h.width) * h.height * 4);
  const bool four = h.compression == bi_rle4;
  const std::uint8_t* b = bits.data();
  const std::size_t n = bits.size();
  std::size_t pos = 0;
  std::uint32_t x = 0, y = 0;
  const auto put = [&](std::uint32_t idx) {
    if (x < h.width && y < h.height) pal.put(idx, row_ptr(h, rgba, y) + static_cast<std::size_t>(x) * 4);
    ++x;
  };
  while (pos + 1 < n && y < h.height) {
    const std::uint32_t count = b[pos];
    const std::uint32_t code = b[pos + 1];
    pos += 2;
    if (count > 0) {  // encoded run: one index, or two alternating nibbles
      for (std::uint32_t i = 0; i < count && x < h.width; ++i) {
        put(four ? ((i & 1u) ? (code & 0xFu) : (code >> 4)) : code);
      }
      continue;
    }
    if (code == 0) {  // end of line
      x = 0;
      ++y;
      if ((y & 63u) == 0 && ctx && ctx->cancelled()) return status::cancelled;
    } else if (code == 1) {  // end of bitmap
      break;
    } else if (code == 2) {  // delta
      if (pos + 1 >= n) break;
      x += b[pos];
      y += b[pos + 1];
      pos += 2;
    } else {  // absolute run of `code` pixels, padded to a 16-bit boundary
      const std::size_t bytes = four ? (code + 1u) / 2u : code;
      if (pos + bytes > n) return status::corrupt;
      for (std::uint32_t i = 0; i < code; ++i) {
        const std::uint8_t v = b[pos + (four ? i / 2u : i)];
        put(four ? ((i & 1u) ? (v & 0xFu) : (v >> 4)) : v);
      }
      pos += (bytes + 1u) & ~std::size_t{1};
    }
  }
  return status::ok;
}

}  // namespace

result<header> parse(std::span<const std::uint8_t> dib, std::uint64_t table_limit, bool icon) {
  const std::uint64_t n = dib.size();
  if (n < 12) return err(status::corrupt);
  const std::uint8_t* p = dib.data();
  header h;
  h.size = u32(p);
  if (h.size < 12 || h.size > n) return err(status::corrupt);
  table_limit = std::min<std::uint64_t>(table_limit, n);

  std::uint32_t clr_used = 0;
  std::int64_t height_s = 0;
  std::int64_t width_s = 0;
  if (h.size == 12) {  // BITMAPCOREHEADER: 16-bit sizes, always bottom-up
    width_s = u16(p + 4);
    height_s = u16(p + 6);
    if (u16(p + 8) != 1) return err(status::corrupt);
    h.bpp = u16(p + 10);
    h.palette_entry = 3;
  } else {
    if (h.size < 16) return err(status::corrupt);
    width_s = i32(p + 4);
    height_s = i32(p + 8);
    if (u16(p + 12) != 1) return err(status::corrupt);
    h.bpp = u16(p + 14);
    if (h.size >= 20) h.compression = u32(p + 16);
    if (h.size >= 36) clr_used = u32(p + 32);
    // OS/2 2.x reuses 3 and 4 for Huffman 1D and RLE24.
    const bool os2 = h.size == 64 || (h.size > 12 && h.size < 40);
    if (os2 && h.compression >= bi_bitfields) return err(status::unsupported_format);
  }
  if (width_s <= 0 || height_s == 0 || height_s == std::numeric_limits<std::int32_t>::min()) {
    return err(status::corrupt);
  }
  h.bottom_up = height_s > 0;
  std::uint64_t height = static_cast<std::uint64_t>(height_s > 0 ? height_s : -height_s);
  if (icon) height /= 2u;
  if (height == 0) return err(status::corrupt);
  if (static_cast<std::uint64_t>(width_s) > kMaxDim || height > kMaxDim ||
      static_cast<std::uint64_t>(width_s) * height > kMaxPixels) {
    return err(status::unsupported_format);
  }
  h.width = static_cast<std::uint32_t>(width_s);
  h.height = static_cast<std::uint32_t>(height);

  switch (h.compression) {
    case bi_rgb:
      if (h.bpp != 1 && h.bpp != 2 && h.bpp != 4 && h.bpp != 8 && h.bpp != 16 && h.bpp != 24 &&
          h.bpp != 32) {
        return err(status::unsupported_format);
      }
      break;
    case bi_rle8:
      if (h.bpp != 8) return err(status::unsupported_format);
      break;
    case bi_rle4:
      if (h.bpp != 4) return err(status::unsupported_format);
      break;
    case bi_bitfields:
    case bi_alphabitfields:
      if (h.bpp != 16 && h.bpp != 32) return err(status::unsupported_format);
      break;
    case bi_jpeg:
    case bi_png:
      break;
    default:  // BI_CMYK and friends (print spoolers only)
      return err(status::unsupported_format);
  }

  std::uint64_t off = h.size;
  if (h.compression == bi_bitfields || h.compression == bi_alphabitfields) {
    const std::uint32_t want = h.compression == bi_alphabitfields ? 4u : 3u;
    if (h.size >= 52) {  // V2+: the masks are header fields
      for (std::uint32_t i = 0; i < 3; ++i) h.mask[i] = u32(p + 40 + 4 * i);
      if (h.size >= 56) h.mask[3] = u32(p + 52);
    } else {  // BITMAPINFOHEADER: the masks follow it
      if (off + 4ull * want > n) return err(status::corrupt);
      for (std::uint32_t i = 0; i < want; ++i) h.mask[i] = u32(p + off + 4 * i);
      off += 4ull * want;
    }
    if (h.mask[0] == 0 && h.mask[1] == 0 && h.mask[2] == 0) return err(status::corrupt);
  }

  // Palette: the declared count sets where the tables end; lookups never go
  // past 2^bpp or past the bytes the caller allows for tables.
  std::uint64_t declared = clr_used;
  const bool indexed = h.bpp <= 8 && h.compression != bi_jpeg && h.compression != bi_png;
  if (indexed && declared == 0) declared = 1ull << h.bpp;
  declared = std::min<std::uint64_t>(declared, 65536);
  h.palette_offset = static_cast<std::uint32_t>(off);
  const std::uint64_t fit = off < table_limit ? (table_limit - off) / h.palette_entry : 0;
  const std::uint64_t usable = std::min({declared, fit, indexed ? (1ull << h.bpp) : 0ull});
  h.palette_count = static_cast<std::uint32_t>(usable);
  h.after_tables = static_cast<std::uint32_t>(std::min<std::uint64_t>(off + declared * h.palette_entry, n));

  if (h.size >= 108) {  // V4 / V5 colour space
    const std::uint32_t cs = u32(p + 56);
    h.srgb = cs == kLcsSrgb || cs == kLcsWindows;
    if (h.size >= 124 && cs == kProfileEmbedded) {
      const std::uint32_t at = u32(p + 112);
      const std::uint32_t len = u32(p + 116);
      if (len >= 128 && at >= h.size && static_cast<std::uint64_t>(at) + len <= n) {
        h.icc_offset = at;
        h.icc_size = len;
      }
    }
  }
  return h;
}

status pixels(const header& h, std::span<const std::uint8_t> dib, std::span<const std::uint8_t> bits,
              std::uint8_t* rgba, const job_context* ctx, bool& alpha_seen) {
  const palette_view pal{dib.data() + h.palette_offset, h.palette_count, h.palette_entry};
  switch (h.compression) {
    case bi_rle8:
    case bi_rle4:
      return rle(h, pal, bits, rgba, ctx);
    case bi_rgb:
    case bi_bitfields:
    case bi_alphabitfields:
      return unpacked(h, pal, bits, rgba, ctx, alpha_seen);
    default:
      return status::unsupported_format;
  }
}

}  // namespace mv::codec::dib
