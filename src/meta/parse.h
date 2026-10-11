// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: the readers for what Exiv2 does not surface (docs/design/06 "Beyond Exiv2").
//
//   C2PA / Content Credentials   JPEG APP11 JUMBF (multi-segment), PNG caBX,
//                                ISO-BMFF uuid / c2pa box (HEIF, AVIF, MP4, MOV)
//   PNG text                     every tEXt, zTXt and iTXt chunk (compressed or not)
//   XMP outside Exiv2            properties of the packet Exiv2 did not list, raw
//   ICC profile                  description, copyright and header
//   Containers                   JFIF, JPEG comment, WebP chunks, HEIF items, PNG pHYs
//
// Portable and pure: bytes in, rows out. No Exiv2, no FFmpeg, no platform header
// (D9), so each reader has a libFuzzer harness (tools/fuzz/fuzz_meta_*.cpp) that
// builds without the rest of meta. Every read is bounds-checked against the span
// it was given; every loop, nesting level, decompression and row count is capped,
// so a hostile file costs at most a few megabytes and a few milliseconds. Nothing
// is validated (a C2PA signature is read, never checked) and nothing touches the
// network (rule 6). Never throws: an allocation failure stops a reader where it is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "meta/meta.h"

namespace mv::meta::parse {

// The facts provenance is decided from (generators.h `derive_provenance`). The
// readers only collect; nothing here decides that a file is AI-made.
struct evidence {
  std::string c2pa_generator;               // claim_generator_info name [version], else claim_generator
  std::vector<std::string> c2pa_agents;     // actions' softwareAgent names
  std::vector<std::string> source_types;    // digitalSourceType values: C2PA actions, XMP Iptc4xmpExt
  std::string c2pa_signer;                  // the signing certificate's subject O, else CN
  bool png_parameters = false;              // a PNG `parameters` text chunk (AUTOMATIC1111 / SD)
  bool png_comfyui = false;                 // a PNG `prompt` / `workflow` chunk holding JSON
  std::string png_software;                 // PNG `Software`
  std::vector<std::string> png_generator_text;  // other PNG text naming a tool (`Generator`, `Source`, `parameters` tail)
  std::string xmp_creator_tool;             // xmp:CreatorTool
  std::string exif_software;                // EXIF Software
};

// Where the readers put what they find. `max_rows` bounds `rows` as a whole
// (the read model's cap), so a reader stops adding once it is reached.
struct sink {
  std::vector<property>& rows;
  evidence& ev;
  std::size_t max_rows = 4000;

  [[nodiscard]] bool full() const noexcept { return rows.size() >= max_rows; }
  // Adds one row; values are cleaned to valid UTF-8 and capped. False when full.
  bool add(origin space, std::string_view group, std::string_view name, std::string_view label,
           std::string_view value, std::string_view raw_tag, std::size_t cap_chars = 512);
};

// Random access for the ISO-BMFF walk: a clip's c2pa box can sit after a
// multi-gigabyte mdat, so the file is walked by box headers, not read whole.
class byte_source {
 public:
  virtual ~byte_source() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  // Fills `into` from `offset`; returns the bytes read (short only at the end).
  [[nodiscard]] virtual std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept = 0;
};

class memory_source final : public byte_source {
 public:
  explicit memory_source(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
  [[nodiscard]] std::uint64_t size() const noexcept override { return bytes_.size(); }
  [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept override;

 private:
  std::span<const std::uint8_t> bytes_;
};

// ---- Containers -------------------------------------------------------------

// JPEG: JFIF / JFXX, COM, Adobe APP14, and the APP11 JUMBF C2PA store (its
// segments reassembled in sequence order). `icc_out`, when given, receives
// nothing: the ICC profile comes from Exiv2, which already reassembles APP2.
void read_jpeg(std::span<const std::uint8_t> jpeg, sink& out);

// PNG: tEXt / zTXt / iTXt (all of them; XMP and Exiv2's "Raw profile" chunks
// are left to Exiv2), caBX (C2PA), pHYs, sRGB, gAMA.
void read_png(std::span<const std::uint8_t> png, sink& out);

// WebP: VP8X features and canvas, ANIM loop and frame count, the bitstream kind.
void read_webp(std::span<const std::uint8_t> webp, sink& out);

// ISO-BMFF (HEIF, AVIF, MP4, MOV): ftyp brands; a top-level meta box's items,
// references and properties (HEIF/AVIF); and the C2PA uuid / c2pa box wherever
// it sits at the top level. `stills` = false skips the meta box (a clip's
// metadata is libavformat's).
void read_bmff(byte_source& file, bool stills, sink& out);

// ---- Payloads ---------------------------------------------------------------

// A C2PA manifest store: one JUMBF superbox labelled "c2pa". Curated rows for
// the active (last) manifest, then every assertion flattened to key paths.
void read_c2pa(std::span<const std::uint8_t> jumbf, sink& out);

// The JUMBF store inside a JPEG's APP11 segments, reassembled; empty when none.
[[nodiscard]] std::vector<std::uint8_t> jpeg_jumbf(std::span<const std::uint8_t> jpeg);

// An ICC profile: header, desc, cprt, dmnd, dmdd. Returns the description ("" when
// it has none) so the summary can name the profile.
std::string read_icc(std::span<const std::uint8_t> icc, sink& out);

// XMP: every leaf property of the packet as an origin::xmp_raw row keyed
// "Xmp.<prefix>.<path>" (the packet's own prefix; paths in Exiv2's form,
// "History[2]/stEvt:action"), for which `listed(ctx, ns_uri, path)` is false:
// the caller asks Exiv2 whether it already listed that property under the
// namespace's URI. Null `listed` lists everything. Also collects
// xmp:CreatorTool and Iptc4xmpExt:DigitalSourceType into the evidence, listed or not.
using listed_fn = bool (*)(void* ctx, std::string_view ns_uri, std::string_view path);
void read_xmp(std::string_view packet, listed_fn listed, void* ctx, sink& out);

// ---- Building blocks, exposed for the tests -----------------------------------

// zlib (RFC 1950) inflate into at most `max_out` bytes. False on damage or a
// result that would exceed the cap (`out` then holds what fit).
bool inflate_bounded(std::span<const std::uint8_t> in, std::size_t max_out, std::string& out);

// Text in a PNG tEXt / zTXt is Latin-1 by the spec; tools write UTF-8 there too.
// Valid UTF-8 is kept, anything else is read as Latin-1.
[[nodiscard]] std::string latin1_or_utf8(std::string_view bytes);

// Valid UTF-8 out, control characters other than tab / newline as '?', capped
// at `cap_chars` bytes on a character boundary with an ellipsis.
[[nodiscard]] std::string clean_text(std::string_view bytes, std::size_t cap_chars);

}  // namespace mv::meta::parse
