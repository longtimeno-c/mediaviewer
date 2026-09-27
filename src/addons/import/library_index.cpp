// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/library_index.h"

#include <sqlite3.h>

#include <cstring>

#include "core/json.h"

namespace mv::import {
namespace {

constexpr int kSchemaVersion = 1;

// A prepared statement that finalizes itself. Binds are 1-based, as SQLite's.
class stmt {
 public:
  stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;

  [[nodiscard]] bool ok() const noexcept { return s_ != nullptr; }
  stmt& bind(int i, const std::string& v) {
    sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  stmt& bind(int i, std::int64_t v) {
    sqlite3_bind_int64(s_, i, v);
    return *this;
  }
  stmt& bind(int i, const digest& d) {
    sqlite3_bind_blob(s_, i, d.data(), static_cast<int>(d.size()), SQLITE_TRANSIENT);
    return *this;
  }
  void reset() {
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
  }
  bool step_row() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
  bool run() {
    if (!s_) return false;
    const int rc = sqlite3_step(s_);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
  }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, col));
    return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, col))) : std::string();
  }
  [[nodiscard]] std::int64_t i64(int col) const { return sqlite3_column_int64(s_, col); }
  [[nodiscard]] digest hash(int col) const {
    digest d{};
    const void* p = sqlite3_column_blob(s_, col);
    if (p && sqlite3_column_bytes(s_, col) == static_cast<int>(d.size())) {
      std::memcpy(d.data(), p, d.size());
    }
    return d;
  }

 private:
  sqlite3_stmt* s_ = nullptr;
};

std::string strings_json(const std::vector<std::string>& v) {
  json::writer w;
  w.begin_array();
  for (const auto& s : v) w.string(s);
  w.end_array();
  return w.take();
}

std::vector<std::string> strings_from(const std::string& text) {
  std::vector<std::string> out;
  const auto doc = json::parse(text);
  if (!doc || doc->k != json::kind::array) return out;
  for (const auto& v : doc->a) {
    if (v.k == json::kind::string) out.push_back(v.s);
  }
  return out;
}

library_row library_from(const stmt& s) {
  library_row r;
  r.root = s.text(0);
  r.rel = s.text(1);
  r.size = static_cast<std::uint64_t>(s.i64(2));
  r.mtime = s.i64(3);
  r.hash = s.hash(4);
  return r;
}

job_row job_from(const stmt& s) {
  job_row j;
  j.id = static_cast<std::uint64_t>(s.i64(0));
  j.created = s.i64(1);
  j.finished = s.i64(2);
  j.state = static_cast<std::uint32_t>(s.i64(3));
  j.source_root = s.text(4);
  j.volume_id = s.text(5);
  j.label = s.text(6);
  j.device_key = s.text(7);
  j.preset_json = s.text(8);
  j.summary_json = s.text(9);
  j.kind = static_cast<std::uint32_t>(s.i64(10));
  return j;
}

constexpr const char* kJobColumns =
    "id, created, finished, state, source_root, volume_id, label, device_key, preset_json, "
    "summary_json, kind";

}  // namespace

library_index::~library_index() {
  if (db_) sqlite3_close(db_);
}

