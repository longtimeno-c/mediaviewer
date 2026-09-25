// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12 — metadata write (plan/06 "Writing"): rating, orientation and user
// comment, and nothing else. Shared by both hosts; portable (no Win32 or
// Cocoa, D9).
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
//   * Before the first write to a file in a session the prior value of the
//     three fields is snapshotted to a local store, so `revert` is always
//     available. Nothing here logs a path (rule 6) or leaves the machine.
//
// Worker threads only: reads and rewrites a whole file (rule 1).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

struct write_fields {
  // 1..5 stars; -1 is XMP's "rejected". 0 and `remove()` both remove every
  // rating tag, so "unrated" is the absence of one, not a stored zero.
  change<int> rating;
  change<int> orientation;      // EXIF 1..8
  change<std::string> comment;  // UTF-8, no NUL, at most kMaxCommentBytes; "" removes

  [[nodiscard]] bool empty() const noexcept {
    return !rating.touches() && !orientation.touches() && !comment.touches();
  }
};

enum class write_target : std::uint8_t { in_file, sidecar };

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

// "IMG_1234.CR2" -> "IMG_1234.xmp", beside the file (plan/06). A name with no
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
