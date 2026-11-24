// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keep a copy, slice 2 (docs/plans/self-host-drives-keep-a-copy.md §3–§5): the
// copier that runs drive_copy.h's plan on real folders. One pass lists the
// source and the copy, plans, and carries the plan out:
//
//   copy        to "name.mvpart" beside it, the source's modified time set on
//               it, then renamed into place: a crash or an unplugged drive
//               never leaves half a file under the real name.
//   to the bin  a file gone from the source, or saved over there, is moved
//               into the copy's Recycle Bin (`.mediaviewer-copy/bin/<id>`),
//               never deleted; bin.json says where it was.
//   expiry      bin items past their time are removed; the bin is trimmed,
//               oldest first, only when the copy would not fit otherwise.
//
// The source is only ever opened for reading. A folder that fails to list
// makes the pass take nothing to the bin as deleted (it may be there still).
// Portable (std::filesystem and io::list_children): the Mac and Windows run
// the same code. Nothing here logs a path or a name (rule 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/cli/drive_copy.h"

namespace mv::driftwire::cli::drive_copy {

// At the top of the copy drive: the marker (the guards refuse to erase a drive
// with one: it may be the only other copy), the copy's own store (its bin),
// and the copied files' folder.
inline constexpr std::string_view kMarkerName = ".mediaviewer-copy.json";
inline constexpr std::string_view kStoreName = ".mediaviewer-copy";
// "MediaViewer copy of Family Media".
[[nodiscard]] std::string copy_folder_name(std::string_view label);

// The OS's own folders at the top of a volume ("System Volume Information",
// "$RECYCLE.BIN", ".Spotlight-V100", ...) and the copy's own: never copied.
[[nodiscard]] bool skipped_at_top(std::string_view name) noexcept;

struct marker {
  std::string label;            // the source's name
  std::int64_t complete_at = 0; // the last pass that left nothing behind (0: not yet)
};
[[nodiscard]] std::string marker_json(const marker& m);
[[nodiscard]] std::optional<marker> marker_from_json(std::string_view text);
[[nodiscard]] bool has_marker(const std::string& mount);
[[nodiscard]] bool write_marker(const std::string& mount, const marker& m);
[[nodiscard]] std::optional<marker> read_marker(const std::string& mount);
void remove_marker(const std::string& mount);

// Every file and folder under `root` ('/'-separated, relative); links are left
// out. `top_skips`: skipped_at_top() names at the top are too. `incomplete` is
// set when a folder could not be listed.
[[nodiscard]] std::vector<entry> list_tree(const std::string& root, bool top_skips, bool& incomplete,
                                           const std::atomic<bool>* stop = nullptr);

struct pass_options {
  std::string source;      // the source volume's root ("E:\\", "/Volumes/Family Media")
  std::string copy_mount;  // the copy drive's root
  std::string label;       // the source's name: the copy's folder is named after it
  const std::vector<entry>* previous = nullptr;  // the source's listing from the pass before
  std::int64_t now = 0;
  std::int64_t settle_seconds = 10;
  keep_for keep = kDefaultKeep;
  const std::atomic<bool>* stop = nullptr;  // the keeper stopping: the pass ends between chunks
  // The tests: called after each chunk written to a ".mvpart"; true fails that file there.
  std::function<bool(std::string_view path, std::uint64_t written)> fail_at;
  // Each file done (copied or failed): the files and bytes still to copy.
  std::function<void(std::uint64_t files_left, std::uint64_t bytes_left)> progress;
};

enum class pass_trouble : std::uint8_t { none, source_missing, copy_missing, copy_full };

struct pass_result {
  pass_trouble trouble = pass_trouble::none;
  bool stopped = false;
  std::uint64_t copied = 0, bytes_copied = 0, failed = 0, held = 0, to_bin = 0, expired = 0, trimmed = 0;
  std::uint64_t files_left = 0, bytes_left = 0;  // at the end: not copied (held, failed, or full)
  std::uint64_t needed = 0, free = 0;            // copy_full: bytes to copy, and free on the copy drive
  std::vector<entry> listing;                    // the source's, for the next pass
  bin bin_now;
  // Nothing left to copy, nothing failed, nothing held: the copy has everything.
  [[nodiscard]] bool complete() const noexcept {
    return trouble == pass_trouble::none && !stopped && failed == 0 && held == 0 && files_left == 0;
  }
};

[[nodiscard]] pass_result run_pass(const pass_options& o);

// The copy's bin (read for `host drives list` and the status line).
[[nodiscard]] bin read_bin(const std::string& copy_mount);

}  // namespace mv::driftwire::cli::drive_copy