bool library_index::exec(const char* sql) {
  return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

result<std::unique_ptr<library_index>> library_index::open(const std::string& db_path) {
  std::unique_ptr<library_index> idx(new library_index());
  if (sqlite3_open_v2(db_path.c_str(), &idx->db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_busy_timeout(idx->db_, 5000);
  // WAL + synchronous=FULL: the journal row that says "verified" must survive
  // a power cut, or a resume would skip a file that never landed.
  if (!idx->exec("PRAGMA journal_mode=WAL;") || !idx->exec("PRAGMA synchronous=FULL;") ||
      !idx->exec("PRAGMA foreign_keys=ON;")) {
    return err(status::io);
  }
  const bool created = idx->exec(
      "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS library(root TEXT NOT NULL, rel TEXT NOT NULL,"
      "  size INTEGER NOT NULL, mtime INTEGER NOT NULL, hash BLOB NOT NULL,"
      "  PRIMARY KEY(root, rel));"
      "CREATE INDEX IF NOT EXISTS library_size ON library(size);"
      "CREATE TABLE IF NOT EXISTS card_files(volume_id TEXT NOT NULL, rel TEXT NOT NULL,"
      "  size INTEGER NOT NULL, mtime INTEGER NOT NULL, hash BLOB NOT NULL,"
      "  imported_at INTEGER NOT NULL, PRIMARY KEY(volume_id, rel));"
      "CREATE TABLE IF NOT EXISTS seq(root TEXT NOT NULL, day TEXT NOT NULL,"
      "  last INTEGER NOT NULL, PRIMARY KEY(root, day));"
      "CREATE TABLE IF NOT EXISTS presets(name TEXT PRIMARY KEY, json TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS card_presets(volume_id TEXT PRIMARY KEY, preset TEXT NOT NULL,"
      "  auto_import INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS sources(path TEXT PRIMARY KEY);"
      "CREATE TABLE IF NOT EXISTS jobs(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  created INTEGER NOT NULL, finished INTEGER NOT NULL DEFAULT 0,"
      "  state INTEGER NOT NULL, source_root TEXT NOT NULL, volume_id TEXT NOT NULL,"
      "  label TEXT NOT NULL, device_key TEXT NOT NULL, preset_json TEXT NOT NULL,"
      "  summary_json TEXT NOT NULL DEFAULT '', kind INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE IF NOT EXISTS journal(job INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,"
      "  unit INTEGER NOT NULL, member INTEGER NOT NULL, src TEXT NOT NULL, rel TEXT NOT NULL,"
      "  size INTEGER NOT NULL, mtime INTEGER NOT NULL, targets TEXT NOT NULL,"
      "  target_roots TEXT NOT NULL, state INTEGER NOT NULL, hash BLOB,"
      "  reason TEXT NOT NULL DEFAULT '', display TEXT NOT NULL DEFAULT '',"
      "  PRIMARY KEY(job, unit, member));");
  if (!created) return err(status::io);
  {
    stmt s(idx->db_, "INSERT OR IGNORE INTO meta(key, value) VALUES('schema', ?1)");
    s.bind(1, std::to_string(kSchemaVersion)).run();
  }
  return idx;
}

std::vector<library_row> library_index::by_size(std::uint64_t size, const std::string* root) {
  std::lock_guard lock(mutex_);
  std::vector<library_row> out;
  stmt s(db_, root ? "SELECT root, rel, size, mtime, hash FROM library WHERE size = ?1 AND root = ?2"
                   : "SELECT root, rel, size, mtime, hash FROM library WHERE size = ?1");
  s.bind(1, static_cast<std::int64_t>(size));
  if (root) s.bind(2, *root);
  while (s.step_row()) out.push_back(library_from(s));
  return out;
}

std::optional<library_row> library_index::by_path(const std::string& root, const std::string& rel) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT root, rel, size, mtime, hash FROM library WHERE root = ?1 AND rel = ?2");
  s.bind(1, root).bind(2, rel);
  if (!s.step_row()) return std::nullopt;
  return library_from(s);
}

void library_index::upsert(const library_row& row) {
  std::lock_guard lock(mutex_);
  stmt s(db_,
         "INSERT INTO library(root, rel, size, mtime, hash) VALUES(?1, ?2, ?3, ?4, ?5) "
         "ON CONFLICT(root, rel) DO UPDATE SET size = excluded.size, mtime = excluded.mtime, "
         "hash = excluded.hash");
  s.bind(1, row.root).bind(2, row.rel).bind(3, static_cast<std::int64_t>(row.size))
      .bind(4, row.mtime).bind(5, row.hash).run();
}

void library_index::drop(const std::string& root, const std::string& rel) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "DELETE FROM library WHERE root = ?1 AND rel = ?2");
  s.bind(1, root).bind(2, rel).run();
}

