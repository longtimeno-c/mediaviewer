// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Check library (docs/plans/check-library.md): what a source already has in a
// library folder, by content alone -- size first, BLAKE3-256 only where a size
// matches, never the name (docs/design/18 "Duplicate test").
//
// The library side is import.db's `library` table: the destination is walked
// for sizes and mtimes, a file is read only when its size is one a source file
// has (or another library file has: the duplicates list), and each hash is kept
// under the destination's volume key (dest_locator). A second check of an
// unchanged library reads no file. A row is dropped only when its folder
// answers and the file is not in it (docs/design/18 "Offline destinations").
//
// Report only: nothing here writes to a source or a library file, or deletes
// one. I/O threads only (rule 1); hashing yields to the present loop through
// the caller's hooks. Paths, names and hashes stay on the machine (rule 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addons/import/host.h"
#include "addons/import/library_index.h"
#include "addons/import/model.h"
#include "core/result.h"

namespace mv::import {

struct check_request {
  std::string destination;  // the library folder (normalize_root'ed)
  dup_scope scope = dup_scope::destination;
  // A folder source: the add-on walks and hashes it itself.
  std::string source_folder;
  // A host source (the Mac's Photos library): only the sizes cross; the host
  // hashes the bytes it has and asks library_find.
  std::vector<std::uint64_t> source_sizes;
  // Files the caller's own record says are in the destination (the Photos
  // backup's manifest), '/'-separated under it: which are gone, and whether
  // their bytes are somewhere else in the library now (moved, not deleted).
  struct expected_file {
    std::string rel;
    std::uint64_t size = 0;
    std::optional<digest> hash;
  };
  std::vector<expected_file> expected;
  // In Past imports with a report file (a check someone ran), or not (the
  // backup's own pass before it copies).
  bool history = true;
};

// Strict: a mistyped known key fails; unknown keys are ignored.
//   {"destination": "...", "scope": "destination"|"library",
//    "source_folder": "...", "source_sizes": [n, ...],
//    "expected": [{"rel": "...", "size": n, "hash": "<64 hex>"}], "history": bool}
[[nodiscard]] result<check_request> parse_check_request(std::string_view json_text);

struct check_found {
  std::string path;                // the source file
  std::uint64_t size = 0;
  std::vector<std::string> where;  // every library file with its bytes, absolute
};
struct check_group {
  std::uint64_t size = 0;
  std::vector<std::string> paths;  // absolute, sorted
};
struct check_gone {
  std::string rel;
  std::string moved_to;  // absolute; empty: not anywhere in scope (deleted)
};

enum class check_phase : std::uint8_t { walking = 0, hashing_library = 1, hashing_source = 2 };

struct check_result {
  std::string destination;
  bool reachable = false;      // the destination folder answered
  bool walk_complete = false;  // and was walked to the end
  bool cancelled = false;
  std::uint64_t library_files = 0;
  std::uint64_t library_bytes = 0;
  std::uint32_t hashed = 0;      // library files read now
  std::uint32_t remembered = 0;  // library files whose hash the index already had
  std::uint64_t hashed_bytes = 0;
  std::uint32_t dropped = 0;     // index rows of files no longer there
  // A folder source.
  std::uint64_t source_files = 0;
  std::uint64_t source_bytes = 0;
  std::vector<check_found> found;
  std::vector<std::string> not_found;
  std::uint64_t not_found_bytes = 0;
  std::vector<std::string> unreadable;  // either side
  // A host source: the sizes that have at least one indexed file in scope.
  std::vector<std::uint64_t> matched_sizes;
  std::vector<check_group> duplicates;  // inside the destination, largest waste first
  std::vector<check_gone> gone;         // of `expected`, only when walk_complete
};

struct check_hooks {
  std::function<bool()> cancelled;
  // The job's own flag, read directly by long hashes inside lookups (a
  // re-hash of a changed file elsewhere), so a cancel reaches them at once.
  const std::atomic<bool>* cancel_flag = nullptr;
  std::function<void()> yield;
  // Throttled by the caller. `current` is for the person's own screen only.
  std::function<void(check_phase, std::uint64_t done, std::uint64_t total, std::uint64_t bytes,
                     const std::string& current)>
      progress;
};

// [worker] The whole check. The index is updated as it goes (hashes kept even
// on cancel).
[[nodiscard]] check_result run_library_check(const host& h, library_index& idx,
                                             const dest_locator& loc, const check_request& req,
                                             const check_hooks& hooks);

// The summary JSON (`full`: every list; otherwise counts only, for history).
[[nodiscard]] std::string check_json(const check_result& r, std::uint64_t job_id,
                                     std::int64_t elapsed_ms, bool full);
// The person's local report.
[[nodiscard]] std::string check_report_text(const check_result& r);

// ---- one file at a time, for a host source (the Photos backup) ----

struct find_request {
  std::string destination;
  dup_scope scope = dup_scope::destination;
  std::uint64_t size = 0;
  std::optional<digest> hash;  // absent: only count the candidates
  std::string exclude;         // a path that is never an answer (the file itself)
};
//   {"destination","scope","size", "hash"?, "exclude"?}
[[nodiscard]] result<find_request> parse_find_request(std::string_view json_text);

struct find_answer {
  std::uint32_t candidates = 0;    // indexed files of this size in scope
  std::vector<std::string> paths;  // with this hash, confirmed on disk now
};
// [worker] Rows of the size in scope; with a hash, each candidate that matches
// is confirmed where it is now (a stat; re-read once if it changed). An offline
// destination confirms nothing and keeps its rows.
[[nodiscard]] find_answer library_find(const host& h, library_index& idx, const dest_locator& loc,
                                       const find_request& req, const std::atomic<bool>& cancel);
[[nodiscard]] std::string find_json(const find_answer& a);

// [worker] A file a host tool just wrote and verified under `destination`
// joins the index, so Import (and the next check) knows it without a read.
//   {"destination","path","size","mtime","hash"}
[[nodiscard]] expected library_record(const host& h, library_index& idx, const dest_locator& loc,
                                      std::string_view json_text);

}  // namespace mv::import
