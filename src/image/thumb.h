// SPDX-License-Identifier: GPL-2.0-or-later
// On-disk JPEG-512 thumbnail cache. Spec jpg512.1 (plan/04, plan/12 2026-09-07).
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

inline constexpr char kThumbSpec[] = "jpg512.1";
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

[[nodiscard]] result<std::vector<std::uint8_t>> make_thumb_jpeg(
    std::span<const std::uint8_t> src_bytes, const job_context* ctx = nullptr);

// Same cache spec (jpg512.1), for callers that already hold pixels rather than
// an encoded file — the video poster frame, which image/ must not decode
// itself (player/ owns FFmpeg, and image/ never depends on player/). `rgba` is
// tightly packed and already no larger than kThumbLongEdge on its long edge.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_thumb_rgba(
    std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

}  // namespace mv::image
