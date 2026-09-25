// SPDX-License-Identifier: GPL-2.0-or-later
// On-disk JPEG-512 thumbnail cache. Spec jpg512.2 (plan/04, plan/12 2026-09-07;
// .2 since PR 10: JPEG thumbs carry the EXIF orientation, so .1 rows regenerate).
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"

struct sqlite3;

namespace mv::image {

inline constexpr char kThumbSpec[] = "jpg512.2";
inline constexpr std::uint32_t kThumbLongEdge = 512;
inline constexpr int kThumbJpegQuality = 80;

struct thumb_key {
  std::string path;
  std::int64_t mtime_unix = 0;
  std::uint64_t size = 0;
};

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
  // whose file has since been deleted is a miss, and the row is dropped.
  [[nodiscard]] result<std::string> lookup(const thumb_key& key);
  [[nodiscard]] result<std::string> store(const thumb_key& key,
                                          std::span<const std::uint8_t> jpeg);

 private:
  // sqlite3 is compiled serialized, so the handle itself is safe to share.
  // This guards `db_` and `dir_` against open/close racing a lookup.
  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
  std::string dir_;
};

// The thumbnail's pixels: the same "first pixel is never the full decode"
// path (rule 3: JPEG DCT scaling, a RAW's embedded preview, else a decode),
// ICC -> sRGB, box-fit so the long edge is at most `max_long_edge`, never
// enlarged. Tightly packed, straight-alpha RGBA. make_thumb_jpeg is this at
// kThumbLongEdge, encoded; the Explorer handler takes the pixels directly.
struct thumb_pixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};
[[nodiscard]] result<thumb_pixels> make_thumb_rgba(std::span<const std::uint8_t> src_bytes,
                                                   std::uint32_t max_long_edge,
                                                   const job_context* ctx = nullptr);

[[nodiscard]] result<std::vector<std::uint8_t>> make_thumb_jpeg(
    std::span<const std::uint8_t> src_bytes, const job_context* ctx = nullptr);

// Same cache spec (jpg512.2), for callers that already hold pixels rather than
// an encoded file — the video poster frame, which image/ must not decode
// itself (player/ owns FFmpeg, and image/ never depends on player/). `rgba` is
// tightly packed and already no larger than kThumbLongEdge on its long edge.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_thumb_rgba(
    std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

}  // namespace mv::image
