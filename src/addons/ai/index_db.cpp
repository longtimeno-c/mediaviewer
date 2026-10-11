// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/index_db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mv::ai {
namespace {

constexpr int kSchemaVersion = 3;

// A prepared statement that finalizes itself; binds are 1-based.
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
  stmt& bind_real(int i, double v) {
    sqlite3_bind_double(s_, i, v);
    return *this;
  }
  stmt& bind_blob(int i, const void* p, std::size_t n) {
    sqlite3_bind_blob(s_, i, p, static_cast<int>(n), SQLITE_TRANSIENT);
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
  [[nodiscard]] double real(int col) const { return sqlite3_column_double(s_, col); }
  [[nodiscard]] std::span<const std::int8_t> blob(int col) const {
    const void* p = sqlite3_column_blob(s_, col);
    const int n = sqlite3_column_bytes(s_, col);
    return p && n > 0 ? std::span<const std::int8_t>(static_cast<const std::int8_t*>(p), static_cast<std::size_t>(n))
                      : std::span<const std::int8_t>();
  }

 private:
  sqlite3_stmt* s_ = nullptr;
};

asset_row asset_from(const stmt& s, int c0) {
  asset_row a;
  a.id = s.i64(c0);
  a.path = s.text(c0 + 1);
  a.root_id = s.i64(c0 + 2);
  a.mtime = s.i64(c0 + 3);
  a.size = static_cast<std::uint64_t>(s.i64(c0 + 4));
  a.kind = s.i64(c0 + 5) == 2 ? asset_kind::video : asset_kind::photo;
  a.duration_ms = s.i64(c0 + 6);
  a.cloud = s.i64(c0 + 7) != 0;
  return a;
}

constexpr const char* kAssetCols = "a.id, a.path, a.root_id, a.mtime, a.size, a.kind, a.duration_ms, a.cloud";

}  // namespace

void quantise(std::span<const float> v, std::vector<std::int8_t>& q, float& scale) {
  float peak = 0;
  for (float x : v) peak = std::max(peak, std::fabs(x));
  scale = peak > 0 ? peak / 127.0f : 1.0f;
  q.resize(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) {
    const long r = std::lround(v[i] / scale);
    q[i] = static_cast<std::int8_t>(std::clamp<long>(r, -127, 127));
  }
}

index_db::~index_db() {
  if (db_) sqlite3_close(db_);
}

