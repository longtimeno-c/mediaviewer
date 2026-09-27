// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 10 export (plan/07 "Export"): the edit stack baked at full resolution
// into a *new* file (rule 5 — the original is never the target), with a
// metadata preservation policy.
//
// Two paths, chosen here and reported back so the chrome can say which ran:
//   lossless  — JPEG in, JPEG out, only rotate / flip / an MCU-aligned crop:
//               the DCT coefficients are rearranged (lossless_jpeg.h).
//   re-encode — everything else: decode, geometry on the CPU (geometry.h),
//               encode (encode.h).
//   bake      — PR 11: any colour adjust: the FP16 working image
//               (image::decode_linear — for a RAW, LibRaw's linear develop),
//               geometry and the colour kernel at full resolution
//               (edit/bake.h), encoded as sRGB.
// Either way the output is upright: EXIF Orientation = 1, EXIF pixel
// dimensions = the written size, tiff:Orientation in XMP = 1.
//
// Worker thread only. Pure bytes in, bytes out: the host reads the source and
// writes the result (io::write_new), so this is testable without a disk.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "edit/adjust.h"
#include "edit/edit_stack.h"
#include "edit/encode.h"
#include "edit/metadata_policy.h"

namespace mv::edit {

struct export_options {
  encode_options encode{};
  metadata_policy policy = metadata_policy::all;
  // Take the lossless path whenever it applies (plan/07: "Offer this whenever
  // the requested edit stack contains only those ops"). Off forces a re-encode.
  bool prefer_lossless = true;
  // The export dialog's size: the long edge in pixels, applied on top of the
  // stack (it replaces a resize op). 0 = the stack's own size.
  std::uint32_t long_edge = 0;
};

struct export_result {
  std::vector<std::uint8_t> bytes;
  bool lossless = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// `g` is relative to what the viewer displays (the decoded, oriented image).
// `carried` is the metadata of a source this module cannot read itself (HEIC,
// TIFF, RAW, WebP: meta::read_carried, which the host runs — edit/ and meta/
// are siblings). Ignored for JPEG and PNG sources, which are read here.
[[nodiscard]] result<export_result> export_image(std::span<const std::uint8_t> source,
                                                 const geometry& g, const export_options& opt,
                                                 const job_context* ctx = nullptr,
                                                 const metadata_blobs* carried = nullptr);
// PR 11: with colour adjusts. An identity `c` is exactly the overload above.
[[nodiscard]] result<export_result> export_image(std::span<const std::uint8_t> source,
                                                 const geometry& g, const colour& c,
                                                 const export_options& opt,
                                                 const job_context* ctx = nullptr,
                                                 const metadata_blobs* carried = nullptr);

// The metadata an export carries for `source` under `policy`, before the
// orientation / dimension patch. JPEG (APP1) and PNG (eXIf, iTXt XMP) are read
// here; any other source uses `carried` when given. The policy applies to
// both.
[[nodiscard]] metadata_blobs source_metadata(std::span<const std::uint8_t> source,
                                             metadata_policy policy,
                                             const metadata_blobs* carried = nullptr);

// "IMG_0001.HEIC" → "IMG_0001-edit.jpg". The host runs it through
// io::unique_name, so an existing export is never overwritten either.
[[nodiscard]] std::string export_file_name(std::string_view source_name, image_format format);

}  // namespace mv::edit
