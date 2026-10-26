// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/sniff.h"

#include <cstring>
#include <string_view>

namespace mv::enhance {
namespace {

bool starts(std::span<const std::uint8_t> h, std::string_view magic, std::size_t at = 0) noexcept {
  return h.size() >= at + magic.size() && std::memcmp(h.data() + at, magic.data(), magic.size()) == 0;
}

// The ISO base media file's major brand and its compatible brands, in the
// first box when it is `ftyp`.
item_kind iso_brand(std::span<const std::uint8_t> h) noexcept {
  if (!starts(h, "ftyp", 4)) return item_kind::unknown;
  const std::size_t box = (std::size_t{h[0]} << 24) | (std::size_t{h[1]} << 16) | (std::size_t{h[2]} << 8) | h[3];
  const std::size_t end = box >= 16 && box <= h.size() ? box : h.size();
  const auto brand_at = [&](std::size_t at) { return std::string_view(reinterpret_cast<const char*>(h.data() + at), 4); };
  if (end < 12) return item_kind::unknown;
  const std::string_view major = brand_at(8);
  if (major == "M4A " || major == "M4B " || major == "M4P ") return item_kind::audio;
  if (major == "avis" || major == "msf1" || major == "hevs") return item_kind::animation;
  if (major == "heic" || major == "heix" || major == "heim" || major == "heis" || major == "mif1" ||
      major == "avif" || major == "crx ") {
    return item_kind::still;
  }
  // A camera's major brand is often generic ("isom", "mp42", "qt  "); a
  // HEIF that leads with one names mif1 among its compatible brands.
  for (std::size_t at = 16; at + 4 <= end; at += 4) {
    const std::string_view b = brand_at(at);
    if (b == "mif1" || b == "heic" || b == "avif") return item_kind::still;
  }
  return item_kind::media;
}

}  // namespace

item_kind sniff(std::span<const std::uint8_t> h) noexcept {
  if (h.size() < 4) return item_kind::unknown;
  if (starts(h, "%PDF-")) return item_kind::document;
  if (starts(h, "PK\x03\x04")) return item_kind::document;  // DOCX / PPTX
  if (starts(h, "GIF87a") || starts(h, "GIF89a")) return item_kind::animation;
  if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return item_kind::still;  // JPEG
  if (starts(h, "\x89PNG\r\n\x1a\n")) return item_kind::still;
  if (starts(h, "II*\0") || starts(h, "MM\0*") || starts(h, "IIRO") || starts(h, "IIU\0") ||
      starts(h, "FUJIFILMCCD-RAW")) {
    return item_kind::still;  // TIFF and the TIFF-shaped RAWs, ORF, RW2, RAF
  }
  if (starts(h, "BM") && h.size() >= 14) return item_kind::still;
  if (starts(h, std::string_view("\0\0\1\0", 4))) return item_kind::still;  // ICO
  if (starts(h, "RIFF") && starts(h, "WEBP", 8)) return item_kind::still;
  if (starts(h, "RIFF") && starts(h, "AVI ", 8)) return item_kind::media;
  if (starts(h, "RIFF") && starts(h, "WAVE", 8)) return item_kind::audio;
  if (starts(h, "\x1a\x45\xdf\xa3")) return item_kind::media;  // Matroska / WebM
  if (const item_kind k = iso_brand(h); k != item_kind::unknown) return k;
  if (h[0] == 0x47 && (h.size() < 189 || h[188] == 0x47)) return item_kind::media;  // MPEG-TS
  if (starts(h, "ID3")) return item_kind::audio;
  if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x06) != 0) return item_kind::audio;  // an MPEG audio frame
  if (starts(h, "fLaC") || starts(h, "OggS")) return item_kind::audio;
  return item_kind::unknown;
}

}  // namespace mv::enhance
