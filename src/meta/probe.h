// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345: the provenance probe behind the canvas's AI badge (docs/design/06 "Made with
// and AI provenance", "The AI badge").
//
// The badge needs summary::ai_generated for whatever is on screen, pane open or
// not, and the full read (meta::read: Exiv2 over an 8 MB prefix, 17-20 ms for a
// 6 MP JPEG) is too much to spend on every item a person settles on. This reads
// only the bytes provenance lives in and runs the same readers and the same rule
// (generators.h derive_provenance) over them:
//
//   JPEG        the header segments up to the first scan, by marker walk:
//               APP11 C2PA, APP1 XMP (CreatorTool, DigitalSourceType) and APP1
//               EXIF IFD0 Software
//   PNG         every chunk but the image data (IDAT is skipped by seeking):
//               caBX, tEXt / zTXt / iTXt (the XMP packet included), eXIf
//   WebP        the EXIF and XMP chunks
//   TIFF        IFD0 Software and XMP (tag 700)
//   ISO-BMFF    the top-level C2PA box (HEIF, AVIF, MP4, MOV), by box headers
//
// Typically a few kilobytes and well under a millisecond from the page cache. It
// is the full read's answer for every field it covers (tested fixture by
// fixture). What it does not read -- a HEIF's XMP item, a clip's QuickTime
// software tag, RAW -- the full read still does, and a host that holds the full
// record uses it instead.
//
// Portable (D9): io/ for the reads, no platform header. Never throws; a file it
// cannot read has no provenance, which is not "not AI".
#pragma once

#include <string_view>

#include "meta/generators.h"

namespace mv::meta {

[[nodiscard]] provenance probe_provenance(std::string_view utf8_path) noexcept;

}  // namespace mv::meta
