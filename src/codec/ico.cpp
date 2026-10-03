// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ICO: ICONDIR of BMP (no BITMAPFILEHEADER) or PNG images. Largest size is
// the still; other sizes are Ctrl+PageUp/PageDown inside one folder stop.
//
// Entries are ranked by the payload's own dimensions (directory bytes are
// 8-bit, 0 means 256, and are often wrong), then bit depth. If the best entry
// is unreadable the next one is tried; the first error is returned when none
// decode. Every offset and size is checked against the buffer.
#include "codec/decode.h"
#include "codec/dib.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mv::codec {
namespace {

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

// Info header + colour tables + XOR (colour) rows + AND (1-bit mask) rows.
// The header height counts both bitmaps; codec/dib.h decodes the XOR rows.
result<raster> decode_dib(std::span<const std::uint8_t> e, const job_context* ctx) {
  auto parsed = dib::parse(e, e.size(), true);
  if (!parsed) return err(parsed.error());
  const dib::header& h = *parsed;
  if (h.compression == dib::bi_jpeg || h.compression == dib::bi_png) {
    return err(status::unsupported_format);  // an icon's PNG is a bare PNG entry
  }
  const std::uint64_t n = e.size();
  const std::uint64_t xor_off = h.after_tables;
  // RLE rows have no fixed size; the AND mask then sits at the end of the entry.
  const bool rle = h.compression == dib::bi_rle8 || h.compression == dib::bi_rle4;
  const std::uint64_t xor_stride = (static_cast<std::uint64_t>(h.width) * h.bpp + 31) / 32 * 4;
  const std::uint64_t and_stride = (static_cast<std::uint64_t>(h.width) + 31) / 32 * 4;
  const std::uint64_t and_bytes = and_stride * h.height;
  if (xor_off >= n) return err(status::corrupt);
  std::uint64_t xor_end = 0;
  if (rle) {
    xor_end = n >= xor_off + and_bytes ? n - and_bytes : n;
  } else {
    xor_end = xor_off + xor_stride * h.height;
    if (xor_end > n) return err(status::corrupt);
  }
  // The AND mask is optional in practice: a writer that dropped it gets opaque.
  const bool has_mask = xor_end + and_bytes <= n && xor_end > xor_off;

  raster out;
  out.format = format_family::ico;
  out.intent = transfer_intent::display_referred;
  out.width = h.width;
  out.height = h.height;
  try {
    out.rgba.resize(static_cast<std::size_t>(h.width) * h.height * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  bool alpha_seen = false;
  const status s = dib::pixels(h, e, e.subspan(xor_off, xor_end - xor_off), out.rgba.data(), ctx,
                               alpha_seen);
  if (s != status::ok) return err(s);

  // Alpha the pixels really carry wins. Otherwise (no alpha channel, or a
  // 32-bit entry whose alpha is all zero, which predates alpha icons) the AND
  // mask is the transparency, or the entry is opaque.
  if (!(h.alpha_channel() && alpha_seen)) {
    const std::uint8_t* p = e.data();
    for (std::uint32_t y = 0; y < h.height; ++y) {
      const std::uint64_t sy = h.bottom_up ? h.height - 1 - y : y;
      const std::uint8_t* mask = has_mask ? p + xor_end + sy * and_stride : nullptr;
      std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * h.width * 4;
      for (std::uint32_t x = 0; x < h.width; ++x) {
        if (mask && mask_bit(mask, x)) {
          dst[x * 4 + 3] = 0;
        } else if (!rle) {
          dst[x * 4 + 3] = 255;
        }
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
