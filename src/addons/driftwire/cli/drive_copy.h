// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host drives copy` (docs/plans/self-host-drives-keep-a-copy.md):
// a second drive that keeps a copy of a full one, nothing on the full drive
// erased. This file is the pure part, tested on every OS; the copier and the
// keeper that run it come in later slices.
//
//   the planner   make_plan(): the source's listing and the copy's → what to
//                 copy, what goes to the copy's Recycle Bin, what is held back
//                 (still being written) or skipped (links).
//   the bin       bin_item / bin, read and written as bin.json; expired(),
//                 trim_for_room(), restore_path(), bin_line().
//
// Paths here are relative to the copied root, '/'-separated. Nothing here logs
// a path or a name (rule 6): bin_line() gives counts and bytes only.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli::drive_copy {

// ---- the planner --------------------------------------------------------------------------

enum class entry_kind : std::uint8_t { file, folder, link };

struct entry {
  std::string path;          // "2024/Trip/IMG_0001.JPG"
  entry_kind kind = entry_kind::file;
  std::uint64_t size = 0;    // files only
  std::int64_t mtime = 0;    // seconds since 1970 (files only)
};

// The copier's half-written files ("name.mvpart"): never real files.
inline constexpr std::string_view kPartSuffix = ".mvpart";

enum class bin_reason : std::uint8_t { deleted, replaced };

enum class action_kind : std::uint8_t {
  copy,          // source → copy (new, or changed: a `to_bin` of the old one comes first)
  to_bin,        // the copy's file or folder → the Recycle Bin
  rename_case,   // the copy's name differs from the source's in case only: rename it
  drop_partial,  // a leftover "*.mvpart" of ours: delete it
  hold,          // still being written: next pass
  skip_link,     // a symlink or junction: listed, never followed
};

struct action {
  action_kind kind = action_kind::copy;
  std::string path;          // the source's spelling (rename_case: the new one)
  std::string from;          // rename_case: the copy's current spelling
  std::uint64_t size = 0;    // copy: bytes; to_bin: bytes moved (a folder: all its files)
  bin_reason reason = bin_reason::deleted;  // to_bin only
  bool folder = false;       // to_bin: a whole folder, one bin item
};

struct plan_input {
  std::span<const entry> source;
  std::span<const entry> copy;
  // The source's listing from the pass before, when there was one: a file whose
  // size or time moved since then is still being written.
  std::span<const entry> previous_source;
  bool has_previous = false;
  std::int64_t now = 0;               // seconds since 1970
  std::int64_t settle_seconds = 10;   // a file changed more recently than this waits
  bool fold_case = true;              // Windows and the Mac: "A.jpg" and "a.jpg" are one file
  std::uint64_t copy_free = 0;        // free bytes on the copy drive
};

struct plan {
  std::vector<action> actions;        // in the order to run them
  std::uint64_t bytes_to_copy = 0;
  std::uint64_t files_to_copy = 0;
  std::uint64_t held = 0;             // files held back this pass
  std::uint64_t links_skipped = 0;
  bool fits = true;                   // bytes_to_copy <= copy_free
};

[[nodiscard]] plan make_plan(const plan_input& in);

// ---- the Recycle Bin ----------------------------------------------------------------------

struct bin_item {
  std::string id;            // the item's name in the bin folder ("20261115-140203-1")
  std::string original;      // where it was, relative to the copied root
  std::uint64_t bytes = 0;
  std::int64_t removed_at = 0;  // seconds since 1970
  bin_reason reason = bin_reason::deleted;
  bool folder = false;
};

struct bin {
  std::vector<bin_item> items;
};

[[nodiscard]] std::string to_json(const bin& b);
// nullopt when the text is not a whole bin.json (cut short, not JSON, a field
// of the wrong type): the caller keeps the bin it had.
[[nodiscard]] std::optional<bin> bin_from_json(std::string_view text);

// A new item's id: its time (UTC) and the lowest sequence number not taken.
[[nodiscard]] std::string next_id(const bin& b, std::int64_t now);

enum class keep_for : std::uint8_t { days7, days30, days90, until_emptied };
inline constexpr keep_for kDefaultKeep = keep_for::days30;
inline constexpr double kDefaultBinShare = 0.10;  // trimmed for room only above this share of the drive

// The ids whose time is up, oldest first.
[[nodiscard]] std::vector<std::string> expired(const bin& b, keep_for keep, std::int64_t now);

// The ids to delete, oldest first, to bring the drive's free space up to
// `want_free`, but never taking the bin below `share` of the drive's size.
// Empty when there is room already or the bin is within its share.
[[nodiscard]] std::vector<std::string> trim_for_room(const bin& b, std::uint64_t drive_size,
                                                     std::uint64_t free_now, std::uint64_t want_free,
                                                     double share = kDefaultBinShare);

// Where a restored item goes: `original` when nothing is there, else
// "name (restored).ext", then "name (restored 2).ext", and so on (a folder
// keeps its whole name: "Trip.2024 (restored)"). Never a path that `exists`
// says is taken.
[[nodiscard]] std::string restore_path(std::string_view original, bool folder,
                                       const std::function<bool(std::string_view)>& exists);

// Whether a bin.json's id or original path is safe to act on: an id is
// letters, digits and '-'; an original is relative, '/'-separated, with no
// "..", "." or empty part, no '\\' and no ':'. bin_from_json() refuses a file
// with any other, so a changed bin.json can never point outside the bin or the
// copied root.
[[nodiscard]] bool safe_id(std::string_view id) noexcept;
[[nodiscard]] bool safe_relative(std::string_view path) noexcept;

// "Recycle Bin: 214 items, 3.2 GB" ("Recycle Bin: empty"); the Mac says Trash.
[[nodiscard]] std::string bin_line(const bin& b, bool mac);

}  // namespace mv::driftwire::cli::drive_copy
