// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/meta_cache.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>

#include "io/file_port.h"
#include "io/paths.h"

namespace mv::meta {
namespace {

// Bump to throw every row away (a column change). kSearchFactsVersion covers
// what search_facts_of extracts; this covers the table's shape.
// 2 (#405): rating, label and the exposure numbers.
constexpr const char* kSchema = "2";

constexpr const char* kCreate =
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY, dir TEXT NOT NULL,"
    " size INTEGER NOT NULL, mtime INTEGER NOT NULL, version INTEGER NOT NULL,"
    " fields TEXT NOT NULL, folded TEXT NOT NULL, camera TEXT NOT NULL, lens TEXT NOT NULL,"
    " made_with TEXT NOT NULL, ai INTEGER NOT NULL, taken INTEGER NOT NULL,"
    " has_where INTEGER NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL,"
    " has_alt INTEGER NOT NULL, alt REAL NOT NULL, rating INTEGER NOT NULL, label TEXT NOT NULL,"
    " iso INTEGER NOT NULL, aperture INTEGER NOT NULL, focal INTEGER NOT NULL,"
    " shutter_den INTEGER NOT NULL, shutter_us INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS files_dir ON files(dir);";

constexpr const char* kCols =
    "path, size, mtime, fields, folded, camera, lens, made_with, ai, taken, has_where, lat, lon, has_alt, alt,"
    " rating, label, iso, aperture, focal, shutter_den, shutter_us";

class stmt {
 public:
  stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
  [[nodiscard]] bool ok() const noexcept { return s_ != nullptr; }
  void text(int i, std::string_view v) {
    sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  }
  void i64(int i, std::int64_t v) { sqlite3_bind_int64(s_, i, v); }
  void real(int i, double v) { sqlite3_bind_double(s_, i, v); }
  bool row() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
  bool run() {
    if (!s_) return false;
    const int rc = sqlite3_step(s_);
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
  }
  [[nodiscard]] std::string col_text(int c) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
    return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, c))) : std::string();
  }
  [[nodiscard]] std::int64_t col_i64(int c) const { return sqlite3_column_int64(s_, c); }
  [[nodiscard]] double col_real(int c) const { return sqlite3_column_double(s_, c); }

 private:
  sqlite3_stmt* s_ = nullptr;
};

std::string join_fields(const std::vector<std::string>& fields) {
  std::string out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i) out.push_back(kFieldSeparator);
    out += fields[i];
  }
  return out;
}

std::vector<std::string> split_fields(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::size_t b = 0;
  for (;;) {
    const std::size_t e = s.find(kFieldSeparator, b);
    out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
    if (e == std::string::npos) break;
    b = e + 1;
  }
  return out;
}

cached_facts row_of(const stmt& s) {
  cached_facts r;
  r.path = s.col_text(0);
  r.size = static_cast<std::uint64_t>(s.col_i64(1));
  r.mtime = s.col_i64(2);
  r.facts.fields = split_fields(s.col_text(3));
  r.folded = s.col_text(4);
  r.facts.camera = s.col_text(5);
  r.facts.lens = s.col_text(6);
  r.facts.made_with = s.col_text(7);
  r.facts.ai_generated = s.col_i64(8) != 0;
  r.facts.date_taken_key = s.col_i64(9);
  if (s.col_i64(10) != 0) {
    geo_point p;
    p.lat = s.col_real(11);
    p.lon = s.col_real(12);
    if (s.col_i64(13) != 0) p.altitude = s.col_real(14);
    r.facts.where = p;
  }
  r.facts.rating = static_cast<int>(s.col_i64(15));
  r.facts.label = s.col_text(16);
  r.facts.iso = static_cast<std::int32_t>(s.col_i64(17));
  r.facts.aperture_x100 = static_cast<std::int32_t>(s.col_i64(18));
  r.facts.focal_mm = static_cast<std::int32_t>(s.col_i64(19));
  r.facts.shutter_den = static_cast<std::int32_t>(s.col_i64(20));
  r.facts.shutter_us = s.col_i64(21);
  return r;
}

