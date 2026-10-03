// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/transfer.h"

#include "addons/ai/photos_source.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace mv::ai::transfer {
namespace {

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
  stmt& bind_null(int i) {
    sqlite3_bind_null(s_, i);
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
  [[nodiscard]] bool is_null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
  [[nodiscard]] std::span<const std::uint8_t> blob(int col) const {
    const void* p = sqlite3_column_blob(s_, col);
    const int n = sqlite3_column_bytes(s_, col);
    return p && n > 0 ? std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p), static_cast<std::size_t>(n))
                      : std::span<const std::uint8_t>();
  }

 private:
  sqlite3_stmt* s_ = nullptr;
};

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

// An SQL string literal ('it''s').
std::string quoted(const std::string& v) {
  std::string o = "'";
  for (char ch : v) {
    o += ch;
    if (ch == '\'') o += '\'';
  }
  return o + "'";
}

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_sep(char c) { return c == '/' || c == '\\'; }

std::string strip_seps(std::string s) {
  while (!s.empty() && is_sep(s.back())) s.pop_back();
  return s;
}

// A comparable spelling: '/' separators and, on Windows, ASCII case folded
// (engine.h path_key, without its trailing-slash rule).
std::string fold(const std::string& s) {
  std::string k = s;
  for (char& c : k) {
    if (c == '\\') c = '/';
#if defined(_WIN32)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
  }
  return k;
}

// `path` relative to `root`, '/'-separated; empty when it is not under it.
std::string relative(const std::string& root, const std::string& path) {
  const std::string r = strip_seps(root);
  if (path.size() <= r.size() + 1) return {};
  if (fold(path.substr(0, r.size())) != fold(r) || !is_sep(path[r.size()])) return {};
  std::string rel = path.substr(r.size() + 1);
  for (char& c : rel) {
    if (c == '\\') c = '/';
  }
  return rel;
}

// A file's relative path is untrusted: no absolute path, no "." or "..", no
// empty part, nothing a Windows path would read as a drive or a stream.
bool safe_rel(const std::string& rel) {
  if (rel.empty() || rel.size() > 4096 || rel.front() == '/') return false;
  std::size_t start = 0;
  while (start <= rel.size()) {
    const std::size_t end = rel.find('/', start);
    const std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (part.find_first_of(std::string("\\:\0", 3)) != std::string::npos) return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

std::string name_of(const std::string& root) {
  const std::string r = strip_seps(root);
  const std::size_t slash = r.find_last_of("/\\");
  return slash == std::string::npos ? r : r.substr(slash + 1);
}

bool finite_floats(std::span<const std::uint8_t> blob) {
  if (blob.size() % sizeof(float) != 0) return false;
  for (std::size_t i = 0; i < blob.size(); i += sizeof(float)) {
    float v;
    std::memcpy(&v, blob.data() + i, sizeof(float));
    if (!std::isfinite(v)) return false;
  }
  return true;
}

constexpr const char* kSchema =
    "CREATE TABLE info(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE roots(id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL,"
    " recursive INTEGER NOT NULL, media INTEGER NOT NULL);"
    "CREATE TABLE assets(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL, rel TEXT NOT NULL,"
    " mtime INTEGER NOT NULL, size INTEGER NOT NULL, kind INTEGER NOT NULL, duration_ms INTEGER NOT NULL);"
    "CREATE TABLE progress(asset_id INTEGER NOT NULL, spec TEXT NOT NULL, state INTEGER NOT NULL,"
    " resume_ms INTEGER NOT NULL, indexed_at INTEGER NOT NULL, PRIMARY KEY(asset_id, spec));"
    "CREATE TABLE frames(asset_id INTEGER NOT NULL, spec TEXT NOT NULL, pts_ms INTEGER NOT NULL,"
    " pts_tb INTEGER NOT NULL, tb_num INTEGER NOT NULL, tb_den INTEGER NOT NULL, flags INTEGER NOT NULL,"
    " generic REAL NOT NULL, scale REAL NOT NULL, emb BLOB NOT NULL);"
    "CREATE TABLE speech(asset_id INTEGER NOT NULL, spec TEXT NOT NULL, start_ms INTEGER NOT NULL,"
    " end_ms INTEGER NOT NULL, text TEXT NOT NULL);"
    "CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT NOT NULL, created_at INTEGER NOT NULL);"
    "CREATE TABLE faces(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL, pts_ms INTEGER NOT NULL,"
    " x REAL, y REAL, w REAL, h REAL, score REAL, person_id INTEGER, emb BLOB NOT NULL,"
    " pinned INTEGER NOT NULL, quality REAL, tta INTEGER NOT NULL);"
    "CREATE TABLE rejected(face_id INTEGER NOT NULL, person_id INTEGER NOT NULL);"
    "CREATE TABLE no_merge(a INTEGER NOT NULL, b INTEGER NOT NULL);"
    "CREATE TABLE face_scanned(asset_id INTEGER NOT NULL, spec TEXT NOT NULL);"
    "CREATE TABLE thumbs(asset_id INTEGER NOT NULL, pts_ms INTEGER NOT NULL, jpeg BLOB NOT NULL,"
    " PRIMARY KEY(asset_id, pts_ms));";

// Opens someone else's file to read: read-only, and its schema is not
// trusted to run anything (no triggers, views or functions it could carry).
result<sqlite3*> open_untrusted(const std::string& file) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(status::io);
  }
  sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
#if defined(SQLITE_DBCONFIG_TRUSTED_SCHEMA)
  sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
#endif
  sqlite3_busy_timeout(db, 5000);
  return db;
}

