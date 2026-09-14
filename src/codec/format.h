// SPDX-License-Identifier: GPL-2.0-or-later
// Format family identified by magic bytes, never by extension
// (plan/04-image-pipeline.md).
#pragma once

#include <cstdint>
#include <span>

namespace mv::codec {

enum class format_family : std::uint32_t {
  unknown = 0,
  jpeg = 1,
  png = 2,
  bmp = 3,
  gif = 4,   // GIF87a / GIF89a (PR 6, animated)
  webp = 5,  // RIFF....WEBP (PR 6 pulled forward from PR 7: still + animated)
  tiff = 6,  // II*\0 / MM\0* — TIFF-container RAWs reclassify in decode()
  ico = 7,   // ICONDIR type=1
  heic = 8,  // ISO BMFF ftyp heic/heix/mif1/msf1 (libheif + libde265)
  avif = 9,  // ISO BMFF ftyp avif/avis (libavif + dav1d)
  raw = 10,  // CR3/RAF/ORF/RW2 magics; CR2/NEF/ARW/DNG via LibRaw on TIFF
};

// First bytes only. A short or empty span is unknown, not corrupt.
[[nodiscard]] format_family probe(std::span<const std::uint8_t> header) noexcept;

[[nodiscard]] constexpr const char* format_name(format_family f) noexcept {
  switch (f) {
    case format_family::jpeg: return "JPEG";
    case format_family::png:  return "PNG";
    case format_family::bmp:  return "BMP";
    case format_family::gif:  return "GIF";
    case format_family::webp: return "WebP";
    case format_family::tiff: return "TIFF";
    case format_family::ico:  return "ICO";
    case format_family::heic: return "HEIC";
    case format_family::avif: return "AVIF";
    case format_family::raw:  return "RAW";
    case format_family::unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace mv::codec