bool index_db::exec(const char* sql) {
  return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

result<std::unique_ptr<index_db>> index_db::open(const std::string& path) {
  std::unique_ptr<index_db> d(new index_db());
  d->path_ = path;
  if (sqlite3_open_v2(path.c_str(), &d->db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_busy_timeout(d->db_, 5000);
  if (!d->exec("PRAGMA journal_mode=WAL;") || !d->exec("PRAGMA synchronous=NORMAL;") ||
      !d->exec("PRAGMA foreign_keys=ON;")) {
    return err(status::io);
  }
  const char* schema =
      "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
      " recursive INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,"
      " last_scan_at INTEGER NOT NULL DEFAULT 0, media INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE IF NOT EXISTS assets(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
      " root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
      " mtime INTEGER NOT NULL, size INTEGER NOT NULL, kind INTEGER NOT NULL,"
      " duration_ms INTEGER NOT NULL DEFAULT 0, seen INTEGER NOT NULL DEFAULT 0,"
      " cloud INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS assets_root ON assets(root_id);"
      "CREATE TABLE IF NOT EXISTS progress(asset_id INTEGER NOT NULL REFERENCES assets(id)"
      " ON DELETE CASCADE, spec TEXT NOT NULL, state INTEGER NOT NULL,"
      " resume_ms INTEGER NOT NULL DEFAULT 0, tries INTEGER NOT NULL DEFAULT 0,"
      " indexed_at INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(asset_id, spec));"
      "CREATE TABLE IF NOT EXISTS frames(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL"
      " REFERENCES assets(id) ON DELETE CASCADE, spec TEXT NOT NULL, pts_ms INTEGER NOT NULL,"
      " pts_tb INTEGER NOT NULL, tb_num INTEGER NOT NULL, tb_den INTEGER NOT NULL,"
      " flags INTEGER NOT NULL, generic REAL NOT NULL, scale REAL NOT NULL, emb BLOB NOT NULL);"
      "CREATE INDEX IF NOT EXISTS frames_asset ON frames(asset_id, spec);"
      "CREATE INDEX IF NOT EXISTS frames_spec ON frames(spec);"
      // Settings' per-folder frame counts (frames_in_root) from the index alone,
      // not the rows with their vectors: 27 -> 8 ms at 89 k frames.
      "CREATE INDEX IF NOT EXISTS frames_spec_asset ON frames(spec, asset_id);"
      "CREATE TABLE IF NOT EXISTS speech(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL"
      " REFERENCES assets(id) ON DELETE CASCADE, spec TEXT NOT NULL, start_ms INTEGER NOT NULL,"
      " end_ms INTEGER NOT NULL, text TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS speech_asset ON speech(asset_id, spec);";
  if (!d->exec(schema)) return err(status::corrupt);
  std::string v = d->meta("schema");
  if (v == "1") {
    // Schema 1 -> 2 (audio): the roots gain their media choice; `speech` was
    // created above.
    if (!d->exec("ALTER TABLE roots ADD COLUMN media INTEGER NOT NULL DEFAULT 0;")) return err(status::corrupt);
    MV_TRY_VOID(d->set_meta("schema", "2"));
    v = "2";
  }
  if (v == "2") {
    // Schema 2 -> 3 (2026-10-05, cloud files): an asset only a cloud provider
    // has (OneDrive online-only, evicted iCloud Drive) is listed but never read
    // until the opt-in fetch brings it down. The next scan sets the mark.
    if (!d->exec("ALTER TABLE assets ADD COLUMN cloud INTEGER NOT NULL DEFAULT 0;")) return err(status::corrupt);
    MV_TRY_VOID(d->set_meta("schema", "3"));
    v = "3";
  }
  if (v.empty()) {
    MV_TRY_VOID(d->set_meta("schema", std::to_string(kSchemaVersion)));
  } else if (v != std::to_string(kSchemaVersion)) {
    return err(status::unsupported_format);  // a newer add-on's index
  }
  return d;
}

result<std::unique_ptr<index_db>> index_db::open_read_only(const std::string& path) {
  std::unique_ptr<index_db> d(new index_db());
  d->path_ = path;
  const int rc = sqlite3_open_v2(path.c_str(), &d->db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
  // No index yet (the app has not indexed anything): not_found, so a second
  // window says "nothing indexed yet" rather than "needs an update".
  if (rc == SQLITE_CANTOPEN) return err(status::not_found);
  if (rc != SQLITE_OK) return err(status::unsupported_format);
  sqlite3_busy_timeout(d->db_, 5000);
  // An index this build cannot read as it is (older or newer): the app
  // migrates it, never a reader.
  if (d->meta("schema") != std::to_string(kSchemaVersion)) return err(status::unsupported_format);
  return d;
}

std::int64_t index_db::data_version() {
  std::lock_guard lock(m_);
  stmt s(db_, "PRAGMA data_version");
  return s.step_row() ? s.i64(0) : 0;
}

std::vector<root_row> index_db::roots() {
  std::lock_guard lock(m_);
  std::vector<root_row> out;
  stmt s(db_, "SELECT id, path, recursive, enabled, last_scan_at, media FROM roots ORDER BY id");
  while (s.step_row()) {
    root_row r;
    r.id = s.i64(0);
    r.path = s.text(1);
    r.recursive = s.i64(2) != 0;
    r.enabled = s.i64(3) != 0;
    r.last_scan_at = s.i64(4);
    r.media = static_cast<std::uint32_t>(s.i64(5));
    out.push_back(std::move(r));
  }
  return out;
}

result<std::int64_t> index_db::add_root(const std::string& path, bool recursive) {
  std::lock_guard lock(m_);
  stmt s(db_, "INSERT INTO roots(path, recursive, enabled) VALUES(?1, ?2, 1)"
              " ON CONFLICT(path) DO UPDATE SET recursive = MAX(recursive, excluded.recursive),"
              " enabled = 1");
  if (!s.bind(1, path).bind(2, std::int64_t{recursive ? 1 : 0}).run()) return err(status::io);
  stmt q(db_, "SELECT id FROM roots WHERE path = ?1");
  if (!q.bind(1, path).step_row()) return err(status::io);
  return q.i64(0);
}

expected index_db::set_root_enabled(std::int64_t id, bool enabled) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE roots SET enabled = ?2 WHERE id = ?1");
  return s.bind(1, id).bind(2, std::int64_t{enabled ? 1 : 0}).run() ? expected{} : err(status::io);
}

expected index_db::set_root_recursive(std::int64_t id, bool recursive) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE roots SET recursive = ?2 WHERE id = ?1");
  return s.bind(1, id).bind(2, std::int64_t{recursive ? 1 : 0}).run() ? expected{} : err(status::io);
}

expected index_db::set_root_media(std::int64_t id, std::uint32_t media) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE roots SET media = ?2 WHERE id = ?1");
  return s.bind(1, id).bind(2, std::int64_t{media}).run() ? expected{} : err(status::io);
}

expected index_db::remove_root(std::int64_t id) {
  std::lock_guard lock(m_);
  stmt s(db_, "DELETE FROM roots WHERE id = ?1");  // cascades
  return s.bind(1, id).run() ? expected{} : err(status::io);
}

expected index_db::merge_root(std::int64_t from, std::int64_t to) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  stmt a(db_, "UPDATE assets SET root_id = ?2 WHERE root_id = ?1");
  stmt r(db_, "DELETE FROM roots WHERE id = ?1");
  const bool ok = a.bind(1, from).bind(2, to).run() && r.bind(1, from).run();
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::touch_root(std::int64_t id, std::int64_t when) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE roots SET last_scan_at = ?2 WHERE id = ?1");
  return s.bind(1, id).bind(2, when).run() ? expected{} : err(status::io);
}

