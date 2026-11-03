// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// On-disk JPEG-512 thumbnail cache. Spec jpg512.4 (docs/design/04, docs/design/12 2026-09-07;
// .2 since PR 10: JPEG thumbs carry the EXIF orientation, so .1 rows regenerate;
// .3 since 2026-10-03: a clip's poster is turned by its display matrix, so .2
// rows regenerate; .4 since 2026-10-10: a transparent image is box-fit weighted
// by alpha and composited onto kThumbMatte, so .3 rows regenerate, issue #169).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace mv::image {

inline constexpr char kThumbSpec[] = "jpg512.4";
inline constexpr std::uint32_t kThumbLongEdge = 512;
inline constexpr int kThumbJpegQuality = 80;
// JPEG has no alpha: a thumbnail's transparency is composited onto this sRGB
// grey before the encode. It was the viewer's old dark background (linear
// 0.018); since issue #408 an open image shows the theme's canvas through its
// alpha instead. The matte stays: changing it would re-encode every cached
// thumbnail (kThumbSpec) for a few transparent files.
inline constexpr std::uint8_t kThumbMatte = 36;

struct thumb_key {
  std::string path;
  std::int64_t mtime_unix = 0;
  std::uint64_t size = 0;
};

// Milestone H (docs/design/17): a clip moment is its own row beside the file's poster,
// under the file's own stamp and its path with "#t=<ms>" appended. The cache
// keys absolute paths and no real file name ends that way, so the two cannot
// collide. The AI pack stores these rows (host table moment_thumbnail); result
// list tiles and a clip opened on its moment read them. One spelling, here.
[[nodiscard]] inline thumb_key moment_thumb_key(std::string path, std::int64_t pts_ms,
                                                std::int64_t mtime_unix, std::uint64_t size) {
  path += "#t=";
  path += std::to_string(pts_ms);
  return thumb_key{std::move(path), mtime_unix, size};
}

// #612: what a sweep (thumb_store::sweep) found and did. Sizes are the JPEG
// files' own; thumbs.sqlite and meta_cache.db are not counted here.
struct thumb_sweep_stats {
  std::uint64_t rows_removed = 0;   // an old spec, a missing JPEG, over the cap
  std::uint64_t files_removed = 0;  // JPEGs deleted: those rows', and orphans
  std::uint64_t bytes_removed = 0;
  std::uint64_t files_kept = 0;
  std::uint64_t bytes_kept = 0;
};

// How a sweep paces itself between steps (docs/design/04 "Thumbnail cache
// upkeep"): `go` on, `wait` (sleep a beat and ask again: a view job is
// running, or the viewer is panning or playing), or `step_aside` (return now,
// keeping its place: a view job is waiting for this worker).
enum class sweep_pace : std::uint8_t { go, wait, step_aside };
using sweep_pacer = std::function<sweep_pace()>;

// A sweep's place, carried between runs so one that stepped aside resumes
// where it was rather than starting again. Fresh-constructed for each sweep.
struct thumb_sweep {
  enum class phase : std::uint8_t { start, rows, orphans, cap, done };
  phase at = phase::start;
  thumb_sweep_stats stats;
  // Internal: the store's cancel token at the start, the newest row at the
  // start, the directory listing, the rows kept (for the cap) and a cursor.
  std::uint64_t token = 0;
  std::int64_t max_rowid = 0;
  std::int64_t after_rowid = 0;
  std::int64_t started_unix = 0;
  struct listed {
    std::uint64_t size = 0;
    std::int64_t mtime_unix = 0;
    bool kept = false;
  };
  std::unordered_map<std::string, listed> files;
  std::vector<std::string> orphans;
  struct kept_row {
    std::int64_t rowid = 0;
    std::string file;
    std::uint64_t size = 0;
    std::int64_t mtime_unix = 0;
  };
  std::vector<kept_row> kept;
  std::size_t cursor = 0;
};

// True for a name the cache writes (16 lowercase hex digits + ".jpg"). The
// sweep and Clear delete only these: thumbs.sqlite and meta_cache.db (the
// file search cache, docs/design/16) share the directory.
[[nodiscard]] bool is_thumb_file_name(std::string_view name) noexcept;

class thumb_store {
 public:
  thumb_store() = default;
  ~thumb_store();

  thumb_store(const thumb_store&) = delete;
  thumb_store& operator=(const thumb_store&) = delete;

  // Idempotent: opening an already-open store on the same directory is a
  // no-op rather than a close/reopen. Thumb jobs run on every pool thread and
  // the check-then-open they used to do could close the handle out from under
  // a lookup already in flight.
  [[nodiscard]] expected open(std::string_view dir_utf8);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;

  // Hit is ok + the thumbnail's path; miss is ok + an empty string. A row
  // whose file has since been deleted is a miss, and the row is dropped. For
  // callers that hand the path on (the chrome loads it as a bitmap): the one
  // stat that proves the file is there runs with the lock released.
  [[nodiscard]] result<std::string> lookup(const thumb_key& key);
  // The row's path with no stat (issue #168), for a caller that reads the
  // bytes itself: a failed read is the miss, and the caller drops the row with
  // forget() so the next sweep regenerates it.
  [[nodiscard]] result<std::string> lookup_unverified(const thumb_key& key);
  void forget(const thumb_key& key) noexcept;
  // Writes the JPEG and its row. #612: the path's rows of any other stamp or
  // spec (the file was edited, rotated or replaced; an old spec) go now, with
  // their JPEGs: a lookup of the new stamp missed, which is what brought the
  // caller here, so the old ones can never be hit again.
  [[nodiscard]] result<std::string> store(const thumb_key& key,
                                          std::span<const std::uint8_t> jpeg);

