// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The viewer's JPEG-512 cache, looked up from another process (docs/design/23): the
// search agent serves the tiles the app already made and never makes one
// (no decoder is linked into it). thumbs.sqlite is opened read-only, the row
// is keyed exactly as image/thumb.h keys it (path + mtime + size + spec, a
// moment as "#t=<ms>"), and a row whose file has gone is a miss, not a
// delete. Portable: the cache folder comes from io::thumb_cache_dir on each
// platform.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "core/result.h"

struct sqlite3;

namespace mv::nle {

class thumb_reader {
 public:
  explicit thumb_reader(std::string cache_dir) : dir_(std::move(cache_dir)) {}
  ~thumb_reader();
  thumb_reader(const thumb_reader&) = delete;
  thumb_reader& operator=(const thumb_reader&) = delete;

  // The JPEG's path for a still (pts_ms < 0) or a clip moment; "" on a miss.
  // Stats `path` for the key's mtime and size. [worker-thread]
  [[nodiscard]] result<std::string> lookup(const std::string& path, std::int64_t pts_ms);

 private:
  std::mutex m_;
  std::string dir_;
  sqlite3* db_ = nullptr;
  bool tried_ = false;
};

}  // namespace mv::nle