std::int64_t index_db::next_generation() {
  const std::string v = meta("scan_generation");
  const std::int64_t next = (v.empty() ? 0 : std::stoll(v)) + 1;
  (void)set_meta("scan_generation", std::to_string(next));
  return next;
}

result<index_db::upsert> index_db::see_one(std::int64_t root, const seen_file& file,
                                            std::int64_t generation) {
  upsert u;
  stmt q(db_, "SELECT id, mtime, size FROM assets WHERE path = ?1");
  if (q.bind(1, file.path).step_row()) {
    u.id = q.i64(0);
    const bool same = q.i64(1) == file.mtime && static_cast<std::uint64_t>(q.i64(2)) == file.size;
    if (!same) {
      // Stale: edited or replaced. Its vectors describe other pixels.
      stmt f(db_, "DELETE FROM frames WHERE asset_id = ?1");
      stmt p(db_, "DELETE FROM progress WHERE asset_id = ?1");
      if (!f.bind(1, u.id).run() || !p.bind(1, u.id).run()) return err(status::io);
      u.changed = true;
    }
    stmt w(db_, "UPDATE assets SET mtime = ?2, size = ?3, kind = ?4, seen = ?5, cloud = ?6 WHERE id = ?1");
    if (!w.bind(1, u.id).bind(2, file.mtime).bind(3, static_cast<std::int64_t>(file.size))
             .bind(4, std::int64_t{static_cast<int>(file.kind)}).bind(5, generation)
             .bind(6, std::int64_t{file.cloud ? 1 : 0}).run()) {
      return err(status::io);
    }
    return u;
  }
  stmt ins(db_, "INSERT INTO assets(path, root_id, mtime, size, kind, seen, cloud) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  if (!ins.bind(1, file.path).bind(2, root).bind(3, file.mtime)
           .bind(4, static_cast<std::int64_t>(file.size))
           .bind(5, std::int64_t{static_cast<int>(file.kind)}).bind(6, generation)
           .bind(7, std::int64_t{file.cloud ? 1 : 0}).run()) {
    return err(status::io);
  }
  u.id = sqlite3_last_insert_rowid(db_);
  u.added = true;
  return u;
}

