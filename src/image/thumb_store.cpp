// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The JPEG-512 cache's SQLite index (image/thumb.h), one TU for both hosts
// (it used to be copied into thumb.cpp and thumb_mac.cpp, which differed only
// in the path separator). Issue #168: statements are prepared once per
// connection, the lock covers one SQLite step and never a stat or a write, and
// a caller that reads the bytes treats a failed read as the miss.
#include <cstdio>
#include <cstring>

#include <sqlite3.h>

#include "image/thumb.h"
#include "io/file.h"

namespace mv::image {
namespace {

constexpr char kSchema[] =
    "CREATE TABLE IF NOT EXISTS thumbs ("
    "  path TEXT NOT NULL,"
    "  mtime INTEGER NOT NULL,"
    "  size INTEGER NOT NULL,"
    "  spec TEXT NOT NULL,"
    "  file TEXT NOT NULL,"
    "  PRIMARY KEY (path, mtime, size, spec)"
    ");";

constexpr char kSelect[] = "SELECT file FROM thumbs WHERE path=? AND mtime=? AND size=? AND spec=?;";
constexpr char kInsert[] =
    "INSERT OR REPLACE INTO thumbs(path,mtime,size,spec,file) VALUES(?,?,?,?,?);";
constexpr char kDelete[] = "DELETE FROM thumbs WHERE path=? AND mtime=? AND size=? AND spec=?;";

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string join_dir(std::string_view dir, std::string_view file) {
  if (dir.empty()) return std::string(file);
  const char last = dir.back();
  std::string out;
  out.reserve(dir.size() + 1 + file.size());
  out.append(dir);
#if defined(_WIN32)
  if (last != '\\' && last != '/') out.push_back('\\');
#else
  if (last != '/') out.push_back('/');
#endif
  out.append(file);
  return out;
}

std::string cache_name(const thumb_key& key) {
  std::string material;
  material.reserve(key.path.size() + 48);
  material.append(key.path);
  material.push_back('|');
  material.append(std::to_string(key.mtime_unix));
  material.push_back('|');
  material.append(std::to_string(key.size));
  material.push_back('|');
  material.append(kThumbSpec);
  const std::uint64_t h = fnv1a(material);
  char name[32]{};
  std::snprintf(name, sizeof(name), "%016llx.jpg", static_cast<unsigned long long>(h));
  return name;
}

// Binds the four key columns. SQLITE_STATIC: the key outlives the step, and
// the statement is reset (bindings cleared) before the lock is released.
void bind_key(sqlite3_stmt* stmt, const thumb_key& key) noexcept {
  sqlite3_bind_text(stmt, 1, key.path.c_str(), static_cast<int>(key.path.size()), SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, key.mtime_unix);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(key.size));
  sqlite3_bind_text(stmt, 4, kThumbSpec, -1, SQLITE_STATIC);
}

void done_with(sqlite3_stmt* stmt) noexcept {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
}

}  // namespace

thumb_store::~thumb_store() { close(); }

bool thumb_store::is_open() const noexcept {
  std::lock_guard lock(mutex_);
  return db_ != nullptr;
}

void thumb_store::finalize_locked() noexcept {
  sqlite3_finalize(select_);
  sqlite3_finalize(insert_);
  sqlite3_finalize(delete_);
  select_ = insert_ = delete_ = nullptr;
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  ++epoch_;
}

expected thumb_store::open(std::string_view dir_utf8) {
  if (dir_utf8.empty()) return err(status::invalid_arg);

  std::lock_guard lock(mutex_);
  // Already serving this directory. Reopening would close a handle that
  // lookups on other pool threads are about to use.
  if (db_ != nullptr && dir_ == dir_utf8) return {};
  finalize_locked();
  dir_.assign(dir_utf8);
  const std::string db_path = join_dir(dir_, "thumbs.sqlite");
  sqlite3* db = nullptr;
  // NOMUTEX: mutex_ already serialises every use of the handle, so SQLite's
  // own per-call connection mutex is a second lock for nothing.
  if (sqlite3_open_v2(db_path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return err(status::io);
  }
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_stmt* sel = nullptr;
  sqlite3_stmt* ins = nullptr;
  sqlite3_stmt* del = nullptr;
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kSelect, -1, SQLITE_PREPARE_PERSISTENT, &sel, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kInsert, -1, SQLITE_PREPARE_PERSISTENT, &ins, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kDelete, -1, SQLITE_PREPARE_PERSISTENT, &del, nullptr) != SQLITE_OK) {
    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sqlite3_finalize(del);
    sqlite3_close(db);
    return err(status::io);
  }
  db_ = db;
  select_ = sel;
  insert_ = ins;
  delete_ = del;
  return {};
}

void thumb_store::close() noexcept {
  std::lock_guard lock(mutex_);
  finalize_locked();
  dir_.clear();
}

result<std::string> thumb_store::select_locked(const thumb_key& key) {
  if (!db_) return err(status::internal);
  bind_key(select_, key);
  std::string file;
  const int rc = sqlite3_step(select_);
  if (rc == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(select_, 0);
    if (t) file.assign(reinterpret_cast<const char*>(t));
  }
  done_with(select_);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return err(status::io);
  if (file.empty()) return std::string{};
  return join_dir(dir_, file);
}

void thumb_store::delete_locked(const thumb_key& key) noexcept {
  if (!db_) return;
  bind_key(delete_, key);
  sqlite3_step(delete_);
  done_with(delete_);
}

result<std::string> thumb_store::lookup_unverified(const thumb_key& key) {
  std::lock_guard lock(mutex_);
  return select_locked(key);
}

void thumb_store::forget(const thumb_key& key) noexcept {
  std::lock_guard lock(mutex_);
  delete_locked(key);
}

result<std::string> thumb_store::lookup(const thumb_key& key) {
  std::string path;
  std::uint64_t epoch = 0;
  {
    std::lock_guard lock(mutex_);
    MV_TRY(std::string found, select_locked(key));
    path = std::move(found);
    epoch = epoch_;
  }
  if (path.empty()) return path;

  // The row is not proof the bytes survived. Clearing the thumbs directory
  // while thumbs.sqlite lives on used to leave every item pointing at a file
  // that is not there, with no path back to regenerating it. The stat runs
  // unlocked: the whole sweep and the foreground clip placeholder share this
  // store.
  if (io::file_exists(path)) return path;
  std::lock_guard lock(mutex_);
  if (epoch == epoch_) delete_locked(key);
  return std::string{};
}

result<std::string> thumb_store::store(const thumb_key& key, std::span<const std::uint8_t> jpeg) {
  if (jpeg.empty()) return err(status::invalid_arg);

  // Snapshot the directory, then write the file with the lock released: every
  // pool thread stores thumbs, and serializing them on one mutex would undo
  // the point of having a pool.
  std::string file = cache_name(key);
  std::string path;
  {
    std::lock_guard lock(mutex_);
    if (!db_) return err(status::invalid_arg);
    path = join_dir(dir_, file);
  }
  auto written = io::write_all(path, jpeg);
  if (!written) return err(written.error());

  std::lock_guard lock(mutex_);
  if (!db_) return err(status::internal);
  bind_key(insert_, key);
  sqlite3_bind_text(insert_, 5, file.c_str(), static_cast<int>(file.size()), SQLITE_STATIC);
  const int rc = sqlite3_step(insert_);
  done_with(insert_);
  if (rc != SQLITE_DONE) return err(status::io);
  return path;
}

}  // namespace mv::image
