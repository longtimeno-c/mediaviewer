// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Export encoders (docs/design/07 "Export"): JPEG through libjpeg-turbo, PNG through
// libspng — the same libraries the decoders already link, so export adds no
// dependency. Worker thread only.
//
// The raster is written in the colour space it arrived in, with its ICC
// profile embedded unchanged: a geometry export does not colour-convert.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "codec/raster.h"
#include "core/result.h"
#include "edit/adjust_size.h"

namespace mv::edit {

enum class image_format : std::uint8_t { jpeg = 0, png = 1 };
enum class chroma : std::uint8_t { s420 = 0, s422 = 1, s444 = 2 };

struct encode_options {
  image_format format = image_format::jpeg;
  int quality = 92;              // JPEG 1..100
  chroma subsampling = chroma::s420;
  // #288: the resolution the file states. JPEG: the JFIF density (dots/inch,
  // or dots/cm when given per cm; 16-bit, so a fractional ppi rounds). PNG:
  // pHYs in pixels per metre. Not stated: JFIF says aspect 1:1 with no unit
  // (libjpeg's default) and PNG has no pHYs. Written under every metadata
  // policy: it is part of the picture's size, like the ICC profile.
  resolution stated{};
};

// Metadata already filtered by the export policy. `exif` is a TIFF block
// (what follows "Exif\0\0"); `xmp` is the packet text.
struct metadata_blobs {
  std::vector<std::uint8_t> exif;
  std::vector<std::uint8_t> xmp;
};

// JPEG: alpha is flattened over white (JPEG has none). An EXIF or XMP block
// too large for one APP1 segment is left out rather than split.
// PNG: RGB when every pixel is opaque, RGBA otherwise; EXIF as eXIf, XMP as
// the iTXt "XML:com.adobe.xmp" chunk, ICC as iCCP.
[[nodiscard]] result<std::vector<std::uint8_t>> encode(const codec::raster& img,
                                                       const encode_options& opt,
                                                       const metadata_blobs& meta);

}  // namespace mv::edit
