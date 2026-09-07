// SPDX-License-Identifier: GPL-2.0-or-later
// On-disk JPEG-512 thumbnail cache. Spec jpg512.1 (plan/04, plan/12 2026-09-07).
#pragma once

#include <cstdint>
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

  [[nodiscard]] expected open(std::string_view dir_utf8);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return db_ != nullptr; }

  // Miss is status::io with an empty... no: miss is ok + empty string.
  [[nodiscard]] result<std::string> lookup(const thumb_key& key);
  [[nodiscard]] result<std::string> store(const thumb_key& key,
                                          std::span<const std::uint8_t> jpeg);

 private:
  sqlite3* db_ = nullptr;
  std::string dir_;
};

[[nodiscard]] result<std::vector<std::uint8_t>> make_thumb_jpeg(
    std::span<const std::uint8_t> src_bytes, const job_context* ctx = nullptr);

}  // namespace mv::image
