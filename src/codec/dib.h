// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Device-independent bitmap: the info header, colour tables and pixel bits a
// BMP file and an ICO entry share. bmp.cpp and ico.cpp parse their wrappers
// (file header, icon directory and AND mask) and hand the DIB here.
//
// Header: BITMAPCOREHEADER (OS/2 1.x, 12 bytes), BITMAPINFOHEADER and its
// V2-V5 extensions, OS/2 2.x (16-64 bytes, its Huffman/RLE24 refused).
// Pixels: 1/2/4/8-bit palette, 16/24/32-bit, BI_BITFIELDS and
// BI_ALPHABITFIELDS masks, RLE8 and RLE4 (skipped pixels are transparent).
// BI_JPEG / BI_PNG payloads are decoded by bmp.cpp, not here.
#pragma once

#include <cstdint>
#include <span>

#include "codec/decode.h"

namespace mv::codec::dib {

inline constexpr std::uint32_t bi_rgb = 0;
inline constexpr std::uint32_t bi_rle8 = 1;
inline constexpr std::uint32_t bi_rle4 = 2;
inline constexpr std::uint32_t bi_bitfields = 3;
inline constexpr std::uint32_t bi_jpeg = 4;
inline constexpr std::uint32_t bi_png = 5;
inline constexpr std::uint32_t bi_alphabitfields = 6;

struct header {
  std::uint32_t size = 0;  // info header bytes
  std::uint32_t width = 0;
  std::uint32_t height = 0;  // colour bitmap rows (an icon's doubled height is halved)
  bool bottom_up = true;
  std::uint16_t bpp = 0;
  std::uint32_t compression = bi_rgb;
  std::uint32_t mask[4] = {};  // R, G, B, A for BI_BITFIELDS / BI_ALPHABITFIELDS
  std::uint32_t palette_offset = 0;  // from the DIB start
  std::uint32_t palette_count = 0;   // usable entries (never more than 2^bpp)
  std::uint32_t palette_entry = 4;   // 3 for BITMAPCOREHEADER
  std::uint32_t after_tables = 0;    // first byte past the header, masks and palette
  std::uint32_t icc_offset = 0;      // V5 PROFILE_EMBEDDED, from the DIB start
  std::uint32_t icc_size = 0;
  bool srgb = false;                 // V4/V5 LCS_sRGB or LCS_WINDOWS_COLOR_SPACE

  // The pixels carry alpha: 32-bit BI_RGB's fourth byte, or an alpha mask.
  [[nodiscard]] bool alpha_channel() const noexcept {
    return (bpp == 32 && compression == bi_rgb) || mask[3] != 0;
  }
};

// `dib` starts at the info header. `table_limit` is how many bytes from the
// DIB start may hold the header, masks and palette (a BMP's pixel offset);
// a palette that runs past it is cut to the entries that fit. `icon`: the
// header height counts the XOR and AND bitmaps.
[[nodiscard]] result<header> parse(std::span<const std::uint8_t> dib, std::uint64_t table_limit,
                                   bool icon);

// Expands `bits` (the first pixel byte to the end of the data) into `rgba`,
// width * height * 4, top row first. `dib` is the span parse() saw (for the
// palette). `alpha_seen` is set when the alpha channel held a non-zero value;
// writers that leave it all zero mean opaque.
[[nodiscard]] status pixels(const header& h, std::span<const std::uint8_t> dib,
                            std::span<const std::uint8_t> bits, std::uint8_t* rgba,
                            const job_context* ctx, bool& alpha_seen);

}  // namespace mv::codec::dib