std::vector<library_row> library_index::all_rows() {
  std::lock_guard lock(mutex_);
  std::vector<library_row> out;
  stmt s(db_, "SELECT root, rel, size, mtime, hash FROM library ORDER BY root, rel");
  while (s.step_row()) out.push_back(library_from(s));
  return out;
}

std::optional<card_row> library_index::card_lookup(const std::string& volume_id,
                                                   const std::string& rel, std::uint64_t size,
                                                   std::int64_t mtime) {
  std::lock_guard lock(mutex_);
  stmt s(db_,
         "SELECT hash, imported_at FROM card_files WHERE volume_id = ?1 AND rel = ?2 "
         "AND size = ?3 AND mtime = ?4");
  s.bind(1, volume_id).bind(2, rel).bind(3, static_cast<std::int64_t>(size)).bind(4, mtime);
  if (!s.step_row()) return std::nullopt;
  card_row r;
  r.hash = s.hash(0);
  r.imported_at = s.i64(1);
  return r;
}

void library_index::card_record(const std::string& volume_id, const std::string& rel,
                                std::uint64_t size, std::int64_t mtime, const digest& hash,
                                std::int64_t imported_at) {
  std::lock_guard lock(mutex_);
  // A hash-only record never clears an earlier import time.
  stmt s(db_,
         "INSERT INTO card_files(volume_id, rel, size, mtime, hash, imported_at) "
         "VALUES(?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT(volume_id, rel) DO UPDATE SET "
         "size = excluded.size, mtime = excluded.mtime, hash = excluded.hash, "
         "imported_at = CASE WHEN excluded.imported_at > 0 THEN excluded.imported_at "
         "WHEN card_files.hash = excluded.hash THEN card_files.imported_at ELSE 0 END");
  s.bind(1, volume_id).bind(2, rel).bind(3, static_cast<std::int64_t>(size)).bind(4, mtime)
      .bind(5, hash).bind(6, imported_at).run();
}

std::uint32_t library_index::last_seq(const std::string& root, const std::string& day) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT last FROM seq WHERE root = ?1 AND day = ?2");
  s.bind(1, root).bind(2, day);
  return s.step_row() ? static_cast<std::uint32_t>(s.i64(0)) : 0u;
}

void library_index::commit_seq(const std::string& root, const std::string& day, std::uint32_t seq) {
  std::lock_guard lock(mutex_);
  stmt s(db_,
         "INSERT INTO seq(root, day, last) VALUES(?1, ?2, ?3) ON CONFLICT(root, day) "
         "DO UPDATE SET last = MAX(last, excluded.last)");
  s.bind(1, root).bind(2, day).bind(3, static_cast<std::int64_t>(seq)).run();
}

std::vector<std::string> library_index::preset_jsons() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  stmt s(db_, "SELECT json FROM presets ORDER BY name");
  while (s.step_row()) out.push_back(s.text(0));
  return out;
}

void library_index::save_preset(const std::string& name, const std::string& json_text) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "INSERT INTO presets(name, json) VALUES(?1, ?2) ON CONFLICT(name) DO UPDATE SET json = excluded.json");
  s.bind(1, name).bind(2, json_text).run();
}

void library_index::delete_preset(const std::string& name) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "DELETE FROM presets WHERE name = ?1");
  s.bind(1, name).run();
  stmt b(db_, "DELETE FROM card_presets WHERE preset = ?1");
  b.bind(1, name).run();
}

std::optional<std::string> library_index::preset_json(const std::string& name) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT json FROM presets WHERE name = ?1");
  s.bind(1, name);
  if (!s.step_row()) return std::nullopt;
  return s.text(0);
}

std::optional<library_index::binding> library_index::card_binding(const std::string& volume_id) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT preset, auto_import FROM card_presets WHERE volume_id = ?1");
  s.bind(1, volume_id);
  if (!s.step_row()) return std::nullopt;
  return binding{s.text(0), s.i64(1) != 0};
}

