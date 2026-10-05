// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12 — metadata write (docs/design/06 "Writing"): rating, orientation and user
// comment. PR 29 (owner, 2026-09-26; docs/design/12): any EXIF / IPTC / XMP tag can
// be set or removed, and the capture date set across every tag that holds
// it -- through the same checked rewrite and the same sidecar rule. Shared by
// both hosts; portable (no Win32 or Cocoa, D9).
//
// Where a change lands:
//   * A plain JPEG is edited in place through Exiv2's parsed structure, so
//     every tag the writer does not touch — maker notes included — is carried
//     over, and the result is swapped in through the io replace port
//     (`ReplaceFileW` / same-directory temp + `F_FULLFSYNC` + `rename`).
//     Nothing is trusted: the new bytes are re-read and checked against the
//     old ones (untouched tags equal, thumbnail equal, every non-metadata JPEG
//     segment identical) *before* anything is replaced. A mismatch is an
//     error and the original is never touched.
//   * Everything else — camera RAW, HEIC/AVIF, TIFF, PNG, WebP, BMP/GIF/ICO,
//     video, and a JPEG that in-place editing would harm (a Multi-Picture
//     "MPF" table, whose offsets a resized header would break, or bytes after
//     the final EOI, as a motion photo has) — gets an XMP sidecar
//     (`IMG_1234.xmp`) next to it. The original is never opened for writing
//     (rule 5). Any existing sidecar is merged, not replaced.
//   * Before the first write to a file in a session its metadata is
//     snapshotted to a local store -- the three fields, and (PR 29) a JPEG's
//     metadata segments byte for byte plus the sidecar as it was -- so
//     `revert` puts every tag back, not just the three. Nothing here logs a
//     path (rule 6) or leaves the machine.
//
// Worker threads only: reads and rewrites a whole file (rule 1).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::meta {

// One field of a write: leave it, set it, or remove it.
template <typename T>
struct change {
  enum class kind : std::uint8_t { keep, set, clear };
  kind k = kind::keep;
  T value{};

  [[nodiscard]] static change to(T v) { return change{kind::set, std::move(v)}; }
  [[nodiscard]] static change remove() { return change{kind::clear, T{}}; }
  [[nodiscard]] bool touches() const noexcept { return k != kind::keep; }
};

inline constexpr int kMaxRating = 5;
inline constexpr std::size_t kMaxCommentBytes = 4096;
inline constexpr std::size_t kMaxTagValueBytes = 64 * 1024;
inline constexpr std::size_t kMaxTagEdits = 512;

// PR 29: one tag, by its full Exiv2 key ("Exif.Photo.DateTimeOriginal",
// "Iptc.Application2.Keywords", "Xmp.dc.title"). `set` takes Exiv2's string
// form -- what the full tree shows as the raw value ("2024:05:01 14:03:22",
// "1/250", "a, b" for an XMP bag). `remove()` deletes every value the key has.
struct tag_edit {
  std::string key;
  change<std::string> value;
};

// Who may change a tag, for a file whose writes land at `target`.
enum class tag_access : std::uint8_t {
  editable,     // set and remove
  via_sidecar,  // set only: the value goes to the XMP sidecar under its XMP
                // name; the original (a RAW, a HEIC, a clip) keeps its own
  read_only,    // structure (sizes, offsets, strips), a maker note, a
                // computed or container row: shown, never written
};

struct write_fields {
  // 1..5 stars; -1 is XMP's "rejected". 0 and `remove()` both remove every
  // rating tag, so "unrated" is the absence of one, not a stored zero.
  change<int> rating;
  change<int> orientation;      // EXIF 1..8
  change<std::string> comment;  // UTF-8, no NUL, at most kMaxCommentBytes; "" removes
  // PR 29. Any tag the file may take (access_of), applied after the three
  // fields above; a later edit of a key replaces an earlier one.
  std::vector<tag_edit> tags;
  // PR 29. "YYYY-MM-DD HH:MM:SS": every capture-time tag the file carries
  // (EXIF DateTimeOriginal -- always -- and DateTimeDigitized; XMP
  // exif:DateTimeOriginal, xmp:CreateDate, photoshop:DateCreated), so no
  // reader sees two dates. `remove()` deletes them all.
  change<std::string> date_taken;

  [[nodiscard]] bool empty() const noexcept {
    return !rating.touches() && !orientation.touches() && !comment.touches() && tags.empty() &&
           !date_taken.touches();
  }
};

enum class write_target : std::uint8_t { in_file, sidecar };

// PR 29: what a write to `key` may do on a file whose writes land at `target`
// (write_target_for). Pure: decided by the key, not by the file.
[[nodiscard]] tag_access access_of(std::string_view key, write_target target) noexcept;

// PR 29: "2024-05-01 14:03:22" (or "...T..." ) -> the EXIF form
// "2024:05:01 14:03:22" / the XMP form "2024-05-01T14:03:22". False for
// anything that is not a real date and time.
[[nodiscard]] bool exif_date_of(std::string_view stamp, std::string& exif_out, std::string& xmp_out);

struct write_outcome {
  write_target target = write_target::in_file;
  // The sidecar the change went to, when there is one. An in-file write also
  // reports the sidecar it kept in agreement, if the file had one.
  std::string sidecar_path;
  bool sidecar_touched = false;
};

// Where a change to `utf8_path` will land, by magic bytes and structure, never
// by extension. `status::io` when the file cannot be read.
[[nodiscard]] result<write_target> write_target_for(std::string_view utf8_path);

// "IMG_1234.CR2" -> "IMG_1234.xmp", beside the file (docs/design/06). A name with no
// extension gets ".xmp" appended.
[[nodiscard]] std::string sidecar_path_for(std::string_view utf8_path);

// Applies `fields`. `snapshot_dir` is where "revert metadata" data lives (a
// directory the host owns; created if missing). Empty = no snapshot, for
// tests and tools; a host always passes one. If a snapshot is due and cannot
// be written, nothing is written to the file.
//
// Errors: invalid_arg (a value out of range, a comment that is not UTF-8, a
// field this file cannot hold), io (unreadable / not replaceable / changed
// under us), corrupt (a JPEG Exiv2 cannot parse), internal (the post-write
// check failed: the original is intact).
[[nodiscard]] result<write_outcome> write(std::string_view utf8_path, const write_fields& fields,
                                          std::string_view snapshot_dir = {});

// Puts the three fields back to what they were before this session's first
// write to the file. `status::io` when there is no snapshot.
[[nodiscard]] result<write_outcome> revert(std::string_view utf8_path,
                                           std::string_view snapshot_dir);
[[nodiscard]] bool has_snapshot(std::string_view utf8_path, std::string_view snapshot_dir);

// Tests only: forget which files have already been snapshotted this session.
void reset_snapshot_session();

// "★★★☆☆" for 1..5, "Rejected" for -1, "" for 0 (see write_fields).
[[nodiscard]] std::string format_rating(int rating);

}  // namespace mv::meta