result<std::vector<index_db::upsert>> index_db::see_assets(std::int64_t root,
                                                           std::span<const seen_file> files,
                                                           std::int64_t generation) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  std::vector<upsert> out;
  out.reserve(files.size());
  for (const seen_file& f : files) {
    auto u = see_one(root, f, generation);
    if (!u) {
      exec("ROLLBACK");
      return err(u.error());
    }
    out.push_back(*u);
  }
  if (!exec("COMMIT")) return err(status::io);
  return out;
}

result<std::vector<std::int64_t>> index_db::end_scan(std::int64_t root, std::int64_t generation) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> gone;
  stmt q(db_, "SELECT id FROM assets WHERE root_id = ?1 AND seen != ?2");
  q.bind(1, root).bind(2, generation);
  while (q.step_row()) gone.push_back(q.i64(0));
  stmt d(db_, "DELETE FROM assets WHERE root_id = ?1 AND seen != ?2");
  if (!d.bind(1, root).bind(2, generation).run()) return err(status::io);
  return gone;
}

// The assets a track covers (track_filter), as SQL over `a` and `r`.
constexpr const char* kTrackWhere =
    " AND ((a.kind = 1 AND ?4 != 0) OR (a.kind = 2 AND"
    " ((CASE r.media WHEN 0 THEN ?5 ELSE r.media END) & ?6) != 0)) AND a.root_id != ?7";

std::vector<work_item> index_db::pending(const std::string& spec, std::size_t limit,
                                         std::int32_t max_tries, const track_filter& filter) {
  return pending_where(spec, limit, max_tries, filter, "");
}

std::vector<work_item> index_db::pending_among(const std::string& spec, std::span<const std::int64_t> ids,
                                               std::int32_t max_tries, const track_filter& filter) {
  if (ids.empty()) return {};
  // Integers only, so written into the SQL; a handful at most.
  std::string in = " AND a.id IN (";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i) in += ",";
    in += std::to_string(ids[i]);
  }
  in += ")";
  return pending_where(spec, ids.size(), max_tries, filter, in);
}

std::vector<work_item> index_db::pending_where(const std::string& spec, std::size_t limit, std::int32_t max_tries,
                                               const track_filter& filter, const std::string& extra) {
  std::lock_guard lock(m_);
  std::vector<work_item> out;
  const std::string sql = std::string("SELECT ") + kAssetCols +
      ", COALESCE(p.state, 0), COALESCE(p.resume_ms, 0), COALESCE(p.tries, 0)"
      " FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1"
      " LEFT JOIN progress p ON p.asset_id = a.id AND p.spec = ?1"
      " WHERE (p.state IS NULL OR p.state = 0 OR p.state = 1 OR (p.state = 3 AND p.tries < ?2))"
      " AND a.cloud = 0" +
      std::string(kTrackWhere) + extra +
      " ORDER BY COALESCE(p.state, 0) = 1 DESC, a.kind ASC, a.id ASC LIMIT ?3";
  stmt s(db_, sql.c_str());
  s.bind(1, spec).bind(2, std::int64_t{max_tries}).bind(3, static_cast<std::int64_t>(limit))
      .bind(4, std::int64_t{filter.photos ? 1 : 0}).bind(5, std::int64_t{filter.default_media})
      .bind(6, std::int64_t{filter.media_bit}).bind(7, filter.skip_root);
  while (s.step_row()) {
    work_item w;
    w.asset = asset_from(s, 0);
    w.state = static_cast<work_state>(s.i64(8));
    w.resume_ms = s.i64(9);
    w.tries = static_cast<std::int32_t>(s.i64(10));
    out.push_back(std::move(w));
  }
  return out;
}

