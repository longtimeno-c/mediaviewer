// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/format.h"

#include <cstring>

namespace mv::codec {
namespace {

bool eq4(const std::uint8_t* p, const char* s) noexcept {
  return p[0] == static_cast<std::uint8_t>(s[0]) && p[1] == static_cast<std::uint8_t>(s[1]) &&
         p[2] == static_cast<std::uint8_t>(s[2]) && p[3] == static_cast<std::uint8_t>(s[3]);
}

// ISO BMFF: size(4) + 'ftyp'(4) + major(4) + minor(4) + compatible brands.
// Match major or any compatible brand. `want` is a 4-char code.
bool ftyp_has_brand(std::span<const std::uint8_t> header, const char* want) noexcept {
  if (header.size() < 12 || !eq4(header.data() + 4, "ftyp")) return false;
  if (eq4(header.data() + 8, want)) return true;
  // Compatible brands start at offset 16.
  for (std::size_t i = 16; i + 4 <= header.size(); i += 4) {
    if (eq4(header.data() + i, want)) return true;
  }
  return false;
}

bool ftyp_heif(std::span<const std::uint8_t> header) noexcept {
  static constexpr const char* kBrands[] = {
      "heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "mif1", "msf1",
  };
  for (const char* b : kBrands) {
    if (ftyp_has_brand(header, b)) return true;
  }
  return false;
}

}  // namespace

format_family probe(std::span<const std::uint8_t> header) noexcept {
  if (header.size() >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return format_family::jpeg;
  }
  static constexpr std::uint8_t kPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (header.size() >= 8 && std::memcmp(header.data(), kPng, 8) == 0) {
    return format_family::png;
  }
  if (header.size() >= 2 && header[0] == 'B' && header[1] == 'M') {
    return format_family::bmp;
  }
  if (header.size() >= 6 && (std::memcmp(header.data(), "GIF87a", 6) == 0 ||
                             std::memcmp(header.data(), "GIF89a", 6) == 0)) {
    return format_family::gif;
  }
  if (header.size() >= 12 && std::memcmp(header.data(), "RIFF", 4) == 0 &&
      std::memcmp(header.data() + 8, "WEBP", 4) == 0) {
    return format_family::webp;
  }
  // ICO before TIFF: both can start with mostly-zero headers. ICONDIR is
  // reserved=0, type=1, count>0.
  if (header.size() >= 6 && header[0] == 0 && header[1] == 0 && header[2] == 1 && header[3] == 0 &&
      (header[4] | header[5]) != 0) {
    return format_family::ico;
  }
  // ISO BMFF: AVIF and CR3 before generic HEIF so avif/crx win over mif1.
  if (ftyp_has_brand(header, "avif") || ftyp_has_brand(header, "avis")) {
    return format_family::avif;
  }
  if (ftyp_has_brand(header, "crx ")) {  // four-char brand: the space is part of it
    return format_family::raw;  // Canon CR3
  }
  if (ftyp_heif(header)) {
    return format_family::heic;
  }
  // Non-TIFF RAW magics. TIFF-container RAWs (CR2/NEF/ARW/DNG) probe as TIFF;
  // decode() reclassifies through LibRaw (plan/04: magic, then identify).
  if (header.size() >= 16 && std::memcmp(header.data(), "FUJIFILMCCD-RAW", 15) == 0) {
    return format_family::raw;
  }
  if (header.size() >= 4 && (std::memcmp(header.data(), "IIRO", 4) == 0 ||
                             std::memcmp(header.data(), "MMOR", 4) == 0 ||
                             std::memcmp(header.data(), "IIRS", 4) == 0)) {
    return format_family::raw;  // Olympus ORF
  }
  if (header.size() >= 4 && header[0] == 'I' && header[1] == 'I' && header[2] == 'U' &&
      header[3] == 0) {
    return format_family::raw;  // Panasonic RW2
  }
  if (header.size() >= 4 && ((header[0] == 'I' && header[1] == 'I' && header[2] == 42 &&
                              header[3] == 0) ||
                             (header[0] == 'M' && header[1] == 'M' && header[2] == 0 &&
                              header[3] == 42))) {
    return format_family::tiff;
  }
  return format_family::unknown;
}

}  // namespace mv::codec
