// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9 — metadata read model (plan/06-metadata.md).
//
// One vocabulary for the UI: EXIF / IPTC / XMP from Exiv2, container and
// per-stream facts from libavformat, and a few computed rows, all normalised
// into `metadata`. Views on it: the summary card, the searchable full tree
// (`properties`), and the video stream inspector (`streams`).
//
// Reading is worker-thread work (rule 1) and happens once per opened item.
// Toggling the info overlay or the AF-point quads reads this struct, never
// the file (plan/16, PR 9 verify).
//
// Missing metadata is not an error: a PNG with no XMP, a BMP, a clip with no
// tags all come back as a `metadata` whose fields are empty. An error result
// means the file could not be read at all.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::meta {

enum class origin : std::uint8_t { exif, iptc, xmp, container, computed };

// One row of the full tree. `raw_tag` is always kept ("Exif.Photo.ExposureTime")
// so the origin of a value is never lost to normalisation (plan/06).
struct property {
  origin space = origin::exif;
  std::string group;    // "Exif.Photo", "Xmp.dc", "Container", "Video #0"
  std::string name;     // "ExposureTime"
  std::string label;    // "Exposure Time" — falls back to `name`
  std::string value;    // human form ("1/250 s")
  std::string raw;      // untranslated form ("1/250")
  std::string raw_tag;  // full key
};

struct summary {
  std::uint32_t width = 0;   // stored pixels, before any display rotation
  std::uint32_t height = 0;
  std::uint64_t file_size = 0;
  std::string format;        // "JPEG", "MP4 / QuickTime"
  std::string camera;        // "Canon EOS R5"
  std::string lens;
  std::string exposure;      // "1/250 s"
  std::string aperture;      // "f/2.8"
  std::string iso;           // "ISO 400"
  std::string focal_length;  // "85 mm"
  std::string date_taken;    // "2024-05-01 14:03:22"
  std::string gps;           // "48.85837° N, 2.29448° E"
  std::string colour_space;
  std::string duration;      // "1:23.4" (clips)
  std::string codec;         // primary video codec
  std::string bitrate;       // "12.4 Mb/s"
  // The date-taken sort key (plan/16): the stamp read as if it were UTC.
  // Camera stamps carry no zone, so this orders correctly within a folder and
  // is not a real instant. 0 = unknown.
  std::int64_t date_taken_key = 0;
  // EXIF orientation as stored (1..8, 1 when absent).
  std::uint8_t orientation = 1;
};

// An AF area in the stored pixel grid, normalised to 0..1 with a top-left
// origin. Draw it through `metadata::display_orientation`.
struct af_point {
  float x = 0, y = 0, w = 0, h = 0;
  bool in_focus = false;
};

struct field {
  std::string label;
  std::string value;
};

enum class stream_kind : std::uint8_t { video, audio, subtitle, data, attachment };

struct stream_info {
  int index = 0;
  stream_kind kind = stream_kind::data;
  std::string codec;
  std::vector<field> fields;
};

struct chapter {
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string title;
};

struct metadata {
  summary s;
  std::vector<property> properties;
  std::vector<stream_info> streams;
  std::vector<chapter> chapters;
  std::vector<af_point> af_points;
  // The EXIF orientation that the decoder has *already applied* to the pixels
  // on screen: RAW and (from PR 10) JPEG are decoded rotated, TIFF is not
  // (plan/04). AF quads
  // must be transformed by exactly this and no more.
  std::uint8_t display_orientation = 1;
  bool is_clip = false;
};

// Full read. Worker thread only. `status::io` when the file cannot be read;
// everything else (unknown format, no metadata, damaged tags) is an OK result
// with fewer fields.
[[nodiscard]] result<metadata> read(std::string_view utf8_path);

// The sort key alone: date taken, from a bounded prefix read, or nullopt.
// Cheap enough to run for every file in a folder on a worker (sort by date
// taken). Never throws, never reads the whole file.
[[nodiscard]] std::optional<std::int64_t> read_date_taken(std::string_view utf8_path);

// Pure helpers, exposed so the tests can pin them without a fixture file.
// "2024:05:01 14:03:22" (EXIF) or "2024-05-01T14:03:22[.fff][Z|+hh:mm]" (XMP,
// container) → the UTC-read key. nullopt if it is not a date.
[[nodiscard]] std::optional<std::int64_t> parse_date_key(std::string_view stamp) noexcept;
// EXIF GPS: degrees/minutes/seconds + ref → "48.85837° N".
[[nodiscard]] std::string format_coordinate(double degrees, char hemisphere);
// ---- PR 10: metadata an export carries ----------------------------------------

// EXIF as a TIFF block (what follows "Exif\0\0" in a JPEG APP1) and the XMP
// packet, for a re-encoded export of a still whose container is not JPEG or
// PNG (HEIC / AVIF, TIFF, camera RAW, WebP — edit/export.h reads those two
// itself). Built by Exiv2 from what it read, not copied: a TIFF's or a RAW's
// IFD0 describes *its* pixels (strips, tiles, compression, sub-images, the
// embedded thumbnail, DNG private data), and those tags are left out.
// Orientation is written as 1, since the pixels are exported upright. Maker
// notes are kept while the block still fits one JPEG APP1 segment, and dropped
// rather than the whole block when it would not. Empty fields when there is
// nothing to carry. Worker thread only. Never throws.
struct carried_metadata {
  std::vector<std::uint8_t> exif;
  std::vector<std::uint8_t> xmp;
};

[[nodiscard]] carried_metadata read_carried(std::span<const std::uint8_t> bytes) noexcept;

// ---- Presentation, shared by both hosts (pure; no I/O) ----------------------

// The summary card as label/value rows, in display order. Every row for the
// item's kind is present even when its value is empty — the card shows the
// gap ("—") instead of hiding it, so missing metadata reads as missing rather
// than as a broken pane (PR 9 verify).
[[nodiscard]] std::vector<field> summary_rows(const metadata& m);

// The three lines the on-canvas info overlay adds under the item line. An
// empty string means "nothing to say", and the host draws no line for it.
[[nodiscard]] std::string overlay_camera_line(const metadata& m);
[[nodiscard]] std::string overlay_exposure_line(const metadata& m);
[[nodiscard]] std::string overlay_date_line(const metadata& m);

// AF quads in the *displayed* image: the stored-grid boxes run through
// `display_orientation` (corners mapped and re-normalised into a box).
[[nodiscard]] std::vector<af_point> displayed_af_points(const metadata& m);

// "6000 \xC3\x97 4000", "1.2 MB".
[[nodiscard]] std::string format_dimensions(std::uint32_t w, std::uint32_t h);
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// Maps a normalised point from the stored grid to the displayed one for an
// EXIF orientation (1..8). Orientation 1 is the identity.
void orient_point(std::uint8_t orientation, float& x, float& y) noexcept;
// True when orientation swaps width and height (5..8).
[[nodiscard]] constexpr bool orientation_swaps(std::uint8_t o) noexcept { return o >= 5 && o <= 8; }

}  // namespace mv::meta
