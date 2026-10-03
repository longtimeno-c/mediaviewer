// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/thumb_reader.h"

#include <sqlite3.h>

#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"

namespace mv::nle {

thumb_reader::~thumb_reader() {
  if (db_) sqlite3_close(db_);
}

result<std::string> thumb_reader::lookup(const std::string& path, std::int64_t pts_ms) {
  MV_TRY(io::file_stat st, io::stat_path(path));
  const image::thumb_key key = pts_ms >= 0 ? image::moment_thumb_key(path, pts_ms, st.mtime_unix, st.size)
                                           : image::thumb_key{path, st.mtime_unix, st.size};
  std::lock_guard lock(m_);
  if (!db_ && !tried_) {
    tried_ = true;
    const std::string file = io::join_path(dir_, "thumbs.sqlite");
    if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      if (db_) sqlite3_close(db_);
      db_ = nullptr;
    } else {
      sqlite3_busy_timeout(db_, 2000);
    }
  }
  if (!db_) return std::string();  // no cache yet: every tile is a placeholder
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT file FROM thumbs WHERE path=? AND mtime=? AND size=? AND spec=?;", -1, &s,
                         nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_bind_text(s, 1, key.path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(s, 2, key.mtime_unix);
  sqlite3_bind_int64(s, 3, static_cast<sqlite3_int64>(key.size));
  sqlite3_bind_text(s, 4, image::kThumbSpec, -1, SQLITE_STATIC);
  std::string file;
  if (sqlite3_step(s) == SQLITE_ROW) {
    if (const unsigned char* t = sqlite3_column_text(s, 0)) file.assign(reinterpret_cast<const char*>(t));
  }
  sqlite3_finalize(s);
  if (file.empty()) return std::string();
  std::string jpeg = io::join_path(dir_, file);
  return io::file_exists(jpeg) ? jpeg : std::string();
}

}  // namespace mv::nle