expected index_db::set_duration(std::int64_t asset, std::int64_t duration_ms) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE assets SET duration_ms = ?2 WHERE id = ?1");
  return s.bind(1, asset).bind(2, duration_ms).run() ? expected{} : err(status::io);
}

expected index_db::commit_frames(std::int64_t asset, const std::string& spec,
                                 std::span<const frame_in> frames, work_state state,
                                 std::int64_t resume_ms, std::uint32_t dim) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    stmt ins(db_, "INSERT INTO frames(asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags,"
                  " generic, scale, emb) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    std::vector<std::int8_t> q;
    for (const frame_in& f : frames) {
      if (f.emb.size() != dim) {
        ok = false;
        break;
      }
      float scale = 1;
      quantise(f.emb, q, scale);
      ins.reset();
      ins.bind(1, asset).bind(2, spec).bind(3, f.pts_ms).bind(4, f.pts_tb)
          .bind(5, std::int64_t{f.tb_num}).bind(6, std::int64_t{f.tb_den})
          .bind(7, std::int64_t{f.flags}).bind_real(8, f.generic).bind_real(9, scale)
          .bind_blob(10, q.data(), q.size());
      if (!ins.run()) {
        ok = false;
        break;
      }
    }
    stmt p(db_, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
                " VALUES(?1, ?2, ?3, ?4, strftime('%s','now'))"
                " ON CONFLICT(asset_id, spec) DO UPDATE SET state = excluded.state,"
                " resume_ms = excluded.resume_ms, indexed_at = excluded.indexed_at");
    ok = ok && p.bind(1, asset).bind(2, spec).bind(3, std::int64_t{static_cast<int>(state)})
                   .bind(4, resume_ms).run();
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::fail(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt p(db_, "INSERT INTO progress(asset_id, spec, state, tries) VALUES(?1, ?2, 3, 1)"
              " ON CONFLICT(asset_id, spec) DO UPDATE SET state = 3, tries = tries + 1");
  return p.bind(1, asset).bind(2, spec).run() ? expected{} : err(status::io);
}

expected index_db::mark_unavailable(std::int64_t asset, const std::string& spec,
                                    std::span<const frame_in> frames, std::uint32_t dim) {
  if (!frames.empty()) return commit_frames(asset, spec, frames, work_state::unavailable, 0, dim);
  std::lock_guard lock(m_);
  stmt p(db_, "INSERT INTO progress(asset_id, spec, state, tries) VALUES(?1, ?2, 4, 0)"
              " ON CONFLICT(asset_id, spec) DO UPDATE SET state = 4");
  return p.bind(1, asset).bind(2, spec).run() ? expected{} : err(status::io);
}

std::vector<asset_row> index_db::unavailable_assets(std::int64_t root) {
  std::lock_guard lock(m_);
  std::vector<asset_row> out;
  const std::string sql = std::string("SELECT ") + kAssetCols +
      " FROM assets a WHERE a.root_id = ?1 AND EXISTS"
      " (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state = 4) ORDER BY a.id";
  stmt q(db_, sql.c_str());
  q.bind(1, root);
  while (q.step_row()) out.push_back(asset_from(q, 0));
  return out;
}

std::vector<asset_row> index_db::unavailable_videos(std::int64_t root, std::size_t limit) {
  std::lock_guard lock(m_);
  std::vector<asset_row> out;
  const std::string sql = std::string("SELECT ") + kAssetCols +
      " FROM assets a WHERE a.root_id = ?1 AND a.kind = 2 AND EXISTS"
      " (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state = 4) ORDER BY a.mtime DESC, a.id LIMIT ?2";
  stmt q(db_, sql.c_str());
  q.bind(1, root).bind(2, static_cast<std::int64_t>(limit));
  while (q.step_row()) out.push_back(asset_from(q, 0));
  return out;
}

std::uint64_t index_db::unavailable_video_count(std::int64_t root) {
  std::lock_guard lock(m_);
  stmt q(db_, "SELECT COUNT(*) FROM assets a WHERE a.root_id = ?1 AND a.kind = 2 AND EXISTS"
              " (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state = 4)");
  q.bind(1, root);
  return q.step_row() ? static_cast<std::uint64_t>(q.i64(0)) : 0;
}

std::vector<asset_row> index_db::cloud_assets(std::size_t limit, std::span<const std::int64_t> skip) {
  std::lock_guard lock(m_);
  std::vector<asset_row> out;
  // Integers only, so written into the SQL.
  std::string not_in;
  for (std::size_t i = 0; i < skip.size(); ++i) not_in += (i ? "," : "") + std::to_string(skip[i]);
  // Not yet indexed for anything: a cloud file done for pictures (fetched once
  // and given back) is not fetched again for a later model.
  const std::string sql = std::string("SELECT ") + kAssetCols +
      " FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1 WHERE a.cloud = 1"
      " AND NOT EXISTS (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state >= 2)" +
      (not_in.empty() ? std::string() : " AND a.id NOT IN (" + not_in + ")") +
      " ORDER BY a.mtime DESC, a.id LIMIT ?1";
  stmt q(db_, sql.c_str());
  q.bind(1, static_cast<std::int64_t>(limit));
  while (q.step_row()) out.push_back(asset_from(q, 0));
  return out;
}

std::uint64_t index_db::cloud_count() {
  std::lock_guard lock(m_);
  stmt q(db_, "SELECT COUNT(*) FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1 WHERE a.cloud = 1"
              " AND NOT EXISTS (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state >= 2)");
  return q.step_row() ? static_cast<std::uint64_t>(q.i64(0)) : 0;
}

std::uint64_t index_db::cloud_in_root(std::int64_t root) {
  std::lock_guard lock(m_);
  stmt q(db_, "SELECT COUNT(*) FROM assets a WHERE a.root_id = ?1 AND a.cloud = 1"
              " AND NOT EXISTS (SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.state >= 2)");
  q.bind(1, root);
  return q.step_row() ? static_cast<std::uint64_t>(q.i64(0)) : 0;
}

expected index_db::set_cloud(std::int64_t asset, bool cloud) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE assets SET cloud = ?2 WHERE id = ?1");
  return s.bind(1, asset).bind(2, std::int64_t{cloud ? 1 : 0}).run() ? expected{} : err(status::io);
}

