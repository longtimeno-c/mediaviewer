// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345: the provenance probe behind the canvas's AI badge (docs/design/06 "Made with
// and AI provenance", "The AI badge").
//
// The badge needs summary::ai_generated for whatever is on screen, pane open or
// not, and the full read (meta::read: Exiv2 over an 8 MB prefix, 7-20 ms for a
// 6 MP JPEG) is too much to spend on every item a person settles on. This reads
// only the bytes provenance lives in, by seeking, and runs the same readers and
// the same rule (generators.h derive_provenance) over them:
//
//   JPEG        APP11 C2PA, APP1 XMP, APP1 EXIF IFD0 Software; other segments
//               are stepped over unread, and the scan is never reached
//   PNG         caBX, tEXt / zTXt / iTXt (the XMP packet included), eXIf;
//               IDAT is skipped by seeking
//   WebP        the EXIF and XMP chunks
//   TIFF, RAW   IFD0 Software and XMP (tag 700): TIFF, DNG, CR2, NEF, ARW, PEF,
//               ORF ("IIRO" / "IIRS"), RW2 ("IIU\0"); RAF through its embedded
//               JPEG's header; CR3 through moov's Canon box (CMT1 = IFD0) and
//               the XMP uuid
//   HEIF, AVIF  the top-level C2PA box; the `meta` box's Exif item and its
//               `application/rdf+xml` mime item, through iinf / iloc / idat
//   MP4, MOV    the top-level C2PA box and XMP uuid; moov's udta / meta, found by
//               seeking past the other children: (C)swr, (C)too, the
//               com.apple.quicktime.software key, udta/XMP_
//
// Each extent it reads is capped (1 MB XMP, 256 KB EXIF, 1 MB meta / udta), the
// first instance of each kind is the one kept, and every byte read from the file
// counts against one budget (a largest C2PA box plus 1 MB), so no layout makes it
// a whole-file read; a file that would need more is "unknown" -- no badge. It is
// the full read's answer for every field it reads (tests hold the two to each
// other fixture by fixture). An XMP sidecar is not read: sidecars beside a file
// are the full read's (and MediaViewer only writes ratings and edits there).
//
// Portable (D9): io/ for the reads, no platform header. Never throws; a file it
// cannot read has no provenance, which is not "not AI".
#pragma once

#include <string_view>

#include "meta/generators.h"
#include "meta/parse.h"

namespace mv::meta {

[[nodiscard]] provenance probe_provenance(std::string_view utf8_path) noexcept;
// The same over any source (the fuzz harness and tests).
[[nodiscard]] provenance probe_provenance(parse::byte_source& src) noexcept;
// What the probe collects, before the rule: the full read merges it for the
// fields its own readers do not reach (a clip's XMP), so the two agree.
void probe_evidence(parse::byte_source& src, parse::evidence& ev) noexcept;

}  // namespace mv::meta
