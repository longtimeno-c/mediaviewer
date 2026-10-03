// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// BMP: BITMAPFILEHEADER + a DIB (codec/dib.h has every header and pixel
// layout). BI_JPEG / BI_PNG payloads go to the JPEG / PNG decoders. A V5
// embedded profile is carried; a 32-bit file whose alpha is all zero is
// opaque (the fourth byte was "reserved" before V3 headers gave it meaning).
#include "codec/decode.h"
#include "codec/dib.h"

#include <new>

namespace mv::codec {
namespace {

std::uint32_t u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::size_t kFileHeader = 14;

}  // namespace

result<raster> decode_bmp(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::bmp) return err(status::unsupported_format);
  if (bytes.size() < kFileHeader + 12) return err(status::corrupt);

  const std::uint32_t pixel_off = u32(bytes.data() + 10);
  const auto dib_bytes = bytes.subspan(kFileHeader);
  const std::uint64_t table_limit = pixel_off > kFileHeader ? pixel_off - kFileHeader : 0;
  auto parsed = dib::parse(dib_bytes, table_limit, false);
  if (!parsed) return err(parsed.error());
  const dib::header& h = *parsed;
  if (pixel_off < kFileHeader + h.size || pixel_off >= bytes.size()) return err(status::corrupt);
  const auto bits = bytes.subspan(pixel_off);

  if (h.compression == dib::bi_jpeg || h.compression == dib::bi_png) {
    auto inner = h.compression == dib::bi_jpeg ? decode_jpeg(bits, ctx) : decode_png(bits, ctx);
    if (!inner) return err(inner.error() == status::unsupported_format ? status::corrupt : inner.error());
    inner->format = format_family::bmp;
    return inner;
  }

  raster out;
  out.format = format_family::bmp;
  out.intent = transfer_intent::display_referred;
  out.width = h.width;
  out.height = h.height;
  out.tagged_srgb = h.srgb;
  try {
    out.rgba.resize(static_cast<std::size_t>(h.width) * h.height * 4);
    if (h.icc_size > 0) {
      const std::uint8_t* icc = dib_bytes.data() + h.icc_offset;
      out.icc.assign(icc, icc + h.icc_size);
    }
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  bool alpha_seen = false;
  const status s = dib::pixels(h, dib_bytes, bits, out.rgba.data(), ctx, alpha_seen);
  if (s != status::ok) return err(s);
  if (h.alpha_channel() && !alpha_seen) {
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  }
  return out;
}

}  // namespace mv::codec