expected index_db::requeue_unavailable(std::span<const std::int64_t> ids) {
  if (ids.empty()) return {};
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    stmt f(db_, "DELETE FROM frames WHERE asset_id = ?1 AND spec IN"
                " (SELECT spec FROM progress WHERE asset_id = ?1 AND state = 4)");
    stmt p(db_, "DELETE FROM progress WHERE asset_id = ?1 AND state = 4");
    for (std::int64_t id : ids) {
      f.reset();
      p.reset();
      if (!f.bind(1, id).run() || !p.bind(1, id).run()) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::commit_speech(std::int64_t asset, const std::string& spec,
                                 std::span<const speech_in> segments, work_state state,
                                 std::int64_t resume_ms) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    stmt ins(db_, "INSERT INTO speech(asset_id, spec, start_ms, end_ms, text) VALUES(?1, ?2, ?3, ?4, ?5)");
    for (const speech_in& s : segments) {
      ins.reset();
      ins.bind(1, asset).bind(2, spec).bind(3, s.start_ms).bind(4, s.end_ms).bind(5, s.text);
      if (!ins.run()) {
        ok = false;
        break;
      }
    }
    stmt p(db_, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
                " VALUES(?1, ?2, ?3, ?4, strftime('%s','now'))"
                " ON CONFLICT(asset_id, spec) DO UPDATE SET state = excluded.state,"
                " resume_ms = excluded.resume_ms, indexed_at = excluded.indexed_at");
    ok = ok && p.bind(1, asset).bind(2, spec).bind(3, std::int64_t{static_cast<int>(state)}).bind(4, resume_ms).run();
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::each_speech(
    const std::string& spec,
    const std::function<void(std::int64_t, std::int64_t, std::int64_t, const std::string&)>& visit) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT asset_id, start_ms, end_ms, text FROM speech WHERE spec = ?1 ORDER BY asset_id, start_ms");
  if (!s.ok()) return err(status::io);
  s.bind(1, spec);
  while (s.step_row()) visit(s.i64(0), s.i64(1), s.i64(2), s.text(3));
  return {};
}

expected index_db::drop_spec(const std::string& spec) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  stmt f(db_, "DELETE FROM frames WHERE spec = ?1");
  stmt p(db_, "DELETE FROM progress WHERE spec = ?1");
  stmt sp(db_, "DELETE FROM speech WHERE spec = ?1");
  if (!f.bind(1, spec).run() || !p.bind(1, spec).run() || !sp.bind(1, spec).run()) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

counts index_db::count(const std::string& spec, const track_filter& filter) {
  std::lock_guard lock(m_);
  counts c;
  const std::string sql = std::string(
      "SELECT COUNT(*),"
      " SUM(CASE WHEN p.state = 2 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN p.state = 3 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN a.kind = 2 AND a.cloud = 0 AND (p.state IS NULL OR p.state < 2)"
      "     THEN MAX(a.duration_ms - COALESCE(p.resume_ms, 0), 0) ELSE 0 END),"
      " SUM(CASE WHEN a.kind = 1 AND a.cloud = 0 AND (p.state IS NULL OR p.state < 2) THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN p.state = 4 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN a.cloud = 1 AND (p.state IS NULL OR p.state < 2) THEN 1 ELSE 0 END)"
      " FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1"
      " LEFT JOIN progress p ON p.asset_id = a.id AND p.spec = ?1 WHERE 1") + kTrackWhere;
  stmt a(db_, sql.c_str());
  a.bind(4, std::int64_t{filter.photos ? 1 : 0}).bind(5, std::int64_t{filter.default_media})
      .bind(6, std::int64_t{filter.media_bit}).bind(7, filter.skip_root);
  if (a.bind(1, spec).step_row()) {
    c.assets = static_cast<std::uint64_t>(a.i64(0));
    c.done = static_cast<std::uint64_t>(a.i64(1));
    c.failed = static_cast<std::uint64_t>(a.i64(2));
    c.pending_video_ms = static_cast<std::uint64_t>(a.i64(3));
    c.pending_photos = static_cast<std::uint64_t>(a.i64(4));
    c.unavailable = static_cast<std::uint64_t>(a.i64(5));
    c.cloud_only = static_cast<std::uint64_t>(a.i64(6));
  }
  stmt f(db_, "SELECT COUNT(*) FROM frames WHERE spec = ?1");
  if (f.bind(1, spec).step_row()) c.frames = static_cast<std::uint64_t>(f.i64(0));
  return c;
}

std::uint64_t index_db::frames_in_root(std::int64_t root, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM frames f JOIN assets a ON a.id = f.asset_id"
              " WHERE a.root_id = ?1 AND f.spec = ?2");
  return f.bind(1, root).bind(2, spec).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::assets_in_root(std::int64_t root) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM assets WHERE root_id = ?1");
  return f.bind(1, root).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::unavailable_in_root(std::int64_t root, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM assets a JOIN progress p ON p.asset_id = a.id AND p.spec = ?2"
              " WHERE a.root_id = ?1 AND p.state = 4");
  return f.bind(1, root).bind(2, spec).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::done_in_root(std::int64_t root, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM assets a JOIN progress p ON p.asset_id = a.id AND p.spec = ?2"
              " WHERE a.root_id = ?1 AND p.state >= 2");
  return f.bind(1, root).bind(2, spec).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

expected index_db::each_frame(const std::string& spec, const std::function<void(const frame_out&)>& visit,
                              std::int64_t after_id) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT id, asset_id, pts_ms, generic, scale, emb FROM frames WHERE spec = ?1 AND id > ?2 ORDER BY id");
  if (!s.ok()) return err(status::io);
  s.bind(1, spec).bind(2, after_id);
  while (s.step_row()) {
    frame_out f;
    f.id = s.i64(0);
    f.asset_id = s.i64(1);
    f.pts_ms = s.i64(2);
    f.generic = static_cast<float>(s.real(3));
    f.scale = static_cast<float>(s.real(4));
    f.emb = s.blob(5);
    visit(f);
  }
  return {};
}