void library_index::bind_card(const std::string& volume_id, const std::string& preset,
                              bool auto_import) {
  std::lock_guard lock(mutex_);
  if (preset.empty()) {
    stmt s(db_, "DELETE FROM card_presets WHERE volume_id = ?1");
    s.bind(1, volume_id).run();
    return;
  }
  stmt s(db_,
         "INSERT INTO card_presets(volume_id, preset, auto_import) VALUES(?1, ?2, ?3) "
         "ON CONFLICT(volume_id) DO UPDATE SET preset = excluded.preset, "
         "auto_import = excluded.auto_import");
  s.bind(1, volume_id).bind(2, preset).bind(3, static_cast<std::int64_t>(auto_import ? 1 : 0)).run();
}

std::vector<std::string> library_index::folder_sources() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  stmt s(db_, "SELECT path FROM sources ORDER BY path");
  while (s.step_row()) out.push_back(s.text(0));
  return out;
}

void library_index::add_folder_source(const std::string& path) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "INSERT OR IGNORE INTO sources(path) VALUES(?1)");
  s.bind(1, path).run();
}

void library_index::remove_folder_source(const std::string& path) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "DELETE FROM sources WHERE path = ?1");
  s.bind(1, path).run();
}

std::string library_index::setting(const std::string& key) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT value FROM meta WHERE key = ?1");
  s.bind(1, "setting." + key);
  return s.step_row() ? s.text(0) : std::string();
}

void library_index::set_setting(const std::string& key, const std::string& value) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  s.bind(1, "setting." + key).bind(2, value).run();
}

std::uint64_t library_index::create_job(const job_row& job) {
  std::lock_guard lock(mutex_);
  stmt s(db_,
         "INSERT INTO jobs(id, created, finished, state, source_root, volume_id, label, device_key, "
         "preset_json, summary_json, kind) VALUES(?9, ?1, 0, ?2, ?3, ?4, ?5, ?6, ?7, '', ?8)");
  s.bind(1, job.created).bind(2, static_cast<std::int64_t>(job.state)).bind(3, job.source_root)
      .bind(4, job.volume_id).bind(5, job.label).bind(6, job.device_key).bind(7, job.preset_json)
      .bind(8, static_cast<std::int64_t>(job.kind)).bind(9, static_cast<std::int64_t>(job.id));
  if (!s.run()) return 0;
  return static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_));
}

std::uint64_t library_index::max_job_id() {
  std::lock_guard lock(mutex_);
  stmt s(db_, "SELECT COALESCE(MAX(id), 0) FROM jobs");
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0u;
}

void library_index::set_job_state(std::uint64_t id, std::uint32_t state, std::int64_t finished,
                                  const std::string& summary_json) {
  std::lock_guard lock(mutex_);
  stmt s(db_, "UPDATE jobs SET state = ?2, finished = ?3, summary_json = ?4 WHERE id = ?1");
  s.bind(1, static_cast<std::int64_t>(id)).bind(2, static_cast<std::int64_t>(state))
      .bind(3, finished).bind(4, summary_json).run();
}

std::optional<job_row> library_index::job(std::uint64_t id) {
  std::lock_guard lock(mutex_);
  const std::string sql = std::string("SELECT ") + kJobColumns + " FROM jobs WHERE id = ?1";
  stmt s(db_, sql.c_str());
  s.bind(1, static_cast<std::int64_t>(id));
  if (!s.step_row()) return std::nullopt;
  return job_from(s);
}

std::vector<job_row> library_index::jobs(std::size_t limit) {
  std::lock_guard lock(mutex_);
  const std::string sql = std::string("SELECT ") + kJobColumns + " FROM jobs ORDER BY id DESC LIMIT ?1";
  stmt s(db_, sql.c_str());
  s.bind(1, static_cast<std::int64_t>(limit));
  std::vector<job_row> out;
  while (s.step_row()) out.push_back(job_from(s));
  return out;
}