struct db_closer {
  sqlite3* db;
  ~db_closer() {
    if (db) sqlite3_close_v2(db);
  }
};

std::string info_value(sqlite3* db, const char* key) {
  stmt s(db, "SELECT value FROM info WHERE key = ?1");
  return s.ok() && s.bind(1, std::string(key)).step_row() ? s.text(0) : std::string();
}

std::uint64_t count_of(sqlite3* db, const char* sql) {
  stmt s(db, sql);
  return s.step_row() ? static_cast<std::uint64_t>(std::max<std::int64_t>(0, s.i64(0))) : 0;
}

}  // namespace

std::string local_path(const std::string& dir, const std::string& rel) {
#if defined(_WIN32)
  constexpr char sep = '\\';
#else
  constexpr char sep = '/';
#endif
  std::string out = strip_seps(dir);
  out += sep;
  for (char c : rel) out += c == '/' ? sep : c;
  return out;
}

// ---- export ------------------------------------------------------------------------------

result<export_counts> write(const std::string& dest, const export_options& o, const control& c) {
  export_counts n;
  const std::string part = dest + ".part";
  std::error_code ec;
  std::filesystem::remove(fs_path(part), ec);
  sqlite3* x = nullptr;
  if (sqlite3_open_v2(part.c_str(), &x, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    sqlite3_close_v2(x);
    return err(status::io);
  }
  bool ok = true;
  const auto fail = [&](status s) -> result<export_counts> {
    sqlite3_close_v2(x);
    x = nullptr;
    std::error_code e2;
    std::filesystem::remove(fs_path(part), e2);
    return err(s);
  };
  // A new file renamed into place at the end: no journal to keep.
  ok = exec(x, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;") && exec(x, kSchema);
  if (!ok) return fail(status::io);
  {
    stmt a(x, "ATTACH ?1 AS src");
    if (!a.bind(1, o.index_db).run()) return fail(status::io);
  }
  const bool with_faces = o.faces && !o.faces_db.empty() && std::filesystem::exists(fs_path(o.faces_db), ec);
  if (with_faces) {
    stmt a(x, "ATTACH ?1 AS fdb");
    if (!a.bind(1, o.faces_db).run()) return fail(status::io);
  }
  // One read transaction: a consistent snapshot while indexing goes on.
  if (!exec(x, "BEGIN")) return fail(status::io);
  {
    stmt i(x, "INSERT INTO info(key, value) VALUES(?1, ?2)");
    const auto put = [&](const char* k, const std::string& v) {
      i.reset();
      ok = ok && i.bind(1, std::string(k)).bind(2, v).run();
    };
    put("format", kFormat);
    put("version", std::to_string(kVersion));
    put("created", std::to_string(unix_now()));
    put("from", o.from);
    put("picture_spec", o.picture_spec);
    put("face_spec", with_faces ? o.face_spec : std::string());
    put("faces", with_faces ? "1" : "0");
    put("thumbs", o.thumbs && o.thumb ? "1" : "0");
  }
  // Roots, and their assets with paths made relative.
  std::set<std::int64_t> wanted(o.roots.begin(), o.roots.end());
  std::vector<std::pair<std::int64_t, std::string>> roots;
  {
    stmt r(x, "SELECT id, path, recursive, media FROM src.roots ORDER BY id");
    stmt ins(x, "INSERT INTO roots(id, name, path, recursive, media) VALUES(?1, ?2, ?3, ?4, ?5)");
    while (ok && r.step_row()) {
      const std::int64_t id = r.i64(0);
      if (!wanted.empty() && !wanted.count(id)) continue;
      const std::string path = r.text(1);
      // The Photos library (issue #72) is this Mac's: its keys are PhotoKit
      // identifiers, not paths under a folder another machine could map.
      if (is_photos_key(path)) continue;
      ins.reset();
      ok = ins.bind(1, id).bind(2, name_of(path)).bind(3, path).bind(4, r.i64(2)).bind(5, r.i64(3)).run();
      roots.emplace_back(id, path);
    }
  }
  if (!ok) return fail(status::io);
  if (roots.empty()) return fail(status::invalid_arg);
  n.roots = roots.size();
  std::unordered_map<std::int64_t, std::string> path_of;  // for thumbnails
  {
    stmt q(x, "SELECT id, path, mtime, size, kind, duration_ms FROM src.assets WHERE root_id = ?1");
    stmt ins(x, "INSERT INTO assets(id, root_id, rel, mtime, size, kind, duration_ms)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    for (const auto& [id, rpath] : roots) {
      q.reset();
      q.bind(1, id);
      while (ok && q.step_row()) {
        const std::string path = q.text(1);
        const std::string rel = relative(rpath, path);
        if (rel.empty()) continue;
        ins.reset();
        ok = ins.bind(1, q.i64(0)).bind(2, id).bind(3, rel).bind(4, q.i64(2)).bind(5, q.i64(3))
                 .bind(6, q.i64(4)).bind(7, q.i64(5)).run();
        if (o.thumbs) path_of.emplace(q.i64(0), path);
        ++n.assets;
      }
      if (c.cancelled()) return fail(status::cancelled);
    }
  }
  if (!ok) return fail(status::io);
  c.report(0.1);
  // The rows that describe them: done and partial work only (a failure is this
  // machine's business, and a pending row describes nothing).
  ok = exec(x, "INSERT INTO progress SELECT p.asset_id, p.spec, p.state, p.resume_ms, p.indexed_at"
               " FROM src.progress p JOIN assets a ON a.id = p.asset_id WHERE p.state IN (1, 2)");
  if (!ok) return fail(status::io);
  ok = exec(x, "INSERT INTO frames SELECT f.asset_id, f.spec, f.pts_ms, f.pts_tb, f.tb_num, f.tb_den, f.flags,"
               " f.generic, f.scale, f.emb FROM src.frames f"
               " JOIN progress p ON p.asset_id = f.asset_id AND p.spec = f.spec");
  n.frames = static_cast<std::uint64_t>(sqlite3_changes64(x));
  if (!ok || c.cancelled()) return fail(ok ? status::cancelled : status::io);
  c.report(0.4);
  ok = exec(x, "INSERT INTO speech SELECT s.asset_id, s.spec, s.start_ms, s.end_ms, s.text FROM src.speech s"
               " JOIN progress p ON p.asset_id = s.asset_id AND p.spec = s.spec");
  n.speech = static_cast<std::uint64_t>(sqlite3_changes64(x));
  if (!ok) return fail(status::io);
  if (with_faces) {
    ok = exec(x, "INSERT INTO face_scanned SELECT s.asset_id, s.spec FROM fdb.scanned s"
                 " JOIN assets a ON a.id = s.asset_id") &&
         // The file is one embedder's (face_spec): a face another made (a
         // re-run under way) stays behind.
         exec(x, ("INSERT INTO faces SELECT f.id, f.asset_id, f.pts_ms, f.x, f.y, f.w, f.h, f.score,"
                  " f.person_id, f.emb, f.pinned, f.quality, f.tta FROM fdb.faces f"
                  " JOIN assets a ON a.id = f.asset_id WHERE f.spec = " + quoted(o.face_spec)).c_str());
    n.faces = count_of(x, "SELECT COUNT(*) FROM faces");
    ok = ok &&
         exec(x, "INSERT INTO people SELECT p.id, p.name, p.created_at FROM fdb.people p"
                 " WHERE p.id IN (SELECT DISTINCT person_id FROM faces WHERE person_id IS NOT NULL)") &&
         exec(x, "INSERT INTO rejected SELECT r.face_id, r.person_id FROM fdb.rejected r"
                 " WHERE r.face_id IN (SELECT id FROM faces) AND r.person_id IN (SELECT id FROM people)") &&
         exec(x, "INSERT INTO no_merge SELECT m.a, m.b FROM fdb.no_merge m"
                 " WHERE m.a IN (SELECT id FROM people) AND m.b IN (SELECT id FROM people)");
    n.people = count_of(x, "SELECT COUNT(*) FROM people");
    if (!ok) return fail(status::io);
  }
  if (!exec(x, "COMMIT")) return fail(status::io);
  exec(x, "DETACH src");
  if (with_faces) exec(x, "DETACH fdb");
  c.report(0.5);

  // Thumbnails the viewer already made: the file's own, and each moment an
  // embedding or a face was taken at.
  if (o.thumbs && o.thumb) {
    std::vector<std::pair<std::int64_t, std::int64_t>> want;
    for (const auto& [id, path] : path_of) want.emplace_back(id, -1);
    {
      stmt m(x, "SELECT DISTINCT asset_id, pts_ms FROM frames WHERE pts_ms >= 0"
                " UNION SELECT DISTINCT asset_id, pts_ms FROM faces WHERE pts_ms >= 0");
      while (m.step_row()) want.emplace_back(m.i64(0), m.i64(1));
    }
    stmt ins(x, "INSERT OR IGNORE INTO thumbs(asset_id, pts_ms, jpeg) VALUES(?1, ?2, ?3)");
    if (!exec(x, "BEGIN")) return fail(status::io);
    for (std::size_t i = 0; i < want.size() && ok; ++i) {
      const auto it = path_of.find(want[i].first);
      if (it == path_of.end()) continue;
      auto jpeg = o.thumb(it->second, want[i].second);
      if (!jpeg || jpeg->empty()) {
        ++n.thumbs_missing;
      } else {
        ins.reset();
        ok = ins.bind(1, want[i].first).bind(2, want[i].second).bind_blob(3, jpeg->data(), jpeg->size()).run();
        ++n.thumbs;
      }
      if ((i & 255) == 255) {
        if (c.cancelled()) {
          exec(x, "ROLLBACK");
          return fail(status::cancelled);
        }
        c.report(0.5 + 0.5 * static_cast<double>(i) / static_cast<double>(want.size()));
      }
    }
    if (!ok || !exec(x, "COMMIT")) return fail(status::io);
  }
  // Indexes an import's lookups use (it never relies on them: the file is not trusted).
  exec(x, "CREATE INDEX assets_root ON assets(root_id);");
  sqlite3_close_v2(x);
  x = nullptr;
  std::filesystem::rename(fs_path(part), fs_path(dest), ec);
  if (ec) {
    std::filesystem::remove(fs_path(part), ec);
    return err(status::io);
  }
  n.bytes = static_cast<std::uint64_t>(std::filesystem::file_size(fs_path(dest), ec));
  c.report(1.0);
  return n;
}

// ---- reading a file ----------------------------------------------------------------------

result<file_info> inspect(const std::string& file) {
  MV_TRY(sqlite3* db, open_untrusted(file));
  db_closer closer{db};
  file_info f;
  if (info_value(db, "format") != kFormat) return err(status::unsupported_format);
  const std::string v = info_value(db, "version");
  f.version = v.empty() ? 0 : std::atoi(v.c_str());
  if (f.version < 1 || f.version > kVersion) return err(status::unsupported_format);
  f.created = std::atoll(info_value(db, "created").c_str());
  f.from = info_value(db, "from");
  f.picture_spec = info_value(db, "picture_spec");
  f.face_spec = info_value(db, "face_spec");
  f.faces = info_value(db, "faces") == "1";
  f.thumbs = info_value(db, "thumbs") == "1";
  {
    stmt s(db, "SELECT DISTINCT spec FROM progress ORDER BY spec");
    while (s.step_row()) f.specs.push_back(s.text(0));
  }
  if (f.faces) {
    f.face_count = count_of(db, "SELECT COUNT(*) FROM faces");
    f.people = count_of(db, "SELECT COUNT(*) FROM people");
  }
  if (f.thumbs) f.thumb_count = count_of(db, "SELECT COUNT(*) FROM thumbs");
  {
    stmt s(db, "SELECT r.id, r.name, r.path, r.recursive, r.media,"
               " (SELECT COUNT(*) FROM assets a WHERE a.root_id = r.id) FROM roots r ORDER BY r.id");
    if (!s.ok()) return err(status::corrupt);
    while (s.step_row()) {
      file_root r;
      r.id = s.i64(0);
      r.name = s.text(1);
      r.path = s.text(2);
      r.recursive = s.i64(3) != 0;
      r.media = static_cast<std::uint32_t>(std::clamp<std::int64_t>(s.i64(4), 0, 3));
      r.assets = static_cast<std::uint64_t>(std::max<std::int64_t>(0, s.i64(5)));
      f.roots.push_back(std::move(r));
    }
  }
  return f;
}

// ---- import ---------------------------------------------------------------------------------

expected import_index(sqlite3* db, const std::string& file, std::span<const root_target> targets,
                      const std::set<std::string>& specs, std::vector<imported_asset>& out,
                      import_counts& n, const control& c) {
  MV_TRY(sqlite3* x, open_untrusted(file));
  db_closer closer{x};
  if (info_value(x, "format") != kFormat) return err(status::unsupported_format);

  std::map<std::int64_t, const root_target*> target_of;
  for (const root_target& t : targets) target_of[t.file_root] = &t;

  // The file's done / partial work per asset and spec.
  std::map<std::int64_t, std::map<std::string, std::pair<std::int64_t, std::int64_t>>> work;  // state, resume
  {
    stmt p(x, "SELECT asset_id, spec, state, resume_ms FROM progress");
    while (p.step_row()) {
      const std::int64_t state = p.i64(2);
      if (state != 1 && state != 2) continue;
      std::string spec = p.text(1);
      if (!specs.count(spec)) {
        ++n.skipped_rows;
        continue;
      }
      work[p.i64(0)][std::move(spec)] = {state, std::max<std::int64_t>(0, p.i64(3))};
    }
  }

  if (!exec(db, "BEGIN")) return err(status::io);
  bool ok = true;
  const auto rollback = [&](status s) -> expected {
    exec(db, "ROLLBACK");
    out.clear();
    return err(s);
  };

  // Which (local asset, spec) take the file's rows: keyed by the file's id.
  std::unordered_map<std::int64_t, std::int64_t> local_of;      // file asset -> local asset
  std::set<std::pair<std::int64_t, std::string>> take;           // (file asset, spec)
  {
    stmt a(x, "SELECT id, root_id, rel, mtime, size, kind, duration_ms FROM assets");
    stmt find(db, "SELECT id, mtime, size FROM assets WHERE path = ?1");
    stmt done(db, "SELECT state FROM progress WHERE asset_id = ?1 AND spec = ?2");
    stmt drop_f(db, "DELETE FROM frames WHERE asset_id = ?1 AND spec = ?2");
    stmt drop_s(db, "DELETE FROM speech WHERE asset_id = ?1 AND spec = ?2");
    stmt drop_p(db, "DELETE FROM progress WHERE asset_id = ?1 AND spec = ?2");
    stmt dur(db, "UPDATE assets SET duration_ms = MAX(duration_ms, ?2) WHERE id = ?1");
    stmt ins(db, "INSERT INTO assets(path, root_id, mtime, size, kind, duration_ms, seen)"
                 " VALUES(?1, ?2, ?3, ?4, ?5, ?6, 0)");
    std::uint64_t seen = 0;
    while (ok && a.step_row()) {
      const auto t = target_of.find(a.i64(1));
      if (t == target_of.end()) continue;
      const std::string rel = a.text(2);
      const std::int64_t kind = a.i64(5);
      if (!safe_rel(rel) || (kind != 1 && kind != 2)) continue;
      ++n.assets;
      imported_asset im;
      im.file_id = a.i64(0);
      im.path = local_path(t->second->dir, rel);
      im.mtime = a.i64(3);
      im.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, a.i64(4)));
      const std::int64_t duration = std::max<std::int64_t>(0, a.i64(6));
      const auto w = work.find(im.file_id);
      find.reset();
      if (find.bind(1, im.path).step_row()) {
        im.local_id = find.i64(0);
        const bool same = find.i64(1) == im.mtime && static_cast<std::uint64_t>(find.i64(2)) == im.size;
        if (!same) {
          // This machine saw the file and the export did not: it is newer here
          // (or older there); either way the export's vectors are not of it.
          ++n.kept;
          continue;
        }
        bool replaced = false;
        if (w != work.end()) {
          for (const auto& [spec, st] : w->second) {
            done.reset();
            const bool here_done = done.bind(1, im.local_id).bind(2, spec).step_row() && done.i64(0) == 2;
            if (here_done) continue;
            drop_f.reset();
            drop_s.reset();
            drop_p.reset();
            ok = ok && drop_f.bind(1, im.local_id).bind(2, spec).run() &&
                 drop_s.bind(1, im.local_id).bind(2, spec).run() &&
                 drop_p.bind(1, im.local_id).bind(2, spec).run();
            take.emplace(im.file_id, spec);
            replaced = true;
          }
        }
        dur.reset();
        ok = ok && dur.bind(1, im.local_id).bind(2, duration).run();
        ++(replaced ? n.replaced : n.kept);
      } else {
        ins.reset();
        ok = ok && ins.bind(1, im.path).bind(2, t->second->local_root).bind(3, im.mtime)
                       .bind(4, static_cast<std::int64_t>(im.size)).bind(5, kind).bind(6, duration).run();
        if (!ok) break;
        im.local_id = sqlite3_last_insert_rowid(db);
        im.added = true;
        ++n.added;
        if (w != work.end()) {
          for (const auto& [spec, st] : w->second) take.emplace(im.file_id, spec);
        }
      }
      local_of[im.file_id] = im.local_id;
      out.push_back(std::move(im));
      if ((++seen & 1023) == 0 && c.cancelled()) return rollback(status::cancelled);
    }
  }
  if (!ok) return rollback(status::io);
  c.report(0.2);

  // The rows themselves, in one pass over each table (the file's indexes, if
  // any, are not relied on).
  {
    stmt p(db, "INSERT INTO progress(asset_id, spec, state, resume_ms, indexed_at)"
               " VALUES(?1, ?2, ?3, ?4, strftime('%s','now'))");
    for (const auto& [file_id, spec] : take) {
      const auto& st = work[file_id][spec];
      p.reset();
      ok = ok && p.bind(1, local_of[file_id]).bind(2, spec).bind(3, st.first).bind(4, st.second).run();
    }
  }
  if (!ok) return rollback(status::io);
  {
    // Every vector of a spec has the spec's length (the first one decides);
    // a row that does not is dropped rather than trusted.
    std::map<std::string, std::size_t> dim;
    stmt f(x, "SELECT asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb FROM frames");
    stmt ins(db, "INSERT INTO frames(asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)"
                 " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    std::uint64_t i = 0;
    while (ok && f.step_row()) {
      const std::int64_t file_id = f.i64(0);
      std::string spec = f.text(1);
      if (!take.count({file_id, spec})) continue;
      const auto emb = f.blob(9);
      const double generic = f.real(7), scale = f.real(8);
      if (emb.empty() || emb.size() > 8192 || !std::isfinite(generic) || !std::isfinite(scale) || scale <= 0) continue;
      const auto [d, fresh] = dim.emplace(spec, emb.size());
      if (!fresh && d->second != emb.size()) continue;
      ins.reset();
      ok = ins.bind(1, local_of[file_id]).bind(2, spec).bind(3, std::max<std::int64_t>(-1, f.i64(2)))
               .bind(4, f.i64(3)).bind(5, f.i64(4)).bind(6, f.i64(5) == 0 ? 1 : f.i64(5)).bind(7, f.i64(6))
               .bind_real(8, generic).bind_real(9, scale).bind_blob(10, emb.data(), emb.size()).run();
      ++n.frames;
      if ((++i & 4095) == 0 && c.cancelled()) return rollback(status::cancelled);
    }
  }
  if (!ok) return rollback(status::io);
  c.report(0.6);
  {
    stmt s(x, "SELECT asset_id, spec, start_ms, end_ms, text FROM speech");
    stmt ins(db, "INSERT INTO speech(asset_id, spec, start_ms, end_ms, text) VALUES(?1, ?2, ?3, ?4, ?5)");
    while (ok && s.step_row()) {
      const std::int64_t file_id = s.i64(0);
      std::string spec = s.text(1);
      if (!take.count({file_id, spec})) continue;
      std::string text = s.text(4);
      if (text.size() > 16384) continue;
      ins.reset();
      ok = ins.bind(1, local_of[file_id]).bind(2, spec).bind(3, s.i64(2)).bind(4, s.i64(3)).bind(5, text).run();
      ++n.speech;
    }
  }
  if (!ok) return rollback(status::io);
  if (!exec(db, "COMMIT")) return rollback(status::io);
  c.report(0.7);
  return {};
}

expected import_faces(sqlite3* fdb, const std::string& file, const std::string& face_spec,
                      std::span<const imported_asset> assets, import_counts& n, const control& c) {
  MV_TRY(sqlite3* x, open_untrusted(file));
  db_closer closer{x};
  if (info_value(x, "faces") != "1" || info_value(x, "face_spec") != face_spec || face_spec.empty()) return {};

  // The assets that take the file's faces: scanned there, and neither scanned
  // nor holding faces here (a clip half-scanned here keeps its own).
  std::unordered_map<std::int64_t, const imported_asset*> by_file;
  for (const imported_asset& a : assets) by_file[a.file_id] = &a;
  std::unordered_set<std::int64_t> scanned_there;
  {
    stmt s(x, "SELECT asset_id FROM face_scanned WHERE spec = ?1");
    s.bind(1, face_spec);
    while (s.step_row()) scanned_there.insert(s.i64(0));
  }
  std::unordered_map<std::int64_t, const imported_asset*> take;
  {
    stmt here(fdb, "SELECT (SELECT COUNT(*) FROM scanned WHERE asset_id = ?1) +"
                   " (SELECT COUNT(*) FROM faces WHERE asset_id = ?1)");
    for (std::int64_t id : scanned_there) {
      const auto it = by_file.find(id);
      if (it == by_file.end()) continue;
      here.reset();
      if (here.bind(1, it->second->local_id).step_row() && here.i64(0) == 0) take[id] = it->second;
    }
  }
  if (take.empty()) return {};

  if (!exec(fdb, "BEGIN")) return err(status::io);
  bool ok = true;
  const auto rollback = [&](status s) -> expected {
    exec(fdb, "ROLLBACK");
    return err(s);
  };
  // People: named ones join the person of the same name here; the rest are
  // new. Made only when a face lands in them.
  std::unordered_map<std::int64_t, std::pair<std::string, std::int64_t>> file_people;  // name, created
  {
    stmt p(x, "SELECT id, name, created_at FROM people");
    while (p.step_row()) {
      std::string name = p.text(1);
      if (name.size() > 256) name.resize(256);
      file_people[p.i64(0)] = {std::move(name), p.i64(2)};
    }
  }
  std::unordered_map<std::int64_t, std::int64_t> person_here;
  stmt by_name(fdb, "SELECT id FROM people WHERE name != '' AND lower(name) = lower(?1) ORDER BY id LIMIT 1");
  stmt new_person(fdb, "INSERT INTO people(name, created_at) VALUES(?1, ?2)");
  const auto person_for = [&](std::int64_t file_person) -> std::int64_t {
    const auto done = person_here.find(file_person);
    if (done != person_here.end()) return done->second;
    const auto it = file_people.find(file_person);
    if (it == file_people.end()) return 0;
    std::int64_t id = 0;
    if (!it->second.first.empty()) {
      by_name.reset();
      if (by_name.bind(1, it->second.first).step_row()) {
        id = by_name.i64(0);
        ++n.people_joined;
      }
    }
    if (id == 0) {
      new_person.reset();
      if (!new_person.bind(1, it->second.first).bind(2, it->second.second).run()) {
        ok = false;
        return 0;
      }
      id = sqlite3_last_insert_rowid(fdb);
      ++n.people_new;
    }
    person_here[file_person] = id;
    return id;
  };

  std::size_t dim_bytes = 0;
  std::unordered_map<std::int64_t, std::int64_t> face_here;
  {
    stmt f(x, "SELECT id, asset_id, pts_ms, x, y, w, h, score, person_id, emb, pinned, quality, tta FROM faces");
    stmt ins(fdb, "INSERT INTO faces(asset_id, path, pts_ms, x, y, w, h, score, person_id, emb, pinned, quality,"
                  " tta, spec) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)");
    const auto unit = [](double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; };
    std::uint64_t i = 0;
    while (ok && f.step_row()) {
      const auto a = take.find(f.i64(1));
      if (a == take.end()) continue;
      const auto emb = f.blob(9);
      if (emb.empty() || emb.size() > 8192 || !finite_floats(emb)) continue;
      if (dim_bytes == 0) dim_bytes = emb.size();
      if (emb.size() != dim_bytes) continue;
      const std::int64_t person = f.is_null(8) ? 0 : person_for(f.i64(8));
      if (!ok) break;
      ins.reset();
      ins.bind(1, a->second->local_id).bind(2, a->second->path).bind(3, std::max<std::int64_t>(-1, f.i64(2)))
          .bind_real(4, unit(f.real(3))).bind_real(5, unit(f.real(4))).bind_real(6, unit(f.real(5)))
          .bind_real(7, unit(f.real(6))).bind_real(8, unit(f.real(7)));
      if (person > 0) {
        ins.bind(9, person);
      } else {
        ins.bind_null(9);
      }
      ins.bind_blob(10, emb.data(), emb.size()).bind(11, std::int64_t{f.i64(10) != 0 ? 1 : 0});
      if (f.is_null(11)) {
        ins.bind_null(12);
      } else {
        ins.bind_real(12, unit(f.real(11)));
      }
      ins.bind(13, std::int64_t{f.i64(12) != 0 ? 1 : 0}).bind(14, face_spec);
      ok = ins.run();
      face_here[f.i64(0)] = sqlite3_last_insert_rowid(fdb);
      ++n.faces;
      if ((++i & 1023) == 0 && c.cancelled()) return rollback(status::cancelled);
    }
  }
  if (!ok) return rollback(status::io);
  {
    // The user's corrections travel with their faces and people.
    stmt r(x, "SELECT face_id, person_id FROM rejected");
    stmt ins(fdb, "INSERT OR IGNORE INTO rejected(face_id, person_id) VALUES(?1, ?2)");
    while (ok && r.step_row()) {
      const auto fh = face_here.find(r.i64(0));
      const auto ph = person_here.find(r.i64(1));
      if (fh == face_here.end() || ph == person_here.end()) continue;
      ins.reset();
      ok = ins.bind(1, fh->second).bind(2, ph->second).run();
    }
    stmt m(x, "SELECT a, b FROM no_merge");
    stmt nm(fdb, "INSERT OR IGNORE INTO no_merge(a, b) VALUES(?1, ?2)");
    while (ok && m.step_row()) {
      const auto pa = person_here.find(m.i64(0));
      const auto pb = person_here.find(m.i64(1));
      if (pa == person_here.end() || pb == person_here.end() || pa->second == pb->second) continue;
      nm.reset();
      ok = nm.bind(1, pa->second).bind(2, pb->second).run();
    }
    stmt sc(fdb, "INSERT OR IGNORE INTO scanned(asset_id, spec) VALUES(?1, ?2)");
    for (const auto& [file_id, a] : take) {
      if (!ok) break;
      sc.reset();
      ok = sc.bind(1, a->local_id).bind(2, face_spec).run();
    }
  }
  if (!ok) return rollback(status::io);
  if (!exec(fdb, "COMMIT")) return rollback(status::io);
  return {};
}

expected import_thumbs(const std::string& file, std::span<const imported_asset> assets, const thumb_io& io,
                       import_counts& n, const control& c) {
  if (!io.store || !io.stat) return {};
  MV_TRY(sqlite3* x, open_untrusted(file));
  db_closer closer{x};
  if (info_value(x, "thumbs") != "1") return {};
  std::unordered_map<std::int64_t, const imported_asset*> by_file;
  for (const imported_asset& a : assets) by_file[a.file_id] = &a;
  // The file here must be the file there: a thumbnail of other pixels, under
  // this file's stamp, would show until the file changes again.
  std::unordered_map<std::int64_t, bool> same;
  stmt t(x, "SELECT asset_id, pts_ms, jpeg FROM thumbs");
  const double total = static_cast<double>(std::max<std::uint64_t>(1, count_of(x, "SELECT COUNT(*) FROM thumbs")));
  std::uint64_t i = 0;
  while (t.step_row()) {
    if ((++i & 63) == 0) {
      if (c.cancelled()) return err(status::cancelled);
      c.report(0.8 + 0.2 * static_cast<double>(i) / total);
    }
    const auto a = by_file.find(t.i64(0));
    if (a == by_file.end()) continue;
    auto s = same.find(a->second->file_id);
    if (s == same.end()) {
      std::int64_t mtime = 0;
      std::uint64_t size = 0;
      const bool match = io.stat(a->second->path, mtime, size) && mtime == a->second->mtime && size == a->second->size;
      s = same.emplace(a->second->file_id, match).first;
    }
    if (!s->second) {
      ++n.thumbs_skipped;
      continue;
    }
    if (io.yield) io.yield();
    const auto jpeg = t.blob(2);
    if (io.store(a->second->path, std::max<std::int64_t>(-1, t.i64(1)), jpeg)) {
      ++n.thumbs;
    } else {
      ++n.thumbs_skipped;
    }
  }
  return {};
}

}  // namespace mv::ai::transfer