expected index_db::each_frame_of(std::int64_t asset, const std::string& spec,
                                 const std::function<void(const frame_out&)>& visit) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT id, asset_id, pts_ms, generic, scale, emb FROM frames"
              " WHERE asset_id = ?1 AND spec = ?2 ORDER BY pts_ms");
  if (!s.ok()) return err(status::io);
  s.bind(1, asset).bind(2, spec);
  while (s.step_row()) {
    frame_out f;
    f.id = s.i64(0);
    f.asset_id = s.i64(1);
    f.pts_ms = s.i64(2);
    f.generic = static_cast<float>(s.real(3));
    f.scale = static_cast<float>(s.real(4));
    f.emb = s.blob(5);
    visit(f);
  }
  return {};
}

std::vector<asset_row> index_db::all_assets() {
  std::lock_guard lock(m_);
  std::vector<asset_row> out;
  const std::string sql = std::string("SELECT ") + kAssetCols + " FROM assets a";
  stmt s(db_, sql.c_str());
  while (s.step_row()) out.push_back(asset_from(s, 0));
  return out;
}

result<asset_row> index_db::asset_by_path(const std::string& path) {
  std::lock_guard lock(m_);
  const std::string sql = std::string("SELECT ") + kAssetCols + " FROM assets a WHERE a.path = ?1";
  stmt s(db_, sql.c_str());
  if (!s.bind(1, path).step_row()) return err(status::invalid_arg);
  return asset_from(s, 0);
}