  // #612: the self-tidying cache (docs/design/04 "Thumbnail cache upkeep").
  // A pool thread at background_generation, never the UI or render thread:
  // it lists the directory once and deletes files. Removes the rows of an old
  // spec and the rows whose JPEG is gone, deletes JPEGs no row names (older
  // than a minute: a store() in flight writes its file before its row), and,
  // when `cap_bytes` is not 0, the oldest-written JPEGs over the cap. The lock
  // is held for one batch of rows at a time, never across a listing or a
  // delete. `pacer` is asked between batches and between deletes. Returns
  // ok(true) when done (state.at == done, state.stats final), ok(false) when
  // the pacer said step_aside (call again with the same `state`),
  // status::cancelled when the store was closed, reopened or cleared under
  // it, stop_sweep() was called, or `ctx` was cancelled.
  [[nodiscard]] result<bool> sweep(thumb_sweep& state, std::uint64_t cap_bytes,
                                   const job_context& ctx, const sweep_pacer& pacer = {});

  // #612, Settings -> Storage -> Clear: every row and every JPEG the cache
  // wrote. thumbs.sqlite stays (emptied); meta_cache.db and anything else in
  // the directory is untouched. A sweep under way stops. Next visits
  // regenerate. Worker thread only.
  [[nodiscard]] expected clear();

  // Stops a sweep in progress at its next step, and refuses new ones until
  // the next open(). A host calls it before shutting its pool down: a sweep
  // waiting out a busy viewer would otherwise hold the worker the join waits
  // for.
  void stop_sweep() noexcept;

  // #612, both hosts: queues sweep() on `jobs` at background_generation,
  // paced like the folder tag pass (meta/tag_index.h): a view job waiting for
  // a worker gets this one (the pass re-queues itself, keeping its place);
  // while a view job runs or `busy()` says the viewer is panning or playing
  // (the host's should_yield flag), it waits. `done` runs on the pool thread
  // once, at the end (status::cancelled when stopped or the pool shut down).
  // False, and nothing queued, when a sweep of this store is already under
  // way. This store (and `keep_alive`, which the jobs hold) must outlive the
  // pass: a host calls stop_sweep() and then shuts its pool down.
  using sweep_done_fn = std::function<void(result<thumb_sweep_stats>)>;
  bool submit_sweep(job_system& jobs, std::uint64_t cap_bytes, std::function<bool()> busy,
                    sweep_done_fn done = {}, std::shared_ptr<void> keep_alive = {});
  [[nodiscard]] bool sweep_running() const noexcept {
    return sweep_running_.load(std::memory_order_relaxed);
  }

  // The JPEG bytes the cache holds, as of the last finished sweep plus what
  // store() added since. `known` is false until a sweep has finished.
  [[nodiscard]] std::uint64_t total_bytes(bool* known = nullptr) const noexcept;

 private:
  void finalize_locked() noexcept;
  [[nodiscard]] result<std::string> select_locked(const thumb_key& key);
  void delete_locked(const thumb_key& key) noexcept;

  // Every use of `db_` and its statements is under this lock, so the handle
  // is opened SQLITE_OPEN_NOMUTEX. Held for one primary-key step at a time:
  // never across a stat, a file write or a decode (issue #168).
  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
  // Prepared once per connection, reset after each use, finalized on close.
  sqlite3_stmt* select_ = nullptr;
  sqlite3_stmt* insert_ = nullptr;
  sqlite3_stmt* delete_ = nullptr;
  // #612: store()'s pair for the path's rows of other stamps and specs.
  sqlite3_stmt* others_select_ = nullptr;
  sqlite3_stmt* others_delete_ = nullptr;
  // Bumped by open/close: a row found missing after the lock was released is
  // only dropped from the connection it was read from.
  std::uint64_t epoch_ = 0;
  std::string dir_;
  // #612: moved by open/close/clear/stop_sweep; a sweep whose token moved stops.
  std::atomic<std::uint64_t> sweep_token_{1};
  std::atomic<std::uint64_t> total_bytes_{0};
  std::atomic<bool> total_known_{false};
  std::atomic<bool> sweep_running_{false};
  std::atomic<bool> sweeps_stopped_{false};  // stop_sweep() until the next open()

  struct sweep_pass;
  void queue_sweep_step(const std::shared_ptr<sweep_pass>& pass);
};

// The thumbnail's pixels: the same "first pixel is never the full decode"
// path (rule 3: JPEG DCT scaling, a RAW's embedded preview, else a decode),
// ICC -> sRGB, box-fit so the long edge is at most `max_long_edge`, never
// enlarged. Tightly packed, straight-alpha RGBA. make_thumb_jpeg is this at
// kThumbLongEdge, composited onto kThumbMatte and encoded; the Explorer
// handler takes the straight-alpha pixels directly.
struct thumb_pixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};
[[nodiscard]] result<thumb_pixels> make_thumb_rgba(std::span<const std::uint8_t> src_bytes,
                                                   std::uint32_t max_long_edge,
                                                   const job_context* ctx = nullptr);

// Flattens `t` onto kThumbMatte in place (alpha becomes 255): the step
// before make_thumb_jpeg's encode, since JPEG would drop alpha and show the
// RGB under a transparent pixel.
void composite_thumb_matte(thumb_pixels& t) noexcept;

[[nodiscard]] result<std::vector<std::uint8_t>> make_thumb_jpeg(
    std::span<const std::uint8_t> src_bytes, const job_context* ctx = nullptr);

// Same cache spec (jpg512.4), for callers that already hold pixels rather than
// an encoded file — the video poster frame, which image/ must not decode
// itself (player/ owns FFmpeg, and image/ never depends on player/). `rgba` is
// tightly packed and already no larger than kThumbLongEdge on its long edge.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_thumb_rgba(
    std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

}  // namespace mv::image