std::vector<job_row> library_index::unfinished_jobs() {
  std::lock_guard lock(mutex_);
  // Queued, running, paused or interrupted (mv_import_job_state 1, 2, 3, 7)
  // with nothing recorded as finished: a crash, a pulled card, a closed app.
  const std::string sql = std::string("SELECT ") + kJobColumns +
                          " FROM jobs WHERE kind = 0 AND state IN (1, 2, 3, 7) ORDER BY id";
  stmt s(db_, sql.c_str());
  std::vector<job_row> out;
  while (s.step_row()) out.push_back(job_from(s));
  return out;
}

void library_index::journal_add(const std::vector<journal_row>& rows) {
  std::lock_guard lock(mutex_);
  (void)exec("BEGIN IMMEDIATE");
  {
    stmt s(db_,
           "INSERT OR REPLACE INTO journal(job, unit, member, src, rel, size, mtime, targets, "
           "target_roots, state, hash, reason, display) "
           "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, NULL, ?11, ?12)");
    for (const journal_row& r : rows) {
      s.reset();
      s.bind(1, static_cast<std::int64_t>(r.job)).bind(2, static_cast<std::int64_t>(r.unit))
          .bind(3, static_cast<std::int64_t>(r.member)).bind(4, r.src).bind(5, r.rel)
          .bind(6, static_cast<std::int64_t>(r.size)).bind(7, r.mtime)
          .bind(8, strings_json(r.targets)).bind(9, strings_json(r.target_roots))
          .bind(10, static_cast<std::int64_t>(r.state)).bind(11, r.reason).bind(12, r.display);
      s.run();
    }
  }
  (void)exec("COMMIT");
}

void library_index::journal_set(std::uint64_t job, std::uint32_t unit, std::uint32_t member,
                                member_state state, const digest* hash, const std::string& reason) {
  std::lock_guard lock(mutex_);
  if (hash) {
    stmt s(db_,
           "UPDATE journal SET state = ?4, hash = ?5, reason = ?6 WHERE job = ?1 AND unit = ?2 AND member = ?3");
    s.bind(1, static_cast<std::int64_t>(job)).bind(2, static_cast<std::int64_t>(unit))
        .bind(3, static_cast<std::int64_t>(member)).bind(4, static_cast<std::int64_t>(state))
        .bind(5, *hash).bind(6, reason).run();
  } else {
    stmt s(db_, "UPDATE journal SET state = ?4, reason = ?5 WHERE job = ?1 AND unit = ?2 AND member = ?3");
    s.bind(1, static_cast<std::int64_t>(job)).bind(2, static_cast<std::int64_t>(unit))
        .bind(3, static_cast<std::int64_t>(member)).bind(4, static_cast<std::int64_t>(state))
        .bind(5, reason).run();
  }
}

std::vector<journal_row> library_index::journal(std::uint64_t job) {
  std::lock_guard lock(mutex_);
  stmt s(db_,
         "SELECT job, unit, member, src, rel, size, mtime, targets, target_roots, state, hash, "
         "reason, display FROM journal WHERE job = ?1 ORDER BY unit, member");
  s.bind(1, static_cast<std::int64_t>(job));
  std::vector<journal_row> out;
  while (s.step_row()) {
    journal_row r;
    r.job = static_cast<std::uint64_t>(s.i64(0));
    r.unit = static_cast<std::uint32_t>(s.i64(1));
    r.member = static_cast<std::uint32_t>(s.i64(2));
    r.src = s.text(3);
    r.rel = s.text(4);
    r.size = static_cast<std::uint64_t>(s.i64(5));
    r.mtime = s.i64(6);
    r.targets = strings_from(s.text(7));
    r.target_roots = strings_from(s.text(8));
    r.state = static_cast<member_state>(s.i64(9));
    r.hash = s.hash(10);
    r.reason = s.text(11);
    r.display = s.text(12);
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace mv::import
