// SPDX-License-Identifier: GPL-2.0-or-later
// ICO: ICONDIR of BMP (no BITMAPFILEHEADER) or PNG images. Largest size is
// the still; other sizes are Ctrl+PageUp/PageDown inside one folder stop.
//
// Entries are ranked by the payload's own dimensions (directory bytes are
// 8-bit, 0 means 256, and are often wrong), then bit depth. If the best entry
// is unreadable the next one is tried; the first error is returned when none
// decode. Every offset and size is checked against the buffer.
#include "codec/decode.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
constexpr std::size_t kDirHeader = 6;
constexpr std::size_t kDirEntry = 16;
constexpr std::uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

std::uint16_t u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int32_t i32(const std::uint8_t* p) noexcept { return static_cast<std::int32_t>(u32(p)); }
std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

bool is_png(std::span<const std::uint8_t> e) noexcept {
  return e.size() >= 8 && std::memcmp(e.data(), kPngSig, 8) == 0;
}

struct candidate {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  std::uint64_t area = 0;
  std::uint32_t bits = 0;
  std::uint16_t index = 0;
};

bool describe(std::span<const std::uint8_t> e, candidate& c) noexcept {
  const std::uint8_t* p = e.data();
  if (is_png(e)) {
    if (e.size() < 33 || std::memcmp(p + 12, "IHDR", 4) != 0) return false;
    const std::uint32_t w = be32(p + 16);
    const std::uint32_t h = be32(p + 20);
    if (w == 0 || h == 0) return false;
    const std::uint32_t depth = p[24];
    const std::uint8_t type = p[25];
    const std::uint32_t channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
    c.area = static_cast<std::uint64_t>(w) * h;
    c.bits = type == 3 ? depth : depth * channels;
    return true;
  }
  if (e.size() < 40) return false;
  const std::uint32_t hdr = u32(p);
  if (hdr < 40 || hdr > e.size()) return false;
  const std::int32_t w = i32(p + 4);
  const std::int32_t hs = i32(p + 8);
  if (w <= 0 || hs == 0 || hs == std::numeric_limits<std::int32_t>::min()) return false;
  const std::uint32_t h = static_cast<std::uint32_t>(hs > 0 ? hs : -hs) / 2u;
  if (h == 0) return false;
  c.area = static_cast<std::uint64_t>(w) * h;
  c.bits = u16(p + 14);
  return true;
}

inline bool mask_bit(const std::uint8_t* row, std::uint32_t x) noexcept {
  return ((row[x >> 3] >> (7u - (x & 7u))) & 1u) != 0;
}

