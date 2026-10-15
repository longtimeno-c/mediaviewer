// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 — metadata read model (docs/design/06-metadata.md).
//
// One vocabulary for the UI: EXIF / IPTC / XMP from Exiv2, container and
// per-stream facts from libavformat, and a few computed rows, all normalised
// into `metadata`. Views on it: the summary card, the searchable full tree
// (`properties`), and the video stream inspector (`streams`).
//
// Reading is worker-thread work (rule 1) and happens once per opened item.
// Toggling the info overlay or the AF-point quads reads this struct, never
// the file (docs/design/16, PR 9 verify).
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
#include "io/pairing.h"

namespace mv::meta {

// #290 appended the last four (docs/design/06 "Beyond Exiv2"): rows read by
// meta/parse.h rather than Exiv2 or libavformat. All four are read-only.
//   c2pa      Content Credentials: the curated claim rows and every assertion
//   png_text  a PNG tEXt / zTXt / iTXt chunk
//   icc       the embedded ICC profile's header and text tags
//   xmp_raw   an XMP property Exiv2 did not list, keyed as the packet spells it
// JFIF, JPEG comments, WebP chunks and HEIF items are `container` rows.
enum class origin : std::uint8_t { exif, iptc, xmp, container, computed, c2pa, png_text, icc, xmp_raw };

// The table name of an origin ("exif", …, "png_text", "xmp_raw"), as the
// properties tables write it (tables.h).
[[nodiscard]] const char* origin_name(origin o) noexcept;

// #290: a position as numbers, WGS 84 degrees (north and east positive).
// `altitude` is metres above sea level (negative below), when the file says.
struct geo_point {
  double lat = 0, lon = 0;
  std::optional<double> altitude;
};

// One row of the full tree. `raw_tag` is always kept ("Exif.Photo.ExposureTime")
// so the origin of a value is never lost to normalisation (docs/design/06).
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
  // #290: the same position as numbers, from EXIF GPS, else XMP exif:GPS*
  // (DJI and other drones), else a clip's ISO 6709 location. nullopt = none.
  std::optional<geo_point> where;
  // #290 (generators.h): the generator or provenance tool the file names
  // ("ChatGPT", "Adobe Firefly", "Stable Diffusion", or a C2PA claim generator
  // such as "Adobe Photoshop 25.0"); empty when nothing says. A camera's or an
  // editor's Software is not "made with" unless the generator table knows it.
  std::string made_with;
  // True only on positive evidence: a C2PA / IPTC digitalSourceType of
  // trainedAlgorithmicMedia or compositeWithTrainedAlgorithmicMedia, or a
  // field naming a known AI generator. False means "not stated", never "not AI".
  bool ai_generated = false;
  // #345: why ai_generated is true, one line for a person (generators.h
  // provenance::evidence); empty when it is false.
  std::string ai_evidence;
  std::string colour_space;
  std::string duration;      // "1:23.4" (clips)
  std::string codec;         // primary video codec
  std::string bitrate;       // "12.4 Mb/s"
  // PR 15: the same clip facts as numbers, for a consumer that is not a
  // person (the Spotlight importer). 0 = unknown.
  double duration_seconds = 0;
  std::int64_t bitrate_bps = 0;
  int audio_channels = 0;        // first audio stream
  int audio_sample_rate = 0;     // Hz, first audio stream
  std::vector<std::string> codecs;  // every audio / video stream's codec, file order, no repeats
  // The date-taken sort key (docs/design/16): the stamp read as if it were UTC.
  // Camera stamps carry no zone, so this orders correctly within a folder and
  // is not a real instant. 0 = unknown.
  std::int64_t date_taken_key = 0;
  // EXIF orientation as stored (1..8, 1 when absent).
  std::uint8_t orientation = 1;
  // PR 12: what the file (or its XMP sidecar, which wins) says. 1..5 stars,
  // -1 rejected, 0 unrated (no tag). The comment is EXIF UserComment, else
  // XMP exif:UserComment, else empty; UTF-8, newlines kept (the pane's table
  // flattens them for display; the edit field reads this one).
  int rating = 0;
  std::string comment;
  // Issue #207: the colour label (XMP xmp:Label, the sidecar wins), as
  // written: "Red", "Yellow", "Green", "Blue", "Purple", or another
  // app's word. Empty = none.
  std::string label;
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
  // (docs/design/04). AF quads
  // must be transformed by exactly this and no more.
  std::uint8_t display_orientation = 1;
  bool is_clip = false;
  // PR 29: where a tag edit lands (write.h write_target_for): true = the file
  // itself (a plain JPEG), false = its XMP sidecar. The pane's editability
  // column (access_of) is decided from this without another read.
  bool writes_in_file = false;
  // #352: the file itself (not its XMP sidecar) holds a position. A sidecar
  // cannot remove that one (rule 5), so the Location row's remove needs it
  // (write.h location_rights_of).
  bool where_in_file = false;
};

