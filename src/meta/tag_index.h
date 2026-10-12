// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #291 — the folder's tags, for Ctrl/⌘+F, `/` and sort by date taken
// (docs/design/16 "File search", "Status, sort, typeahead").
//
// One background pass per folder reads every file's search facts once
// (search.h: a bounded head read) and keeps them in memory for the folder on
// screen, backed by meta_cache.db so a folder seen before costs one SELECT and
// no file reads. The same pass gives the date-taken sort keys: a folder costs
// one read per file, not a date read and a tag read.
//
// "The view comes first" (CLAUDE.md), as code:
//   - the host starts the pass only once the item on screen has its first
//     pixel (both hosts call `scan` from the image-ready path);
//   - it runs as chunks of kChunk files, each a background-queue job, so
//     every foreground (view-tied) job and the ±2 prefetch are taken first,
//     and each chunk goes behind the thumbnail sweep already queued;
//   - between files it steps aside for the view (#291 review): a view job
//     waiting for a worker gets this one (the chunk re-queues itself), and
//     while a view job runs (the photo's full decode, the prefetch) or
//     `busy()` says the viewer is panning or playing (the host's
//     should_yield flag), it waits;
//   - a newer `scan` (another folder) supersedes it at the next file.
// Portable (D9); no lock is held across a file read.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "io/dir.h"
#include "meta/meta_cache.h"
#include "meta/search.h"

namespace mv::meta {

class tag_index {
 public:
  using reader_fn = std::function<result<search_facts>(std::string_view utf8_path)>;
  using busy_fn = std::function<bool()>;
  using notify_fn = std::function<void()>;

  // `cache` may be null (no persistence: tests, or a cache that failed to
  // open). It must outlive the index. The reader defaults to read_search_facts.
  explicit tag_index(meta_cache* cache, reader_fn reader = {});
  // The cache is asked for on the pass's first job, never on the caller's
  // thread (opening it is I/O): the hosts pass `default_meta_cache`.
  using cache_fn = std::function<meta_cache*()>;
  explicit tag_index(cache_fn cache, reader_fn reader = {});
  ~tag_index();
  tag_index(const tag_index&) = delete;
  tag_index& operator=(const tag_index&) = delete;

  static constexpr std::size_t kChunk = 32;

  // The folder on screen is `dir`, listed as `entries`. Keeps what it already
  // has for files whose (size, mtime) did not move, and queues the rest.
  // `on_progress` runs on the pool thread after each chunk and at the end (a
  // host marshals it). `busy` is polled between files; null = never busy.
  // Calling it again with the same folder and stamps reads nothing.
  void scan(std::string dir, std::vector<io::dir_entry> entries, job_system& jobs, notify_fn on_progress = {},
            busy_fn busy = {});
  // Stops the pass and forgets the folder (nothing is shown).
  void stop();

  struct progress_info {
    std::uint32_t done = 0;   // files whose facts are known
    std::uint32_t total = 0;  // files in the listing
    bool reading = false;     // the pass is still going
  };
  [[nodiscard]] progress_info progress() const;
  [[nodiscard]] std::string folder() const;

  struct hit {
    std::string path;
    std::string why;  // the field that matched ("Canon EOS R5")
  };
  // Files whose tag text holds every word of `typed` (folded), in listing
  // order. Only files already read; a short query (< 2 bytes folded) matches
  // nothing, so one typed letter does not light up every camera make.
  [[nodiscard]] std::vector<hit> match(std::string_view typed) const;

  // The date-taken key of a listed file: nullopt while unread, or when the
  // file has none (it sorts by mtime then). For io::sort_entries.
  [[nodiscard]] std::optional<std::int64_t> date_key(const io::dir_entry& e) const;
  // True once since the last call when facts arrived (the host re-sorts by
  // date taken and re-runs an open search).
  [[nodiscard]] bool consume_changed() noexcept;

  // A file's facts, when the pass has them for its current stamp.
  [[nodiscard]] std::optional<search_facts> facts(std::string_view path) const;
  // Every file read so far that carries coordinates, in listing order. Stop
  // early by returning false. (The host table's locations_in for the folder
  // on screen: served without a read.)
  void each_location(const std::function<bool(const std::string& path, const geo_point& where)>& visit) const;

  // PR 12: a metadata write landed for `path` (its sidecar may hold the new
  // tags): drop what is known. Memory only, so the UI thread may call it; the
  // cache row goes on the pass's next job. Call `scan` again to read it.
  void invalidate(std::string_view path);

  // Files read from disk (not from the cache) since construction: the tests
  // and the F3 overlay hold the pass to "once, ever".
  [[nodiscard]] std::uint64_t reads() const noexcept;

  // The pass's shared state (tag_index.cpp): its jobs own it, not `this`.
  struct state;

 private:
  std::shared_ptr<state> state_;
};

}  // namespace mv::meta
