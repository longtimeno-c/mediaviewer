// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// On-disk JPEG-512 thumbnail cache. Spec jpg512.4 (docs/design/04, docs/design/12 2026-09-07;
// .2 since PR 10: JPEG thumbs carry the EXIF orientation, so .1 rows regenerate;
// .3 since 2026-10-03: a clip's poster is turned by its display matrix, so .2
// rows regenerate; .4 since 2026-10-10: a transparent image is box-fit weighted
// by alpha and composited onto kThumbMatte, so .3 rows regenerate, issue #169).
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
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
  [[nodiscard]] result<std::string> store(const thumb_key& key,
                                          std::span<const std::uint8_t> jpeg);

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
  // Bumped by open/close: a row found missing after the lock was released is
  // only dropped from the connection it was read from.
  std::uint64_t epoch_ = 0;
  std::string dir_;
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