std::string join_dir(std::string_view dir, std::string_view file) {
  std::string out(dir);
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
  out.append(file);
  return out;
}

}  // namespace

meta_cache::~meta_cache() { close(); }

std::string meta_cache::dir_of(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

meta_cache::open_result meta_cache::open_locked(const std::string& db_path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return open_result::failed;  // no access, no space: deleting would not help
  }
  sqlite3_busy_timeout(db, 2000);
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  const int rc = sqlite3_exec(db, kCreate, nullptr, nullptr, nullptr);
  open_result out = open_result::ok;
  if (rc != SQLITE_OK) {
    // Only a file that is not this cache is made again. Another process
    // holding the lock (a second window) is busy, not damage: deleting the
    // file under it would throw its rows away.
    const int primary = rc & 0xFF;
    out = (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB) ? open_result::damaged : open_result::failed;
  } else {
    stmt q(db, "SELECT value FROM meta WHERE key='schema'");
    if (q.row()) {
      if (q.col_text(0) != kSchema) out = open_result::damaged;  // another shape: a cache, so make it again
    } else {
      stmt w(db, "INSERT OR IGNORE INTO meta(key, value) VALUES('schema', ?1)");
      w.text(1, kSchema);
      if (!w.run()) out = open_result::failed;
    }
  }
  if (out != open_result::ok) {
    sqlite3_close(db);
    return out;
  }
  db_ = db;
  return open_result::ok;
}

bool meta_cache::remake_locked(const std::string& db_path) {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  (void)io::remove_file(db_path);
  (void)io::remove_file(db_path + "-wal");
  (void)io::remove_file(db_path + "-shm");
  return open_locked(db_path) == open_result::ok;
}

expected meta_cache::open(std::string_view dir_utf8) {
  if (dir_utf8.empty()) return err(status::invalid_arg);
  std::lock_guard lock(m_);
  if (db_ && dir_ == dir_utf8) return {};
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  const std::string path = join_dir(dir_utf8, "meta_cache.db");
  const open_result r = open_locked(path);
  // A cache: a damaged or foreign file is made again, never a reason to fail.
  if (r == open_result::failed || (r == open_result::damaged && !remake_locked(path))) return err(status::io);
  dir_.assign(dir_utf8);
  return {};
}

bool meta_cache::is_open() const {
  std::lock_guard lock(m_);
  return db_ != nullptr;
}

void meta_cache::close() {
  std::lock_guard lock(m_);
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  dir_.clear();
}

std::vector<cached_facts> meta_cache::folder(std::string_view dir_utf8) {
  std::vector<cached_facts> out;
  std::lock_guard lock(m_);
  if (!db_) return out;
  stmt s(db_, (std::string("SELECT ") + kCols + " FROM files WHERE dir = ?1 AND version = ?2").c_str());
  s.text(1, dir_utf8);
  s.i64(2, kSearchFactsVersion);
  while (s.row()) out.push_back(row_of(s));
  return out;
}

std::optional<cached_facts> meta_cache::lookup(std::string_view path, std::uint64_t size, std::int64_t mtime) {
  std::lock_guard lock(m_);
  if (!db_) return std::nullopt;
  stmt s(db_, (std::string("SELECT ") + kCols +
               " FROM files WHERE path = ?1 AND size = ?2 AND mtime = ?3 AND version = ?4")
                  .c_str());
  s.text(1, path);
  s.i64(2, static_cast<std::int64_t>(size));
  s.i64(3, mtime);
  s.i64(4, kSearchFactsVersion);
  if (!s.row()) return std::nullopt;
  return row_of(s);
}