// Full read. Worker thread only. `status::io` when the file cannot be read;
// everything else (unknown format, no metadata, damaged tags) is an OK result
// with fewer fields.
[[nodiscard]] result<metadata> read(std::string_view utf8_path);

// The sort key alone: date taken, from a bounded prefix read, or nullopt.
// Cheap enough to run for every file in a folder on a worker (sort by date
// taken). Never throws, never reads the whole file.
[[nodiscard]] std::optional<std::int64_t> read_date_taken(std::string_view utf8_path);

// Issue #207: the rating and colour label alone, for the cull filter (View >
// Show: Picks, Not rejected, a colour). The same bounded head read as
// read_date_taken (the whole 8 MB prefix only when the head does not parse),
// then the XMP sidecar, which wins, as in `read`. A clip or an unreadable file
// is its sidecar, or unrated and unlabelled. Worker only; never throws.
[[nodiscard]] io::cull_marks read_cull_marks(std::string_view utf8_path);

// Pure helpers, exposed so the tests can pin them without a fixture file.
// "2024:05:01 14:03:22" (EXIF) or "2024-05-01T14:03:22[.fff][Z|+hh:mm]" (XMP,
// container) → the UTC-read key. nullopt if it is not a date.
[[nodiscard]] std::optional<std::int64_t> parse_date_key(std::string_view stamp) noexcept;
// EXIF GPS: degrees/minutes/seconds + ref → "48.85837° N".
[[nodiscard]] std::string format_coordinate(double degrees, char hemisphere);
// #290: an XMP GPS coordinate, "DDD,MM,SSk" or "DDD,MM.mmk" (k = N S E W), or
// plain signed decimal degrees → signed degrees. nullopt when it is not one.
[[nodiscard]] std::optional<double> parse_xmp_coordinate(std::string_view text) noexcept;
// #290: ISO 6709 "+48.8584+002.2945+035.000/" (altitude optional) → a point.
[[nodiscard]] std::optional<geo_point> parse_iso6709(std::string_view text) noexcept;
// "48.85837° N, 2.29448° E" for a point.
[[nodiscard]] std::string format_point(const geo_point& p);

// #352 (owner, 2026-10-12): what a person typed in the Location editor. Both
// hosts call this (Mac bridge, Windows through the C ABI), so a position reads
// the same on each. Accepted, latitude first unless letters say otherwise:
//   decimal               "38.7979, -9.3907"   "38.7979 -9.3907"   "38.7979 N, 9.3907 W"
//                         "N 38.7979, W 9.3907"  "9.3907 W, 38.7979 N"
//   degrees, minutes[, s] "38°47′52″ N, 9°23′27″ W"   "38 47 52 N 9 23 27 W"
//                         "38°47.874' N, 9°23.45' W"  ASCII ' and " (or '') for ′ and ″
// A comma or semicolon separates the two; without one, N/S/E/W letters or a
// single space between two plain decimals do. Only the last number of a
// coordinate may have a fraction; minutes and seconds are under 60; latitude
// within ±90, longitude within ±180; a sign and S / W together are refused.
// Whitespace only is `blank` (the editor's "remove the location").
struct location_input {
  enum class kind : std::uint8_t { point, blank, invalid };
  kind k = kind::invalid;
  geo_point point;         // kind::point: lat / lon; altitude is never set
  const char* error = "";  // kind::invalid: one line for the field (static, UTF-8)
};
[[nodiscard]] location_input parse_location(std::string_view utf8) noexcept;
// The editor's starting text for a point: "38.7979, -9.3907" (6 decimals at
// most, trailing zeros dropped). parse_location reads it back to the same point.
[[nodiscard]] std::string format_location_input(const geo_point& p);
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