// BITMAPINFOHEADER + palette + XOR (colour) rows + AND (1-bit mask) rows. The
// header height counts both bitmaps.
result<raster> decode_dib(std::span<const std::uint8_t> e, const job_context* ctx) {
  const std::uint64_t n = e.size();
  if (n < 40) return err(status::corrupt);
  const std::uint8_t* p = e.data();
  const std::uint32_t hdr = u32(p);
  if (hdr < 40 || hdr > n) return err(status::corrupt);
  const std::int32_t ws = i32(p + 4);
  const std::int32_t hs = i32(p + 8);
  const std::uint16_t bpp = u16(p + 14);
  const std::uint32_t compression = u32(p + 16);
  const std::uint32_t clr_used = u32(p + 32);
  if (ws <= 0 || hs == 0 || hs == std::numeric_limits<std::int32_t>::min()) {
    return err(status::corrupt);
  }
  const auto width = static_cast<std::uint32_t>(ws);
  const bool bottom_up = hs > 0;
  const std::uint32_t height = static_cast<std::uint32_t>(hs > 0 ? hs : -hs) / 2u;
  if (height == 0) return err(status::corrupt);
  if (width > kMaxDim || height > kMaxDim ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    return err(status::unsupported_format);
  }
  if (compression != 0) return err(status::unsupported_format);  // BI_RGB only
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
    return err(status::unsupported_format);
  }

  std::uint64_t palette = 0;
  if (bpp <= 8) {
    const std::uint64_t max = 1ull << bpp;
    palette = clr_used ? clr_used : max;
    if (palette > max) return err(status::corrupt);
  }
  const std::uint64_t pal_off = hdr;
  const std::uint64_t xor_off = pal_off + palette * 4;
  const std::uint64_t xor_stride = (static_cast<std::uint64_t>(width) * bpp + 31) / 32 * 4;
  const std::uint64_t and_stride = (static_cast<std::uint64_t>(width) + 31) / 32 * 4;
  const std::uint64_t xor_end = xor_off + xor_stride * height;
  if (xor_end > n) return err(status::corrupt);
  // The AND mask is optional in practice: a writer that dropped it gets opaque.
  const bool has_mask = xor_end + and_stride * height <= n;

  raster out;
  out.format = format_family::ico;
  out.intent = transfer_intent::display_referred;
  out.width = width;
  out.height = height;
  try {
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  bool any_alpha = false;
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 63u) == 0 && ctx && ctx->cancelled()) return err(status::cancelled);
    const std::uint64_t sy = bottom_up ? height - 1 - y : y;
    const std::uint8_t* row = p + xor_off + sy * xor_stride;
    const std::uint8_t* mask = has_mask ? p + xor_end + sy * and_stride : nullptr;
    std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t r = 0, g = 0, b = 0, a = 255;
      if (bpp == 32) {
        b = row[x * 4];
        g = row[x * 4 + 1];
        r = row[x * 4 + 2];
        a = row[x * 4 + 3];
        any_alpha = any_alpha || a != 0;
      } else if (bpp == 24) {
        b = row[x * 3];
        g = row[x * 3 + 1];
        r = row[x * 3 + 2];
      } else {
        std::uint32_t idx = 0;
        if (bpp == 8) {
          idx = row[x];
        } else if (bpp == 4) {
          idx = (static_cast<std::uint32_t>(row[x >> 1]) >> ((x & 1u) ? 0u : 4u)) & 0xFu;
        } else {
          idx = mask_bit(row, x) ? 1u : 0u;
        }
        if (idx < palette) {
          const std::uint8_t* c = p + pal_off + static_cast<std::size_t>(idx) * 4;
          b = c[0];
          g = c[1];
          r = c[2];
        }
      }
      if (bpp != 32 && mask && mask_bit(mask, x)) a = 0;
      dst[x * 4 + 0] = r;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = b;
      dst[x * 4 + 3] = a;
    }
  }

  // A 32-bit entry whose alpha is all zero predates alpha icons: its AND mask
  // is the transparency (or it is opaque).
  if (bpp == 32 && !any_alpha) {
    for (std::uint32_t y = 0; y < height; ++y) {
      const std::uint64_t sy = bottom_up ? height - 1 - y : y;
      const std::uint8_t* mask = has_mask ? p + xor_end + sy * and_stride : nullptr;
      std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
      for (std::uint32_t x = 0; x < width; ++x) {
        dst[x * 4 + 3] = (mask && mask_bit(mask, x)) ? 0 : 255;
      }
    }
  }
  return out;
}

}  // namespace

result<raster> decode_ico(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::ico) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  if (bytes.size() < kDirHeader) return err(status::corrupt);

  const std::uint16_t count = u16(bytes.data() + 4);
  if (kDirHeader + kDirEntry * static_cast<std::size_t>(count) > bytes.size()) {
    return err(status::corrupt);
  }

  std::vector<candidate> cands;
  try {
    cands.reserve(count);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint8_t* d = bytes.data() + kDirHeader + kDirEntry * i;
    candidate c;
    c.size = u32(d + 8);
    c.offset = u32(d + 12);
    c.index = i;
    if (c.size == 0 || static_cast<std::uint64_t>(c.offset) + c.size > bytes.size()) continue;
    if (!describe(bytes.subspan(c.offset, c.size), c)) continue;
    cands.push_back(c);
  }
  if (cands.empty()) return err(status::corrupt);

  std::sort(cands.begin(), cands.end(), [](const candidate& a, const candidate& b) {
    if (a.area != b.area) return a.area > b.area;
    if (a.bits != b.bits) return a.bits > b.bits;
    return a.index < b.index;
  });

  status first = status::corrupt;
  bool have_error = false;
  for (const candidate& c : cands) {
    const auto entry = bytes.subspan(c.offset, c.size);
    result<raster> r = is_png(entry) ? decode_png(entry, ctx) : decode_dib(entry, ctx);
    if (r) {
      r->format = format_family::ico;
      return r;
    }
    if (r.error() == status::cancelled || r.error() == status::out_of_memory) {
      return err(r.error());
    }
    if (!have_error) {
      first = r.error();
      have_error = true;
    }
  }
  return err(first);
}

}  // namespace mv::codec
