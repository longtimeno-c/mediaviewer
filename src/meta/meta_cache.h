// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #291 — meta_cache.db: what a file's tags say, read once, ever
// (docs/design/06 "Search text"; docs/plans/metadata-search-and-place.md slice 4).
//
// One row per file, keyed by path and valid only while the file's size and
// mtime (and kSearchFactsVersion) are what the row says: a file rewritten in
// place is read again, and nothing needs to be told. It holds the search
// fields (search.h) and their fold, the date-taken key, the coordinates and
// the AI flag, so
//   - Ctrl/⌘+F and `/` match tag text without reading anything (tag_index.h),
//   - sort by date taken is instant on a folder seen before,
//   - the host table's location_of / locations_in (the Places add-on's search
//     provider, docs/design/25 §7) and the AI pack's tag_facts are served
//     from here.
//
// It lives next to the thumbnail cache (io::thumb_cache_dir()) and is a
// cache: an unreadable or foreign file is deleted and made again. Nothing in
// it leaves the machine (rule 6). Thread-safe: one connection, one mutex, no
// I/O of the user's files under it; callers batch writes.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "meta/search.h"

struct sqlite3;

namespace mv::meta {

struct cached_facts {
  std::string path;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  search_facts facts;
  std::string folded;  // folded_text(facts.fields)
};

class meta_cache {
 public:
  meta_cache() = default;
  ~meta_cache();
  meta_cache(const meta_cache&) = delete;
  meta_cache& operator=(const meta_cache&) = delete;

  // Opens (creating) `dir_utf8`/meta_cache.db. Opening the directory already
  // served is a no-op; another directory closes the first.
  [[nodiscard]] expected open(std::string_view dir_utf8);
  [[nodiscard]] bool is_open() const;
  void close();

  // Every row of `dir_utf8`'s own files (not sub-folders), stale or not: the
  // caller compares size and mtime with its listing. Rows of an older
  // kSearchFactsVersion are left out.
  [[nodiscard]] std::vector<cached_facts> folder(std::string_view dir_utf8);
  // The row for a file, only if it is still valid for (size, mtime).
  [[nodiscard]] std::optional<cached_facts> lookup(std::string_view path, std::uint64_t size, std::int64_t mtime);
  // Inserts or replaces, in one transaction.
  [[nodiscard]] expected store(std::span<const cached_facts> rows);
  // Drops a file's row (a metadata write that landed in a sidecar leaves the
  // file's own size and mtime alone).
  void forget(std::string_view path);
  [[nodiscard]] std::uint64_t rows();

  // The folder a path is in: everything before the last '/' (or '\\').
  [[nodiscard]] static std::string dir_of(std::string_view path);

 private:
  bool open_locked(const std::string& db_path);
  mutable std::mutex m_;
  sqlite3* db_ = nullptr;
  std::string dir_;
};

// The process's cache, at io::thumb_cache_dir(), opened on first use (from a
// worker: never call it on the UI thread the first time).
[[nodiscard]] meta_cache& default_meta_cache();

// A file's cached facts if valid, else a bounded read (read_search_facts)
// stored for next time. Worker thread only. For the host table's tag_facts /
// location_of, which ask one file at a time.
[[nodiscard]] result<cached_facts> cached_or_read(meta_cache& cache, std::string_view path);

// The fields joined by kFieldSeparator, whole fields only, at most `cap - 1`
// bytes (a C buffer with its NUL): the host table's tag_facts.
[[nodiscard]] std::string joined_fields(const std::vector<std::string>& fields, std::size_t cap);

}  // namespace mv::meta