expected meta_cache::store(std::span<const cached_facts> rows) {
  if (rows.empty()) return {};
  std::lock_guard lock(m_);
  if (!db_) return err(status::internal);
  if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return err(status::io);
  bool ok = true;
  {
    stmt s(db_,
           "INSERT OR REPLACE INTO files(path, dir, size, mtime, version, fields, folded, camera, lens,"
           " made_with, ai, taken, has_where, lat, lon, has_alt, alt, rating, label, iso, aperture, focal,"
           " shutter_den, shutter_us)"
           " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20,"
           " ?21, ?22, ?23, ?24)");
    ok = s.ok();
    for (const cached_facts& r : rows) {
      if (!ok) break;
      const auto& f = r.facts;
      s.text(1, r.path);
      s.text(2, dir_of(r.path));
      s.i64(3, static_cast<std::int64_t>(r.size));
      s.i64(4, r.mtime);
      s.i64(5, kSearchFactsVersion);
      s.text(6, join_fields(f.fields));
      s.text(7, r.folded);
      s.text(8, f.camera);
      s.text(9, f.lens);
      s.text(10, f.made_with);
      s.i64(11, f.ai_generated ? 1 : 0);
      s.i64(12, f.date_taken_key);
      s.i64(13, f.where ? 1 : 0);
      s.real(14, f.where ? f.where->lat : 0.0);
      s.real(15, f.where ? f.where->lon : 0.0);
      s.i64(16, f.where && f.where->altitude ? 1 : 0);
      s.real(17, f.where && f.where->altitude ? *f.where->altitude : 0.0);
      s.i64(18, f.rating);
      s.text(19, f.label);
      s.i64(20, f.iso);
      s.i64(21, f.aperture_x100);
      s.i64(22, f.focal_mm);
      s.i64(23, f.shutter_den);
      s.i64(24, f.shutter_us);
      ok = s.run();
    }
  }
  if (!ok || sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
    const int primary = sqlite3_errcode(db_) & 0xFF;
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    // Damaged since it was opened (a disk error, a foreign write): make it
    // again rather than fail every later batch. The rows are a cache.
    if ((primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB) && !dir_.empty()) {
      (void)remake_locked(join_dir(dir_, "meta_cache.db"));
    }
    return err(status::io);
  }
  return {};
}

void meta_cache::forget_many(std::span<const std::string> paths) {
  if (paths.empty()) return;
  std::lock_guard lock(m_);
  if (!db_ || sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return;
  {
    stmt s(db_, "DELETE FROM files WHERE path = ?1");
    for (const std::string& p : paths) {
      s.text(1, p);
      (void)s.run();
    }
  }
  if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
}

void meta_cache::forget(std::string_view path) {
  std::lock_guard lock(m_);
  if (!db_) return;
  stmt s(db_, "DELETE FROM files WHERE path = ?1");
  s.text(1, path);
  (void)s.run();
}

std::uint64_t meta_cache::rows() {
  std::lock_guard lock(m_);
  if (!db_) return 0;
  stmt s(db_, "SELECT COUNT(*) FROM files");
  return s.row() ? static_cast<std::uint64_t>(s.col_i64(0)) : 0;
}

meta_cache& default_meta_cache() {
  static meta_cache cache;
  // A cache that cannot open (no space, no access) is tried again at most
  // every 30 s, not on every file the pass or tag_facts asks about.
  static std::atomic<std::int64_t> next_try{0};
  if (!cache.is_open()) {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    if (now >= next_try.load(std::memory_order_relaxed)) {
      auto dir = io::thumb_cache_dir();
      if (!dir || !cache.open(*dir)) next_try.store(now + 30, std::memory_order_relaxed);
    }
  }
  return cache;
}

std::string joined_fields(const std::vector<std::string>& fields, std::size_t cap) {
  std::string out;
  for (const std::string& f : fields) {
    const std::size_t need = out.size() + (out.empty() ? 0 : 1) + f.size();
    if (need + 1 > cap) break;
    if (!out.empty()) out.push_back(kFieldSeparator);
    out += f;
  }
  return out;
}

result<cached_facts> cached_or_read(meta_cache& cache, std::string_view path) {
  MV_TRY(io::file_stat st, io::stat_path(path));
  if (auto hit = cache.lookup(path, st.size, st.mtime_unix)) return std::move(*hit);
  MV_TRY(search_facts facts, read_search_facts(path));
  cached_facts row;
  row.path.assign(path);
  row.size = st.size;
  row.mtime = st.mtime_unix;
  row.folded = folded_text(facts.fields);
  row.facts = std::move(facts);
  (void)cache.store(std::span<const cached_facts>(&row, 1));
  return row;
}

}  // namespace mv::meta