result<asset_row> index_db::asset_by_id(std::int64_t id) {
  std::lock_guard lock(m_);
  const std::string sql = std::string("SELECT ") + kAssetCols + " FROM assets a WHERE a.id = ?1";
  stmt s(db_, sql.c_str());
  if (!s.bind(1, id).step_row()) return err(status::invalid_arg);
  return asset_from(s, 0);
}

std::vector<std::string> index_db::specs() {
  std::lock_guard lock(m_);
  std::vector<std::string> out;
  stmt s(db_, "SELECT DISTINCT spec FROM progress");
  while (s.step_row()) out.push_back(s.text(0));
  return out;
}

std::string index_db::meta(const std::string& key) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT value FROM meta WHERE key = ?1");
  return s.bind(1, key).step_row() ? s.text(0) : std::string();
}

expected index_db::set_meta(const std::string& key, const std::string& value) {
  std::lock_guard lock(m_);
  stmt s(db_, "INSERT INTO meta(key, value) VALUES(?1, ?2)"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  return s.bind(1, key).bind(2, value).run() ? expected{} : err(status::io);
}

expected index_db::clear() {
  std::lock_guard lock(m_);
  if (!exec("DELETE FROM speech; DELETE FROM frames; DELETE FROM progress; DELETE FROM assets; DELETE FROM roots;")) {
    return err(status::io);
  }
  // Give the space back (docs/design/17 PR 23 verify: "clearing the index frees the disk").
  exec("PRAGMA wal_checkpoint(TRUNCATE);");
  if (!exec("VACUUM;")) return err(status::io);
  exec("PRAGMA wal_checkpoint(TRUNCATE);");
  return {};
}

expected index_db::with_connection(const std::function<expected(sqlite3*)>& fn) {
  std::lock_guard lock(m_);
  return fn(db_);
}

std::uint64_t index_db::bytes() {
  std::lock_guard lock(m_);
  std::uint64_t n = 0;
  stmt pc(db_, "PRAGMA page_count");
  stmt ps(db_, "PRAGMA page_size");
  if (pc.step_row() && ps.step_row()) n = static_cast<std::uint64_t>(pc.i64(0)) * static_cast<std::uint64_t>(ps.i64(0));
  return n;
}

}  // namespace mv::ai
