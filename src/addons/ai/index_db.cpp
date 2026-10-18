// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/index_db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mv::ai {
namespace {

constexpr int kSchemaVersion = 10;

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
  [[nodiscard]] bool is_null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
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
  const std::int64_t kind = s.i64(c0 + 5);
  a.kind = kind == 2   ? asset_kind::video
           : kind == 3 ? asset_kind::document
           : kind == 4 ? asset_kind::audio
                       : asset_kind::photo;
  a.duration_ms = s.i64(c0 + 6);
  a.cloud = s.i64(c0 + 7) != 0;
  a.motion = s.text(c0 + 8);
  a.motion_mtime = s.i64(c0 + 9);
  a.motion_size = static_cast<std::uint64_t>(s.i64(c0 + 10));
  a.motion_cloud = s.i64(c0 + 11) != 0;
  return a;
}

// Whether `table` already has `column` (PRAGMA table_info).
bool has_column(sqlite3* db, const char* table, const char* column) {
  stmt s(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
  while (s.step_row()) {
    if (s.text(1) == column) return true;
  }
  return false;
}

constexpr const char* kAssetCols =
    "a.id, a.path, a.root_id, a.mtime, a.size, a.kind, a.duration_ms, a.cloud,"
    " COALESCE(a.motion, ''), COALESCE(a.motion_mtime, 0), COALESCE(a.motion_size, 0), a.motion_cloud";
constexpr int kAssetColCount = 12;

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
  // A new index is made as schema 3 above and takes the same 3 -> 4 step as an
  // old one: one path, so a fresh index and a migrated one cannot differ.
  if (v.empty()) {
    MV_TRY_VOID(d->set_meta("schema", "3"));
    v = "3";
  }
  // Each step is one transaction, and a column already there is not added
  // again: an index a crash left between the ALTER and the schema bump (as
  // earlier builds could) migrates instead of failing every later open.
  const auto add_column = [&](const char* table, const char* column, const char* sql, const char* to) {
    if (!d->exec("BEGIN")) return false;
    const bool ok = (has_column(d->db_, table, column) || d->exec(sql)) && d->set_meta("schema", to);
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return false;
    }
    v = to;
    return true;
  };
  // Schema 1 -> 2 (audio): the roots gain their media choice; `speech` was
  // created above.
  if (v == "1" &&
      !add_column("roots", "media", "ALTER TABLE roots ADD COLUMN media INTEGER NOT NULL DEFAULT 0;", "2")) {
    return err(status::corrupt);
  }
  // Schema 2 -> 3 (2026-10-05, cloud files): an asset only a cloud provider
  // has (OneDrive online-only, evicted iCloud Drive) is listed but never read
  // until the opt-in fetch brings it down. The next scan sets the mark.
  if (v == "2" &&
      !add_column("assets", "cloud", "ALTER TABLE assets ADD COLUMN cloud INTEGER NOT NULL DEFAULT 0;", "3")) {
    return err(status::corrupt);
  }
  // Schema 3 -> 4 (#291, docs/design/17 "Schema 4"): what each asset's tags say
  // (date taken, position, camera, lens, made-with, the AI flag) and an FTS5
  // `asset_text` of its folded search fields. `tags` = 0 until the backfill
  // has read them (a changed file goes back to 0). place / country / region
  // stay NULL: place names are the Places add-on's, answered at query time.
  // One transaction: a kill anywhere in it leaves schema 3 as it was.
  if (v == "3") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    bool ok = true;
    static constexpr std::pair<const char*, const char*> kColumns[] = {
        {"tags", "ALTER TABLE assets ADD COLUMN tags INTEGER NOT NULL DEFAULT 0;"},
        {"taken_at", "ALTER TABLE assets ADD COLUMN taken_at INTEGER;"},
        {"lat", "ALTER TABLE assets ADD COLUMN lat REAL;"},
        {"lon", "ALTER TABLE assets ADD COLUMN lon REAL;"},
        {"camera", "ALTER TABLE assets ADD COLUMN camera TEXT;"},
        {"lens", "ALTER TABLE assets ADD COLUMN lens TEXT;"},
        {"made_with", "ALTER TABLE assets ADD COLUMN made_with TEXT;"},
        {"ai", "ALTER TABLE assets ADD COLUMN ai INTEGER NOT NULL DEFAULT 0;"},
        {"place", "ALTER TABLE assets ADD COLUMN place TEXT;"},
        {"country", "ALTER TABLE assets ADD COLUMN country TEXT;"},
        {"region", "ALTER TABLE assets ADD COLUMN region TEXT;"},
    };
    for (const auto& [column, sql] : kColumns) {
      if (ok && !has_column(d->db_, "assets", column)) ok = d->exec(sql);
    }
    if (ok) {
      // FTS5 when this SQLite has it (the vcpkg port's fts5 feature); else a
      // plain table of the same shape, matched by the engine in memory either
      // way. meta('asset_text') says which.
      const bool fts = d->exec("CREATE VIRTUAL TABLE IF NOT EXISTS asset_text USING fts5(text,"
                               " tokenize = 'unicode61 remove_diacritics 2');");
      ok = fts || d->exec("CREATE TABLE IF NOT EXISTS asset_text(rowid INTEGER PRIMARY KEY, text TEXT NOT NULL);");
      ok = ok && static_cast<bool>(d->set_meta("asset_text", fts ? "fts5" : "plain"));
    }
    ok = ok && d->exec("CREATE INDEX IF NOT EXISTS assets_tags ON assets(tags);") &&
         d->exec("CREATE TRIGGER IF NOT EXISTS assets_text_gone AFTER DELETE ON assets"
                 " BEGIN DELETE FROM asset_text WHERE rowid = old.id; END;") &&
         static_cast<bool>(d->set_meta("schema", "4"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "4";
  }
  // Schema 4 -> 5 (#405): rating, colour label and exposure, NULL until the
  // backfill reads them (`tags` < 2 brings every schema-4 row back to it
  // once, for these alone: nothing is re-embedded). One transaction; a column
  // already there is skipped, as for schema 4.
  if (v == "4") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    bool ok = true;
    static constexpr std::pair<const char*, const char*> kColumns[] = {
        {"rating", "ALTER TABLE assets ADD COLUMN rating INTEGER;"},
        {"label", "ALTER TABLE assets ADD COLUMN label TEXT;"},
        {"iso", "ALTER TABLE assets ADD COLUMN iso INTEGER;"},
        {"aperture_x100", "ALTER TABLE assets ADD COLUMN aperture_x100 INTEGER;"},
        {"focal_mm", "ALTER TABLE assets ADD COLUMN focal_mm INTEGER;"},
        {"shutter_den", "ALTER TABLE assets ADD COLUMN shutter_den INTEGER;"},
        {"shutter_us", "ALTER TABLE assets ADD COLUMN shutter_us INTEGER;"},
    };
    for (const auto& [column, sql] : kColumns) {
      if (ok && !has_column(d->db_, "assets", column)) ok = d->exec(sql);
    }
    ok = ok && static_cast<bool>(d->set_meta("schema", "5"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "5";
  }
  // Schema 5 -> 6 (#406): a clip's own text (chapter titles, text subtitle
  // lines). A table, not speech rows under a Whisper spec: a shared index must
  // not disagree about what was said, and removing the audio piece keeps them.
  if (v == "5") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    const bool ok =
        d->exec("CREATE TABLE IF NOT EXISTS cues(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL"
                " REFERENCES assets(id) ON DELETE CASCADE, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL,"
                " kind INTEGER NOT NULL, text TEXT NOT NULL);") &&
        d->exec("CREATE INDEX IF NOT EXISTS cues_asset ON cues(asset_id, start_ms);") &&
        static_cast<bool>(d->set_meta("schema", "6"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "6";
  }
  // Schema 6 -> 7 (#407): a Live Photo still's paired movie and its stamp, so
  // the motion is sampled onto the still's id. NULL until the next walk sees
  // the pair; that walk queues the motion alone (see_assets).
  if (v == "6") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    bool ok = true;
    static constexpr std::pair<const char*, const char*> kColumns[] = {
        {"motion", "ALTER TABLE assets ADD COLUMN motion TEXT;"},
        {"motion_mtime", "ALTER TABLE assets ADD COLUMN motion_mtime INTEGER;"},
        {"motion_size", "ALTER TABLE assets ADD COLUMN motion_size INTEGER;"},
        {"motion_cloud", "ALTER TABLE assets ADD COLUMN motion_cloud INTEGER NOT NULL DEFAULT 0;"},
    };
    for (const auto& [column, sql] : kColumns) {
      if (ok && !has_column(d->db_, "assets", column)) ok = d->exec(sql);
    }
    ok = ok && static_cast<bool>(d->set_meta("schema", "7"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "7";
  }
  // Schema 7 -> 8 (#355): no table change. assets.kind may now be 4, an audio
  // file (MP3, M4A, M4P); the bump keeps an older pack, which would read kind 4
  // as a photo, off this index.
  if (v == "7") {
    MV_TRY_VOID(d->set_meta("schema", "8"));
    v = "8";
  }
  // Schema 8 -> 9 (#354, documents): assets.kind may be 3, a PDF or DOCX; the
  // roots gain their Documents choice (MV_AI_DOCS_*: 0 follows the setting);
  // `passages` holds a document's text a few sentences at a time with the
  // page that shows it, and `passage_text` its folded words for the keyword
  // search (FTS5 when this SQLite has it, rowid = passage id; else a plain
  // table the engine scans). A deleted passage takes its words with it (the
  // trigger fires on the cascade from assets too). One transaction.
  if (v == "8") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    bool ok = has_column(d->db_, "roots", "docs") ||
              d->exec("ALTER TABLE roots ADD COLUMN docs INTEGER NOT NULL DEFAULT 0;");
    ok = ok &&
         d->exec("CREATE TABLE IF NOT EXISTS passages(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL"
                 " REFERENCES assets(id) ON DELETE CASCADE, spec TEXT NOT NULL, page INTEGER NOT NULL,"
                 " ord INTEGER NOT NULL, text TEXT NOT NULL);") &&
         d->exec("CREATE INDEX IF NOT EXISTS passages_asset ON passages(asset_id, spec, page);");
    if (ok) {
      const bool fts = d->exec("CREATE VIRTUAL TABLE IF NOT EXISTS passage_text USING fts5(words,"
                               " tokenize = 'unicode61 remove_diacritics 2');");
      ok = fts || d->exec("CREATE TABLE IF NOT EXISTS passage_text(rowid INTEGER PRIMARY KEY, words TEXT NOT NULL);");
      ok = ok && static_cast<bool>(d->set_meta("passage_text", fts ? "fts5" : "plain"));
    }
    ok = ok &&
         d->exec("CREATE TRIGGER IF NOT EXISTS passages_gone AFTER DELETE ON passages"
                 " BEGIN DELETE FROM passage_text WHERE rowid = old.id; END;") &&
         static_cast<bool>(d->set_meta("schema", "9"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "9";
  }
  // Schema 9 -> 10 (#354 D3): a passage's vector under the Documents piece's
  // text model, int8 like frames. A passage that goes takes its vectors.
  if (v == "9") {
    if (!d->exec("BEGIN")) return err(status::corrupt);
    const bool ok =
        d->exec("CREATE TABLE IF NOT EXISTS passage_vecs(passage_id INTEGER NOT NULL REFERENCES passages(id)"
                " ON DELETE CASCADE, spec TEXT NOT NULL, generic REAL NOT NULL, scale REAL NOT NULL,"
                " emb BLOB NOT NULL, PRIMARY KEY(passage_id, spec)) WITHOUT ROWID;") &&
        d->exec("CREATE INDEX IF NOT EXISTS passage_vecs_spec ON passage_vecs(spec, passage_id);") &&
        static_cast<bool>(d->set_meta("schema", "10"));
    if (!ok || !d->exec("COMMIT")) {
      d->exec("ROLLBACK");
      return err(status::corrupt);
    }
    v = "10";
  }
  if (v != std::to_string(kSchemaVersion)) {
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
  stmt s(db_, "SELECT id, path, recursive, enabled, last_scan_at, media, docs FROM roots ORDER BY id");
  while (s.step_row()) {
    root_row r;
    r.id = s.i64(0);
    r.path = s.text(1);
    r.recursive = s.i64(2) != 0;
    r.enabled = s.i64(3) != 0;
    r.last_scan_at = s.i64(4);
    r.media = static_cast<std::uint32_t>(s.i64(5));
    r.docs = static_cast<std::uint32_t>(s.i64(6));
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

expected index_db::set_root_docs(std::int64_t id, std::uint32_t docs) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE roots SET docs = ?2 WHERE id = ?1");
  return s.bind(1, id).bind(2, std::int64_t{docs}).run() ? expected{} : err(status::io);
}

expected index_db::remove_root(std::int64_t id) {
  std::lock_guard lock(m_);
  stmt s(db_, "DELETE FROM roots WHERE id = ?1");  // cascades
  return s.bind(1, id).run() ? expected{} : err(status::io);
}

result<std::vector<std::int64_t>> index_db::remove_kind(asset_kind kind) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> gone;
  if (!exec("BEGIN")) return err(status::io);
  {
    stmt q(db_, "SELECT id FROM assets WHERE kind = ?1");
    q.bind(1, std::int64_t{static_cast<int>(kind)});
    while (q.step_row()) gone.push_back(q.i64(0));
  }
  // Frames, speech, cues and progress cascade; asset_text goes by its trigger.
  stmt d(db_, "DELETE FROM assets WHERE kind = ?1");
  if (!d.bind(1, std::int64_t{static_cast<int>(kind)}).run()) {
    exec("ROLLBACK");
    return err(status::io);
  }
  if (!exec("COMMIT")) return err(status::io);
  return gone;
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

result<std::vector<index_db::upsert>> index_db::see_assets(std::int64_t root,
                                                           std::span<const seen_file> files,
                                                           std::int64_t generation) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  std::vector<upsert> out;
  out.reserve(files.size());
  bool ok = true;
  {
    // Prepared once per batch and reset per file, not prepared per file.
    stmt q(db_, "SELECT id, mtime, size, COALESCE(motion, ''), COALESCE(motion_mtime, 0),"
                " COALESCE(motion_size, 0), motion_cloud FROM assets WHERE path = ?1");
    stmt f(db_, "DELETE FROM frames WHERE asset_id = ?1");
    stmt p(db_, "DELETE FROM progress WHERE asset_id = ?1");
    stmt c(db_, "DELETE FROM cues WHERE asset_id = ?1");
    stmt pg(db_, "DELETE FROM passages WHERE asset_id = ?1");
    // #407: a Live Photo's movie changed under the same still: its frames go
    // and the picture pass comes back for the motion alone.
    stmt mf(db_, "DELETE FROM frames WHERE asset_id = ?1 AND pts_ms >= 0");
    stmt mp(db_, "UPDATE progress SET state = 1, resume_ms = 0 WHERE asset_id = ?1 AND state = 2");
    stmt w(db_, "UPDATE assets SET mtime = ?2, size = ?3, kind = ?4, seen = ?5, cloud = ?6,"
                " tags = CASE WHEN mtime = ?2 AND size = ?3 THEN tags ELSE 0 END,"
                " motion = ?7, motion_mtime = ?8, motion_size = ?9, motion_cloud = ?10 WHERE id = ?1");
    stmt ins(db_, "INSERT INTO assets(path, root_id, mtime, size, kind, seen, cloud, motion, motion_mtime,"
                  " motion_size, motion_cloud) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
    const auto bind_motion = [](stmt& s, int at, const seen_file& file) {
      if (!file.motion.empty()) {
        s.bind(at, file.motion).bind(at + 1, file.motion_mtime).bind(at + 2, static_cast<std::int64_t>(file.motion_size));
      }
      s.bind(at + 3, std::int64_t{file.motion_cloud ? 1 : 0});
    };
    for (const seen_file& file : files) {
      upsert u;
      q.reset();
      if (q.bind(1, file.path).step_row()) {
        u.id = q.i64(0);
        const bool same = q.i64(1) == file.mtime && static_cast<std::uint64_t>(q.i64(2)) == file.size;
        const bool same_motion = q.text(3) == file.motion && q.i64(4) == (file.motion.empty() ? 0 : file.motion_mtime) &&
                                 static_cast<std::uint64_t>(q.i64(5)) == (file.motion.empty() ? 0 : file.motion_size) &&
                                 (q.i64(6) != 0) == file.motion_cloud;
        if (same && !same_motion) {
          mf.reset();
          mp.reset();
          if (!mf.bind(1, u.id).run() || !mp.bind(1, u.id).run()) {
            ok = false;
            break;
          }
          u.motion_changed = true;
        }
        if (!same) {
          // Stale: edited or replaced. Its vectors describe other pixels.
          f.reset();
          p.reset();
          c.reset();
          pg.reset();
          if (!f.bind(1, u.id).run() || !p.bind(1, u.id).run() || !c.bind(1, u.id).run() ||
              !pg.bind(1, u.id).run()) {
            ok = false;
            break;
          }
          u.changed = true;
        }
        w.reset();
        w.bind(1, u.id).bind(2, file.mtime).bind(3, static_cast<std::int64_t>(file.size))
            .bind(4, std::int64_t{static_cast<int>(file.kind)}).bind(5, generation)
            .bind(6, std::int64_t{file.cloud ? 1 : 0});
        bind_motion(w, 7, file);
        if (!w.run()) {
          ok = false;
          break;
        }
      } else {
        ins.reset();
        ins.bind(1, file.path).bind(2, root).bind(3, file.mtime)
            .bind(4, static_cast<std::int64_t>(file.size))
            .bind(5, std::int64_t{static_cast<int>(file.kind)}).bind(6, generation)
            .bind(7, std::int64_t{file.cloud ? 1 : 0});
        bind_motion(ins, 8, file);
        if (!ins.run()) {
          ok = false;
          break;
        }
        u.id = sqlite3_last_insert_rowid(db_);
        u.added = true;
      }
      out.push_back(u);
    }
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
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

// The assets a track covers (track_filter), as SQL over `a` and `r`. An audio
// file (#355) is on the picture track alone (its cover art), as a still while
// its root's media takes pictures; its sound and speech wait for slice A2.
//
// A document (#354) is on a track only when the filter asks for documents
// (?9 = track_filter::docs_min, 0: none) and its root's Documents choice (or
// the setting, ?10, for 0) is at least that. ?9 and ?10 unbound read as NULL:
// no document, which keeps every older statement as it was.
constexpr const char* kTrackWhere =
    " AND ((a.kind = 1 AND ?4 != 0) OR (a.kind = 2 AND"
    " ((CASE r.media WHEN 0 THEN ?5 ELSE r.media END) & ?6) != 0)"
    " OR (a.kind = 4 AND ?4 != 0 AND (?6 & 1) != 0 AND"
    " ((CASE r.media WHEN 0 THEN ?5 ELSE r.media END) & 1) != 0)"
    " OR (a.kind = 3 AND ?9 > 0 AND (CASE r.docs WHEN 0 THEN ?10 ELSE r.docs - 1 END) >= ?9))"
    " AND a.root_id != ?7";

// The filter's held roots, as SQL over `a` (integers only, so written in).
static std::string held_where(const track_filter& filter) {
  if (filter.held.empty()) return {};
  std::string in = " AND a.root_id NOT IN (";
  for (std::size_t i = 0; i < filter.held.size(); ++i) {
    if (i) in += ",";
    in += std::to_string(filter.held[i]);
  }
  return in + ")";
}

// pending()'s `after` spec, as SQL over `a`: bound as ?8.
static std::string after_where(const std::string& after) {
  if (after.empty()) return {};
  return " AND EXISTS(SELECT 1 FROM progress q WHERE q.asset_id = a.id AND q.spec = ?8 AND q.state = 2)";
}

// pending()'s rule for "still to do", as SQL over `a` and `p`.
constexpr const char* kPendingWhere =
    " (p.state IS NULL OR p.state = 0 OR p.state = 1 OR (p.state = 3 AND p.tries < ?2)) AND a.cloud = 0";

std::vector<work_item> index_db::pending(const std::string& spec, std::size_t limit,
                                         std::int32_t max_tries, const track_filter& filter,
                                         const std::string& after) {
  return pending_where(spec, limit, max_tries, filter, "", after);
}

std::vector<work_item> index_db::pending_among(const std::string& spec, std::span<const std::int64_t> ids,
                                               std::int32_t max_tries, const track_filter& filter,
                                               const std::string& after) {
  if (ids.empty()) return {};
  // Integers only, so written into the SQL; a handful at most.
  std::string in = " AND a.id IN (";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i) in += ",";
    in += std::to_string(ids[i]);
  }
  in += ")";
  return pending_where(spec, ids.size(), max_tries, filter, in, after);
}

std::vector<work_item> index_db::pending_where(const std::string& spec, std::size_t limit, std::int32_t max_tries,
                                               const track_filter& filter, const std::string& extra,
                                               const std::string& after) {
  std::lock_guard lock(m_);
  std::vector<work_item> out;
  const std::string sql = std::string("SELECT ") + kAssetCols +
      ", COALESCE(p.state, 0), COALESCE(p.resume_ms, 0), COALESCE(p.tries, 0)"
      " FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1"
      " LEFT JOIN progress p ON p.asset_id = a.id AND p.spec = ?1"
      " WHERE" + std::string(kPendingWhere) +
      std::string(kTrackWhere) + held_where(filter) + after_where(after) + extra +
      " ORDER BY COALESCE(p.state, 0) = 1 DESC, a.kind ASC, a.id ASC LIMIT ?3";
  stmt s(db_, sql.c_str());
  s.bind(1, spec).bind(2, std::int64_t{max_tries}).bind(3, static_cast<std::int64_t>(limit))
      .bind(4, std::int64_t{filter.photos ? 1 : 0}).bind(5, std::int64_t{filter.default_media})
      .bind(6, std::int64_t{filter.media_bit}).bind(7, filter.skip_root)
      .bind(9, std::int64_t{filter.docs_min}).bind(10, std::int64_t{filter.default_docs});
  if (!after.empty()) s.bind(8, after);
  while (s.step_row()) {
    work_item w;
    w.asset = asset_from(s, 0);
    w.state = static_cast<work_state>(s.i64(kAssetColCount));
    w.resume_ms = s.i64(kAssetColCount + 1);
    w.tries = static_cast<std::int32_t>(s.i64(kAssetColCount + 2));
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

expected index_db::commit_pages(std::int64_t asset, const std::string& frame_spec, const std::string& progress_spec,
                                std::span<const frame_in> pages, std::uint32_t dim) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    // Read again (a resumed or re-queued document): its old pages go first.
    stmt del(db_, "DELETE FROM frames WHERE asset_id = ?1 AND spec = ?2 AND (flags & ?3) != 0");
    ok = del.bind(1, asset).bind(2, frame_spec).bind(3, std::int64_t{kFramePage}).run();
    stmt ins(db_, "INSERT INTO frames(asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags,"
                  " generic, scale, emb) VALUES(?1, ?2, ?3, 0, 0, 1, ?4, ?5, ?6, ?7)");
    std::vector<std::int8_t> q;
    for (const frame_in& f : pages) {
      if (!ok) break;
      if (f.emb.size() != dim || f.pts_ms < 0) {
        ok = false;
        break;
      }
      float scale = 1;
      quantise(f.emb, q, scale);
      ins.reset();
      ins.bind(1, asset).bind(2, frame_spec).bind(3, f.pts_ms).bind(4, std::int64_t{f.flags | kFramePage})
          .bind_real(5, f.generic).bind_real(6, scale).bind_blob(7, q.data(), q.size());
      ok = ins.run();
    }
    stmt p(db_, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
                " VALUES(?1, ?2, 2, ?3, strftime('%s','now'))"
                " ON CONFLICT(asset_id, spec) DO UPDATE SET state = 2,"
                " resume_ms = excluded.resume_ms, indexed_at = excluded.indexed_at");
    ok = ok && p.bind(1, asset).bind(2, progress_spec).bind(3, static_cast<std::int64_t>(pages.size())).run();
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

result<std::vector<std::int64_t>> index_db::drop_pages(std::uint32_t default_docs) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> ids;
  {
    stmt q(db_, "SELECT a.id FROM assets a JOIN roots r ON r.id = a.root_id WHERE a.kind = 3"
                " AND (CASE r.docs WHEN 0 THEN ?1 ELSE r.docs - 1 END) < 2"
                " AND (EXISTS(SELECT 1 FROM frames f WHERE f.asset_id = a.id AND (f.flags & ?2) != 0)"
                " OR EXISTS(SELECT 1 FROM progress p WHERE p.asset_id = a.id AND p.spec LIKE 'docpage/%'))");
    q.bind(1, std::int64_t{default_docs}).bind(2, std::int64_t{kFramePage});
    while (q.step_row()) ids.push_back(q.i64(0));
  }
  if (ids.empty()) return ids;
  if (!exec("BEGIN")) return err(status::io);
  stmt f(db_, "DELETE FROM frames WHERE asset_id = ?1 AND (flags & ?2) != 0");
  stmt p(db_, "DELETE FROM progress WHERE asset_id = ?1 AND spec LIKE 'docpage/%'");
  for (std::int64_t id : ids) {
    f.reset();
    p.reset();
    if (!f.bind(1, id).bind(2, std::int64_t{kFramePage}).run() || !p.bind(1, id).run()) {
      exec("ROLLBACK");
      return err(status::io);
    }
  }
  if (!exec("COMMIT")) return err(status::io);
  return ids;
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

expected index_db::speech_of(std::int64_t asset, const std::string& spec, std::vector<speech_in>& out, bool& done) {
  std::lock_guard lock(m_);
  out.clear();
  done = false;
  stmt s(db_, "SELECT start_ms, end_ms, text FROM speech WHERE asset_id = ?1 AND spec = ?2 ORDER BY start_ms");
  if (!s.ok()) return err(status::io);
  s.bind(1, asset).bind(2, spec);
  while (s.step_row()) out.push_back(speech_in{s.i64(0), s.i64(1), s.text(2)});
  stmt p(db_, "SELECT state FROM progress WHERE asset_id = ?1 AND spec = ?2");
  if (!p.ok()) return err(status::io);
  p.bind(1, asset).bind(2, spec);
  if (p.step_row()) done = p.i64(0) == static_cast<int>(work_state::done);
  return {};
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

expected index_db::commit_cues(std::int64_t asset, std::span<const cue_in> cues, work_state state,
                               std::int64_t resume_ms, std::int64_t replace_from, const std::string& spec) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    if (replace_from != -1) {
      // The pass's own rows only: a clip's subtitles and the text on its
      // moments are two passes over one table.
      stmt d(db_, "DELETE FROM cues WHERE asset_id = ?1 AND start_ms >= ?2 AND (kind = 3) = ?3");
      ok = d.bind(1, asset).bind(2, replace_from).bind(3, std::int64_t{spec == kCueSpec ? 0 : 1}).run();
    }
    stmt ins(db_, "INSERT INTO cues(asset_id, start_ms, end_ms, kind, text) VALUES(?1, ?2, ?3, ?4, ?5)");
    for (const cue_in& c : cues) {
      if (!ok) break;
      ins.reset();
      ins.bind(1, asset).bind(2, c.start_ms).bind(3, c.end_ms).bind(4, std::int64_t{static_cast<int>(c.kind)})
          .bind(5, c.text);
      ok = ins.run();
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

expected index_db::each_cue(const std::function<void(std::int64_t, const cue_in&)>& visit) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT asset_id, start_ms, end_ms, kind, text FROM cues ORDER BY asset_id, start_ms");
  if (!s.ok()) return err(status::io);
  while (s.step_row()) {
    cue_in c;
    c.start_ms = s.i64(1);
    c.end_ms = s.i64(2);
    const std::int64_t k = s.i64(3);
    c.kind = k == 1 ? cue_kind::chapter : k == 3 ? cue_kind::picture : cue_kind::subtitle;
    c.text = s.text(4);
    visit(s.i64(0), c);
  }
  return {};
}

expected index_db::commit_passages(std::int64_t asset, const std::string& spec, std::span<const passage_in> passages,
                                   work_state state, std::int64_t resume, std::int64_t replace_from) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    if (replace_from >= 0) {
      stmt d(db_, "DELETE FROM passages WHERE asset_id = ?1 AND spec = ?2 AND page >= ?3");
      ok = d.bind(1, asset).bind(2, spec).bind(3, replace_from).run();
    }
    std::int64_t ord = 0;
    if (ok) {
      stmt o(db_, "SELECT COALESCE(MAX(ord) + 1, 0) FROM passages WHERE asset_id = ?1 AND spec = ?2");
      if (o.bind(1, asset).bind(2, spec).step_row()) ord = o.i64(0);
    }
    stmt ins(db_, "INSERT INTO passages(asset_id, spec, page, ord, text) VALUES(?1, ?2, ?3, ?4, ?5)");
    stmt words(db_, "INSERT INTO passage_text(rowid, words) VALUES(?1, ?2)");
    for (const passage_in& p : passages) {
      if (!ok) break;
      ins.reset();
      ins.bind(1, asset).bind(2, spec).bind(3, std::int64_t{p.page}).bind(4, ord++).bind(5, p.text);
      ok = ins.run();
      if (!ok) break;
      words.reset();
      ok = words.bind(1, static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_))).bind(2, p.words).run();
    }
    stmt pr(db_, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
                 " VALUES(?1, ?2, ?3, ?4, strftime('%s','now'))"
                 " ON CONFLICT(asset_id, spec) DO UPDATE SET state = excluded.state,"
                 " resume_ms = excluded.resume_ms, indexed_at = excluded.indexed_at");
    ok = ok && pr.bind(1, asset).bind(2, spec).bind(3, std::int64_t{static_cast<int>(state)}).bind(4, resume).run();
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::reset_state(const std::string& spec, work_state state) {
  std::lock_guard lock(m_);
  stmt d(db_, "DELETE FROM progress WHERE spec = ?1 AND state = ?2");
  return d.bind(1, spec).bind(2, std::int64_t{static_cast<int>(state)}).run() ? expected{} : err(status::io);
}

expected index_db::mark_locked(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt p(db_, "INSERT INTO progress(asset_id, spec, state, tries) VALUES(?1, ?2, 5, 0)"
              " ON CONFLICT(asset_id, spec) DO UPDATE SET state = 5");
  return p.bind(1, asset).bind(2, spec).run() ? expected{} : err(status::io);
}

bool index_db::has_passage_index() {
  std::lock_guard lock(m_);
  stmt k(db_, "SELECT value FROM meta WHERE key = 'passage_text'");
  return k.step_row() && k.text(0) == "fts5";
}

expected index_db::find_passages(const std::string& fts_query, std::size_t limit,
                                 const std::function<bool(const passage_out&)>& visit) {
  std::lock_guard lock(m_);
  bool fts = false;
  {
    stmt k(db_, "SELECT value FROM meta WHERE key = 'passage_text'");
    fts = k.step_row() && k.text(0) == "fts5";
  }
  // With FTS5 the query narrows to the passages holding its words (best
  // first); without it every passage is a candidate and the engine's own rule
  // decides, as it does for each candidate anyway.
  const bool narrowed = fts && !fts_query.empty();
  stmt s(db_, narrowed ? "SELECT p.id, p.asset_id, p.page, p.text, t.words FROM passage_text t"
                         " JOIN passages p ON p.id = t.rowid WHERE passage_text MATCH ?1 ORDER BY rank LIMIT ?2"
                       : "SELECT p.id, p.asset_id, p.page, p.text, t.words FROM passages p"
                         " JOIN passage_text t ON t.rowid = p.id LIMIT ?2");
  if (!s.ok()) return err(status::io);
  if (narrowed) s.bind(1, fts_query);
  // Unnarrowed, every passage is a candidate (-1: no limit).
  s.bind(2, narrowed ? static_cast<std::int64_t>(limit) : std::int64_t{-1});
  while (s.step_row()) {
    passage_out p;
    p.id = s.i64(0);
    p.asset = s.i64(1);
    p.page = static_cast<std::uint32_t>(s.i64(2));
    p.text = s.text(3);
    p.words = s.text(4);
    if (!visit(p)) break;
  }
  return {};
}

std::vector<passage_out> index_db::passages_of(std::int64_t asset) {
  std::lock_guard lock(m_);
  std::vector<passage_out> out;
  stmt s(db_, "SELECT id, page, text FROM passages WHERE asset_id = ?1 ORDER BY ord");
  if (!s.ok()) return out;
  s.bind(1, asset);
  while (s.step_row()) {
    passage_out p;
    p.id = s.i64(0);
    p.asset = asset;
    p.page = static_cast<std::uint32_t>(s.i64(1));
    p.text = s.text(2);
    out.push_back(std::move(p));
  }
  return out;
}

result<passage_out> index_db::passage_by_id(std::int64_t id) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT asset_id, page, text FROM passages WHERE id = ?1");
  if (!s.ok()) return err(status::io);
  s.bind(1, id);
  if (!s.step_row()) return err(status::not_found);
  passage_out p;
  p.id = id;
  p.asset = s.i64(0);
  p.page = static_cast<std::uint32_t>(s.i64(1));
  p.text = s.text(2);
  return p;
}

expected index_db::commit_passage_vecs(std::int64_t asset, const std::string& spec,
                                       std::span<const passage_vec_in> vecs, std::uint32_t dim) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    stmt d(db_, "DELETE FROM passage_vecs WHERE spec = ?2 AND passage_id IN"
                " (SELECT id FROM passages WHERE asset_id = ?1)");
    ok = d.bind(1, asset).bind(2, spec).run();
    // Only passages still there (a changed file dropped them meanwhile).
    stmt ins(db_, "INSERT OR REPLACE INTO passage_vecs(passage_id, spec, generic, scale, emb)"
                  " SELECT ?1, ?2, ?3, ?4, ?5 WHERE EXISTS (SELECT 1 FROM passages WHERE id = ?1 AND asset_id = ?6)");
    std::vector<std::int8_t> q;
    for (const passage_vec_in& v : vecs) {
      if (!ok) break;
      if (v.emb.size() != dim) {
        ok = false;
        break;
      }
      float scale = 1;
      quantise(v.emb, q, scale);
      ins.reset();
      ins.bind(1, v.passage).bind(2, spec).bind_real(3, v.generic).bind_real(4, scale)
          .bind_blob(5, q.data(), q.size()).bind(6, asset);
      ok = ins.run();
    }
    stmt pr(db_, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
                 " VALUES(?1, ?2, 2, 0, strftime('%s','now'))"
                 " ON CONFLICT(asset_id, spec) DO UPDATE SET state = 2, resume_ms = 0,"
                 " indexed_at = excluded.indexed_at");
    ok = ok && pr.bind(1, asset).bind(2, spec).run();
  }
  if (!ok) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected index_db::each_passage_vec(const std::string& spec, const std::function<void(const frame_out&)>& visit,
                                    std::int64_t after_id) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT v.passage_id, p.asset_id, v.generic, v.scale, v.emb FROM passage_vecs v"
              " JOIN passages p ON p.id = v.passage_id WHERE v.spec = ?1 AND v.passage_id > ?2"
              " ORDER BY v.passage_id");
  if (!s.ok()) return err(status::io);
  s.bind(1, spec).bind(2, after_id);
  while (s.step_row()) {
    frame_out f;
    f.id = s.i64(0);
    f.asset_id = s.i64(1);
    f.pts_ms = f.id;
    f.generic = static_cast<float>(s.real(2));
    f.scale = static_cast<float>(s.real(3));
    f.emb = s.blob(4);
    visit(f);
  }
  return {};
}

std::uint64_t index_db::passage_vec_count(const std::string& spec) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM passage_vecs WHERE spec = ?1");
  f.bind(1, spec);
  return f.step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::passage_count(std::int64_t asset) {
  std::lock_guard lock(m_);
  stmt f(db_, asset > 0 ? "SELECT COUNT(*) FROM passages WHERE asset_id = ?1" : "SELECT COUNT(*) FROM passages");
  if (asset > 0) f.bind(1, asset);
  return f.step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

expected index_db::drop_spec(const std::string& spec) {
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  stmt f(db_, "DELETE FROM frames WHERE spec = ?1");
  stmt p(db_, "DELETE FROM progress WHERE spec = ?1");
  stmt sp(db_, "DELETE FROM speech WHERE spec = ?1");
  stmt pv(db_, "DELETE FROM passage_vecs WHERE spec = ?1");
  if (!f.bind(1, spec).run() || !p.bind(1, spec).run() || !sp.bind(1, spec).run() || !pv.bind(1, spec).run()) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

counts index_db::count(const std::string& spec, const track_filter& filter, const std::string& after) {
  std::lock_guard lock(m_);
  counts c;
  const std::string sql = std::string(
      "SELECT COUNT(*),"
      " SUM(CASE WHEN p.state = 2 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN p.state = 3 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN a.kind = 2 AND a.cloud = 0 AND (p.state IS NULL OR p.state < 2)"
      "     THEN MAX(a.duration_ms - COALESCE(p.resume_ms, 0), 0) ELSE 0 END),"
      " SUM(CASE WHEN a.kind IN (1, 4) AND a.cloud = 0 AND (p.state IS NULL OR p.state < 2) THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN p.state = 4 THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN a.cloud = 1 AND (p.state IS NULL OR p.state < 2) THEN 1 ELSE 0 END),"
      " SUM(CASE WHEN p.state = 5 THEN 1 ELSE 0 END)"
      " FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1"
      " LEFT JOIN progress p ON p.asset_id = a.id AND p.spec = ?1 WHERE 1") + kTrackWhere +
      held_where(filter) + after_where(after);
  stmt a(db_, sql.c_str());
  a.bind(4, std::int64_t{filter.photos ? 1 : 0}).bind(5, std::int64_t{filter.default_media})
      .bind(6, std::int64_t{filter.media_bit}).bind(7, filter.skip_root)
      .bind(9, std::int64_t{filter.docs_min}).bind(10, std::int64_t{filter.default_docs});
  if (!after.empty()) a.bind(8, after);
  if (a.bind(1, spec).step_row()) {
    c.assets = static_cast<std::uint64_t>(a.i64(0));
    c.done = static_cast<std::uint64_t>(a.i64(1));
    c.failed = static_cast<std::uint64_t>(a.i64(2));
    c.pending_video_ms = static_cast<std::uint64_t>(a.i64(3));
    c.pending_photos = static_cast<std::uint64_t>(a.i64(4));
    c.unavailable = static_cast<std::uint64_t>(a.i64(5));
    c.cloud_only = static_cast<std::uint64_t>(a.i64(6));
    c.locked = static_cast<std::uint64_t>(a.i64(7));
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
  // #354: documents are counted apart (kind_in_root): the picture pass never
  // takes them, so a folder of photos and PDFs still reads "Up to date".
  stmt f(db_, "SELECT COUNT(*) FROM assets WHERE root_id = ?1 AND kind != 3");
  return f.bind(1, root).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::kind_in_root(std::int64_t root, asset_kind kind) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM assets WHERE root_id = ?1 AND kind = ?2");
  return f.bind(1, root).bind(2, std::int64_t{static_cast<int>(kind)}).step_row()
             ? static_cast<std::uint64_t>(f.i64(0))
             : 0;
}

std::uint64_t index_db::unavailable_in_root(std::int64_t root, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt f(db_, "SELECT COUNT(*) FROM assets a JOIN progress p ON p.asset_id = a.id AND p.spec = ?2"
              " WHERE a.root_id = ?1 AND p.state = 4");
  return f.bind(1, root).bind(2, spec).step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
}

std::uint64_t index_db::pending_in_root(std::int64_t root, const std::string& spec, std::int32_t max_tries,
                                        const track_filter& filter) {
  std::lock_guard lock(m_);
  const std::string sql = std::string(
      "SELECT COUNT(*) FROM assets a JOIN roots r ON r.id = a.root_id AND r.enabled = 1"
      " LEFT JOIN progress p ON p.asset_id = a.id AND p.spec = ?1"
      " WHERE a.root_id = ?3 AND") + kPendingWhere + kTrackWhere;
  stmt f(db_, sql.c_str());
  f.bind(1, spec).bind(2, std::int64_t{max_tries}).bind(3, root)
      .bind(4, std::int64_t{filter.photos ? 1 : 0}).bind(5, std::int64_t{filter.default_media})
      .bind(6, std::int64_t{filter.media_bit}).bind(7, filter.skip_root)
      .bind(9, std::int64_t{filter.docs_min}).bind(10, std::int64_t{filter.default_docs});
  return f.step_row() ? static_cast<std::uint64_t>(f.i64(0)) : 0;
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

std::vector<std::int64_t> index_db::moment_times(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> out;
  stmt s(db_, "SELECT pts_ms FROM frames WHERE asset_id = ?1 AND spec = ?2 AND pts_ms >= 0 ORDER BY pts_ms");
  s.bind(1, asset).bind(2, spec);
  while (s.step_row()) out.push_back(s.i64(0));
  return out;
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
  if (!exec("DELETE FROM speech; DELETE FROM cues; DELETE FROM passage_vecs; DELETE FROM passages;"
            " DELETE FROM passage_text;"
            " DELETE FROM frames; DELETE FROM progress; DELETE FROM asset_text; DELETE FROM assets; DELETE FROM roots;")) {
    return err(status::io);
  }
  // Give the space back (docs/design/17 PR 23 verify: "clearing the index frees the disk").
  exec("PRAGMA wal_checkpoint(TRUNCATE);");
  if (!exec("VACUUM;")) return err(status::io);
  exec("PRAGMA wal_checkpoint(TRUNCATE);");
  return {};
}

// ---- tags (#291, schema 4) -----------------------------------------------------------

std::vector<asset_row> index_db::assets_needing_tags(std::size_t limit, int level) {
  std::lock_guard lock(m_);
  std::vector<asset_row> out;
  const std::string sql = std::string("SELECT ") + kAssetCols +
                          " FROM assets a JOIN roots r ON r.id = a.root_id"
                          " WHERE a.tags < ?2 AND a.cloud = 0 AND r.enabled = 1 ORDER BY a.id LIMIT ?1";
  stmt s(db_, sql.c_str());
  s.bind(1, static_cast<std::int64_t>(limit)).bind(2, std::int64_t{level});
  while (s.step_row()) out.push_back(asset_from(s, 0));
  return out;
}

std::uint64_t index_db::tags_pending(int level) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM assets a JOIN roots r ON r.id = a.root_id"
              " WHERE a.tags < ?1 AND a.cloud = 0 AND r.enabled = 1");
  s.bind(1, std::int64_t{level});
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0;
}

expected index_db::commit_tags(std::span<const asset_tags> rows) {
  if (rows.empty()) return {};
  std::lock_guard lock(m_);
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  {
    // Only while the asset is the file that was read: a rescan that moved
    // its stamp meanwhile set tags back to 0, and this row is dropped.
    stmt up(db_, "UPDATE assets SET tags = ?11, taken_at = ?4, lat = ?5, lon = ?6, camera = ?7, lens = ?8,"
                 " made_with = ?9, ai = ?10, rating = ?12, label = ?13, iso = ?14, aperture_x100 = ?15,"
                 " focal_mm = ?16, shutter_den = ?17, shutter_us = ?18"
                 " WHERE id = ?1 AND mtime = ?2 AND size = ?3");
    stmt del(db_, "DELETE FROM asset_text WHERE rowid = ?1");
    stmt ins(db_, "INSERT INTO asset_text(rowid, text) VALUES(?1, ?2)");
    for (const asset_tags& t : rows) {
      up.reset();
      up.bind(1, t.id).bind(2, t.mtime).bind(3, static_cast<std::int64_t>(t.size));
      if (t.taken_at) up.bind(4, *t.taken_at);
      if (t.has_where) up.bind_real(5, t.lat).bind_real(6, t.lon);
      up.bind(7, t.camera).bind(8, t.lens).bind(9, t.made_with).bind(10, std::int64_t{t.ai ? 1 : 0});
      up.bind(11, std::int64_t{std::clamp(t.level, 1, kTagsLevel)});
      // #405: NULL for what the file does not say (and all of it below level 2).
      if (t.level >= 2) {
        if (t.rating) up.bind(12, std::int64_t{*t.rating});
        if (!t.label.empty()) up.bind(13, t.label);
        if (t.iso > 0) up.bind(14, std::int64_t{t.iso});
        if (t.aperture_x100 > 0) up.bind(15, std::int64_t{t.aperture_x100});
        if (t.focal_mm > 0) up.bind(16, std::int64_t{t.focal_mm});
        if (t.shutter_den > 0) up.bind(17, std::int64_t{t.shutter_den});
        if (t.shutter_us > 0) up.bind(18, t.shutter_us);
      }
      if (!up.run()) {
        ok = false;
        break;
      }
      if (sqlite3_changes(db_) == 0) continue;
      del.reset();
      if (!del.bind(1, t.id).run()) {
        ok = false;
        break;
      }
      if (t.text.empty()) continue;
      ins.reset();
      if (!ins.bind(1, t.id).bind(2, t.text).run()) {
        ok = false;
        break;
      }
    }
  }
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    return err(status::io);
  }
  return {};
}

expected index_db::each_tags(const std::function<void(const asset_tags&)>& visit) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT a.id, a.mtime, a.size, a.taken_at, a.lat, a.lon, a.camera, a.lens, a.made_with, a.ai,"
              " t.text, a.tags, a.rating, a.label, a.iso, a.aperture_x100, a.focal_mm, a.shutter_den, a.shutter_us"
              " FROM assets a LEFT JOIN asset_text t ON t.rowid = a.id WHERE a.tags >= 1");
  if (!s.ok()) return err(status::io);
  while (s.step_row()) {
    asset_tags t;
    t.id = s.i64(0);
    t.mtime = s.i64(1);
    t.size = static_cast<std::uint64_t>(s.i64(2));
    if (!s.is_null(3)) t.taken_at = s.i64(3);
    t.has_where = !s.is_null(4) && !s.is_null(5);
    t.lat = s.real(4);
    t.lon = s.real(5);
    t.camera = s.text(6);
    t.lens = s.text(7);
    t.made_with = s.text(8);
    t.ai = s.i64(9) != 0;
    t.text = s.text(10);
    t.level = static_cast<int>(s.i64(11));
    if (!s.is_null(12)) t.rating = static_cast<int>(s.i64(12));
    t.label = s.text(13);
    t.iso = static_cast<std::int32_t>(s.i64(14));
    t.aperture_x100 = static_cast<std::int32_t>(s.i64(15));
    t.focal_mm = static_cast<std::int32_t>(s.i64(16));
    t.shutter_den = static_cast<std::int32_t>(s.i64(17));
    t.shutter_us = s.i64(18);
    visit(t);
  }
  return {};
}

std::string index_db::fts_phrases(std::string_view typed) {
  // Every word a quoted FTS5 string (a quote inside doubled), joined by
  // spaces (AND): the user's text is never FTS5 syntax, so NEAR, OR, -, *,
  // column filters and an unbalanced quote are words like any other.
  std::string out;
  std::size_t i = 0;
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x1f'; };
  while (i < typed.size()) {
    while (i < typed.size() && space(typed[i])) ++i;
    if (i >= typed.size()) break;
    if (!out.empty()) out.push_back(' ');
    out.push_back('"');
    while (i < typed.size() && !space(typed[i])) {
      if (typed[i] == '"') out.push_back('"');
      out.push_back(typed[i++]);
    }
    out.push_back('"');
  }
  return out;
}

result<std::vector<index_db::located>> index_db::located_assets(std::int64_t after_id, std::size_t limit) {
  std::vector<located> out;
  std::lock_guard lock(m_);
  // The primary key walks in order; a row without both columns is skipped.
  stmt s(db_, "SELECT id, path, lat, lon FROM assets WHERE id > ?1 AND lat IS NOT NULL AND lon IS NOT NULL"
              " ORDER BY id LIMIT ?2");
  if (!s.ok()) return err(status::io);
  s.bind(1, after_id).bind(2, static_cast<std::int64_t>(limit));
  while (s.step_row()) {
    located l;
    l.id = s.i64(0);
    l.path = s.text(1);
    l.lat = s.real(2);
    l.lon = s.real(3);
    out.push_back(std::move(l));
  }
  return out;
}

std::vector<std::int64_t> index_db::match_text(const std::string& words, std::size_t limit) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> out;
  const std::string fts_query = fts_phrases(words);
  if (fts_query.empty()) return out;
  {
    stmt k(db_, "SELECT value FROM meta WHERE key = 'asset_text'");
    if (!k.step_row() || k.text(0) != "fts5") return out;
  }
  stmt s(db_, "SELECT rowid FROM asset_text WHERE asset_text MATCH ?1 LIMIT ?2");
  s.bind(1, fts_query).bind(2, static_cast<std::int64_t>(limit));
  while (s.step_row()) out.push_back(s.i64(0));
  return out;
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
