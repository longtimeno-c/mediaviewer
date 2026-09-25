// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Not part of the meta API: the readers read() dispatches between, and the
// pieces the PR 12 writer shares with them.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "meta/meta.h"

namespace Exiv2 {
class ExifData;
class Image;
class XmpData;
}

namespace mv::meta::detail {

// UTF-8 in, valid UTF-8 out: an invalid byte or a control character other than
// tab / newline becomes '?'. Exiv2 hands back arbitrary bytes for ASCII tags.
[[nodiscard]] std::string sanitise_utf8(std::string s);

// The byte order of an EXIF block, which UNICODE (UCS-2) user comments are
// written in. `unknown` for a file with no EXIF (a sidecar).
enum class comment_order : std::uint8_t { unknown, little, big };
[[nodiscard]] comment_order comment_order_of(const Exiv2::Image& image) noexcept;

// EXIF UserComment (tag 0x9286) is an 8-byte character-set code and then the
// text: "ASCII\0\0\0", "UNICODE\0" (UCS-2 in the block's byte order), "JIS", or
// eight NULs for "undefined". Exiv2 hands a file's value back as opaque bytes,
// so both directions are done here, the same on every platform (no iconv).
// decode: UTF-8, never throws, "" for a code it cannot read.
[[nodiscard]] std::string decode_user_comment(std::span<const std::uint8_t> raw, comment_order order);
// encode: ASCII when the text is, else UNICODE in `order` (which must not be
// `unknown`). The text must be valid UTF-8.
[[nodiscard]] std::vector<std::uint8_t> encode_user_comment(const std::string& utf8, comment_order order);

// The three PR 12 fields as a file says them. nullopt = the file does not say.
struct field_state {
  std::optional<int> rating;  // -1 rejected, 0..5
  std::optional<int> orientation;
  std::optional<std::string> comment;
};
// XMP first for the rating (it is what this app and Lightroom write), then the
// EXIF Rating / RatingPercent tags Windows uses; the EXIF UserComment first for
// the comment, then XMP exif:UserComment. Orientation: EXIF, then XMP
// tiff:Orientation (a sidecar has only the latter). Never throws.
[[nodiscard]] field_state read_fields(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp,
                                      comment_order order = comment_order::unknown) noexcept;

// Initialises Exiv2's XMP parser once (not safe to race) and mutes its log.
// Idempotent; every entry point that touches Exiv2 calls it first.
void ensure_exiv2();

// Exiv2 over bytes in memory, metadata read. Initialises Exiv2's XMP parser
// once and mutes its log (its messages can name paths, rule 6). Null when
// Exiv2 does not know the format. Throws Exiv2's exceptions: callers catch.
[[nodiscard]] std::unique_ptr<Exiv2::Image> open_image(std::span<const std::uint8_t> bytes);

// Exiv2 over an in-memory prefix/whole file. Fills `out`; leaves it untouched
// on a file Exiv2 cannot parse. Never throws (Exiv2's exceptions stop here).
// `is_raw` is the decoder's own answer (codec::looks_like_raw) so that
// display_orientation matches what was actually drawn.
void read_still(std::span<const std::uint8_t> bytes, bool decoder_orients, metadata& out) noexcept;

// Date-taken only, for sort (a prefix is enough). nullopt when absent.
[[nodiscard]] std::optional<std::int64_t> still_date_key(std::span<const std::uint8_t> bytes) noexcept;

// libavformat: container facts, per-stream inspector, chapters, tags.
// Returns false when FFmpeg cannot open the file as a clip.
[[nodiscard]] bool read_clip(std::string_view utf8_path, metadata& out) noexcept;
[[nodiscard]] std::optional<std::int64_t> clip_date_key(std::string_view utf8_path) noexcept;

// Parses the XMP sidecar at `sidecar_utf8_path` into `xmp`. False when there is
// none, it cannot be read, or it is not XMP (`xmp` is then untouched). Never throws.
[[nodiscard]] bool load_sidecar(std::string_view sidecar_utf8_path, Exiv2::XmpData& xmp) noexcept;

// PR 12: the rating, comment and (for tree rows) properties held by the XMP
// sidecar beside `utf8_path` (plan/06), applied over what the file itself
// said. A missing or unparsable sidecar changes nothing. Never throws.
void overlay_sidecar(std::string_view utf8_path, metadata& out) noexcept;

}  // namespace mv::meta::detail
