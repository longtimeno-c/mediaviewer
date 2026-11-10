// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/catalog.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

#include "addons/ai/faces.h"
#include "addons/ai/index_db.h"
#include "addons/ai/nfc.h"
#include "addons/ai/sha256.h"
#include "core/json.h"

namespace mv::ai::catalog {
namespace fs = std::filesystem;
namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

class stmt {
 public:
  stmt(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
  }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
  stmt& bind(int i, const std::string& v) {
    sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
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
  stmt& bind_blob(int i, std::span<const std::uint8_t> v) {
    sqlite3_bind_blob(s_, i, v.empty() ? reinterpret_cast<const void*>("") : v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  void reset() {
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
    failed = false;
  }
  [[nodiscard]] std::int64_t i64(int col) const { return sqlite3_column_int64(s_, col); }
  [[nodiscard]] double real(int col) const { return sqlite3_column_double(s_, col); }
  [[nodiscard]] std::span<const std::uint8_t> blob(int col) const {
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(s_, col));
    return p ? std::span<const std::uint8_t>(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, col)))
             : std::span<const std::uint8_t>();
  }
  // A row, or false at the end; a read error is `failed`, never "no more rows".
  bool next() {
    if (!s_) {
      failed = true;
      return false;
    }
    const int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) failed = true;
    return false;
  }
  [[nodiscard]] bool run() {
    (void)next();
    return !failed;
  }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = sqlite3_column_text(s_, col);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s_, col)))
             : std::string();
  }
  bool failed = false;

 private:
  sqlite3_stmt* s_ = nullptr;
};

struct db_closer {
  sqlite3* db;
  ~db_closer() { sqlite3_close_v2(db); }
};

// A table's columns in `schema`, in order. Hidden FTS columns are not listed.
std::vector<std::string> columns_of(sqlite3* db, const char* schema, const char* table) {
  std::vector<std::string> out;
  stmt q(db, "SELECT name FROM pragma_table_info(?1, ?2)");
  q.bind(1, std::string(table)).bind(2, std::string(schema));
  while (q.next()) out.push_back(q.text(0));
  return out;
}

// Both sides of a copy: the columns the target has, and those the source has too.
std::vector<std::string> shared_columns(sqlite3* db, const char* table) {
  const auto there = columns_of(db, "src", table);
  const std::set<std::string> from(there.begin(), there.end());
  std::vector<std::string> out;
  for (std::string& c : columns_of(db, "main", table)) {
    if (from.count(c)) out.push_back(std::move(c));
  }
  return out;
}

std::string quoted(const std::string& name) { return "\"" + name + "\""; }

std::string join_columns(const std::vector<std::string>& cols) {
  std::string out;
  for (const std::string& c : cols) out += (out.empty() ? "" : ", ") + quoted(c);
  return out;
}

// nfc(text), for the INSERT .. SELECTs below.
void sql_nfc(sqlite3_context* ctx, int, sqlite3_value** v) {
  const auto* p = reinterpret_cast<const char*>(sqlite3_value_text(v[0]));
  if (!p) {
    sqlite3_result_null(ctx);
    return;
  }
  const std::string out = nfc(std::string_view(p, static_cast<std::size_t>(sqlite3_value_bytes(v[0]))));
  sqlite3_result_text(ctx, out.data(), static_cast<int>(out.size()), SQLITE_TRANSIENT);
}

// A path of this machine, as the phone names it: /<root uuid>/<relative path>.
std::string relocated(const std::string& column) {
#if defined(_WIN32)
  const std::string rest = "replace(substr(" + quoted(column) + ", length(?2) + 2), '\\', '/')";
#else
  // A backslash is an ordinary character in a POSIX file name.
  const std::string rest = "substr(" + quoted(column) + ", length(?2) + 2)";
#endif
  return "CASE WHEN COALESCE(" + quoted(column) + ", '') = '' THEN " + quoted(column) + " ELSE '/' || ?1 || '/' || " +
         rest + " END";
}

std::string trimmed(std::string path) {
#if defined(_WIN32)
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
#else
  while (path.size() > 1 && path.back() == '/') path.pop_back();
#endif
  return path;
}

// Keeps the answering tower and everything that is not a picture tower (sound,
// document pages): a migration leaves two towers' vectors in the index, and a
// phone must not carry the one nothing answers from.
const char* const kSpecFilter =
    "(spec = ?1 OR NOT (spec GLOB 'clip-vit-*' OR spec GLOB 'openclip-*'))";

std::string slurp(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A local file of the generation: deliver() copies it with a verified, flushed
// copy, so a power cut here costs only this build.
[[nodiscard]] bool write_text(const std::string& path, std::string_view text) {
  std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return static_cast<bool>(out);
}

bool cancelled(const control& c) { return c.cancelled && c.cancelled(); }
void pause_for_viewer(const control& c) {
  if (c.yield) c.yield();
}
void report(const control& c, double f) {
  if (c.report) c.report(f);
}

// Each asset's place label, read once here because a phone has no Places
// (docs/plans/mobile.md §5). In batches, so a large library yields between them.
expected store_places(sqlite3* db, const build_options& o, build_counts& n, const control& c) {
  if (!o.place_labels) return {};
  std::vector<std::pair<std::int64_t, std::pair<double, double>>> at;
  {
    stmt q(db, "SELECT id, lat, lon FROM main.assets WHERE lat IS NOT NULL AND lon IS NOT NULL");
    while (q.next()) at.push_back({q.i64(0), {q.real(1), q.real(2)}});
    if (q.failed) return err(status::io);
  }
  static constexpr std::size_t kBatch = 256;
  stmt up(db, "UPDATE main.assets SET place = ?2 WHERE id = ?1");
  for (std::size_t i = 0; i < at.size(); i += kBatch) {
    if (cancelled(c)) return err(status::cancelled);
    pause_for_viewer(c);
    std::vector<std::pair<double, double>> points;
    for (std::size_t k = i; k < std::min(at.size(), i + kBatch); ++k) points.push_back(at[k].second);
    auto labels = o.place_labels(points);
    if (!labels) {
      if (labels.error() == status::not_found) return {};  // no Places here: a place term finds nothing, as on the desktop
      return err(labels.error());
    }
    for (std::size_t k = 0; k < points.size() && k < labels->size(); ++k) {
      if ((*labels)[k].empty()) continue;
      up.reset();
      if (!up.bind(1, at[i + k].first).bind(2, (*labels)[k]).run()) return err(status::io);
      ++n.places;
    }
  }
  return {};
}

expected copy_tables(sqlite3* db, const build_options& o, build_counts& n, const control& c) {
  const auto step = [&](double f) -> expected {
    if (cancelled(c)) return err(status::cancelled);
    report(c, f);
    pause_for_viewer(c);
    return {};
  };
  if (!exec(db, "BEGIN")) return err(status::io);
  // meta first: the schema row was made by index_db::open and stays this build's.
  if (!exec(db, "INSERT OR REPLACE INTO main.meta(key, value) SELECT key, value FROM src.meta WHERE key != 'schema'")) {
    return err(status::io);
  }
  {
    const auto cols = shared_columns(db, "roots");
    std::string select;
    for (const std::string& col : cols) {
      select += (select.empty() ? "" : ", ") + (col == "path" ? std::string("'/' || ?1") : quoted(col));
    }
    for (const root_map& r : o.roots) {
      stmt one(db, "INSERT INTO main.roots(" + join_columns(cols) + ") SELECT " + select + " FROM src.roots WHERE id = ?2");
      if (!one.bind(1, r.uuid).bind(2, r.id).run()) return err(status::io);
    }
  }
  {
    // N7: the name a phone matches is NFC, whatever form this disk holds, and
    // the bytes the file really has stay beside it (NULL when they are the
    // same), because the NAS opens by those. Two files that differ only in
    // form exist on a case-sensitive share: the one already in NFC keeps the
    // name, the other keeps its bytes, and neither is lost.
    const auto cols = shared_columns(db, "assets");
    std::string normal, raw;
    for (const std::string& col : cols) {
      const bool path = col == "path" || col == "motion";
      normal += (normal.empty() ? "" : ", ") + (path ? "nfc(" + relocated(col) + ")" : quoted(col));
      raw += (raw.empty() ? "" : ", ") + (path ? relocated(col) : quoted(col));
    }
    const std::string names = join_columns(cols);
    for (const root_map& r : o.roots) {
      const std::string at = trimmed(r.path);
      stmt first(db, "INSERT OR IGNORE INTO main.assets(" + names + ") SELECT " + normal +
                         " FROM src.assets WHERE root_id = ?3 ORDER BY path = nfc(path) DESC");
      if (!first.bind(1, r.uuid).bind(2, at).bind(3, r.id).run()) return err(status::io);
      stmt rest(db, "INSERT INTO main.assets(" + names + ") SELECT " + raw +
                        " FROM src.assets WHERE root_id = ?3 AND id NOT IN (SELECT id FROM main.assets)");
      if (!rest.bind(1, r.uuid).bind(2, at).bind(3, r.id).run()) return err(status::io);
      stmt bytes(db, "UPDATE main.assets SET disk_path = (SELECT " + relocated("path") +
                         " FROM src.assets s WHERE s.id = assets.id), disk_motion = (SELECT " + relocated("motion") +
                         " FROM src.assets s WHERE s.id = assets.id) WHERE root_id = ?3");
      if (!bytes.bind(1, r.uuid).bind(2, at).bind(3, r.id).run()) return err(status::io);
    }
    if (!exec(db, "UPDATE main.assets SET disk_path = NULL WHERE disk_path = path") ||
        !exec(db, "UPDATE main.assets SET disk_motion = NULL WHERE disk_motion IS motion")) {
      return err(status::io);
    }
  }
  MV_TRY_VOID(step(0.15));
  // The rows of the chosen assets, table by table. A table this index does not
  // have (never expected) is simply not copied: its columns come back empty.
  struct linked {
    const char* table;
    const char* where;
    bool spec_bound;
  };
  static constexpr linked kLinked[] = {
      {"progress", "asset_id IN (SELECT id FROM main.assets)", false},
      {"frames", "asset_id IN (SELECT id FROM main.assets)", true},
      {"speech", "asset_id IN (SELECT id FROM main.assets)", false},
      {"cues", "asset_id IN (SELECT id FROM main.assets)", false},
      {"tracks", "asset_id IN (SELECT id FROM main.assets)", false},
      {"passages", "asset_id IN (SELECT id FROM main.assets)", false},
      {"passage_vecs", "passage_id IN (SELECT id FROM main.passages)", false},
  };
  double f = 0.15;
  for (const linked& t : kLinked) {
    const auto cols = shared_columns(db, t.table);
    if (!cols.empty()) {
      std::string sql = "INSERT INTO main." + std::string(t.table) + "(" + join_columns(cols) + ") SELECT " +
                        join_columns(cols) + " FROM src." + t.table + " WHERE " + t.where;
      if (t.spec_bound) sql += std::string(" AND ") + kSpecFilter;
      stmt in(db, sql);
      if (t.spec_bound) in.bind(1, o.active_spec);
      if (!in.run()) return err(status::io);
    }
    f += 0.1;
    MV_TRY_VOID(step(f));
  }
  // Search text: rows of a full-text table are addressed by rowid.
  for (const auto& [table, owner] : {std::pair{"asset_text", "assets"}, std::pair{"passage_text", "passages"}}) {
    const auto cols = shared_columns(db, table);
    if (cols.empty()) continue;
    const std::string sql = "INSERT INTO main." + std::string(table) + "(rowid, " + join_columns(cols) +
                            ") SELECT rowid, " + join_columns(cols) + " FROM src." + table +
                            " WHERE rowid IN (SELECT id FROM main." + owner + ")";
    stmt in(db, sql);
    if (!in.run()) return err(status::io);
  }
  MV_TRY_VOID(store_places(db, o, n, c));
  {
    stmt q(db, "SELECT COUNT(*) FROM main.assets");
    if (!q.next()) return err(status::io);
    n.assets = static_cast<std::uint64_t>(std::stoll(q.text(0)));
  }
  {
    stmt q(db, "SELECT COUNT(*) FROM main.frames WHERE spec = ?1");
    q.bind(1, o.active_spec);
    if (!q.next()) return err(status::io);
    n.frames = static_cast<std::uint64_t>(std::stoll(q.text(0)));
  }
  {
    stmt q(db, "SELECT value FROM main.meta WHERE key = 'schema'");
    if (!q.next()) return err(status::io);
    n.schema = std::stoi(q.text(0));
  }
  if (!exec(db, "COMMIT")) return err(status::io);
  return {};
}

expected make_database(const build_options& o, const std::string& file, build_counts& n, const control& c) {
  {
    // The same migrations as the app's own index, so a reader cannot tell it apart.
    MV_TRY(auto fresh, index_db::open(file));
    (void)fresh;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(status::io);
  }
  {
    db_closer closer{db};
    sqlite3_busy_timeout(db, 5000);
    // One file, no journal beside it: it is thrown away if this fails, and a
    // phone opens it read-only.
    if (!exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=FULL; PRAGMA foreign_keys=OFF;")) return err(status::io);
    // The phone's name and the disk's: nothing but a catalogue has these.
    if (!exec(db, "ALTER TABLE assets ADD COLUMN disk_path TEXT; ALTER TABLE assets ADD COLUMN disk_motion TEXT;")) {
      return err(status::io);
    }
    if (sqlite3_create_function(db, "nfc", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, &sql_nfc, nullptr, nullptr) != SQLITE_OK) {
      return err(status::io);
    }
    {
      stmt a(db, "ATTACH ?1 AS src");
      if (!a.bind(1, o.index_db).run()) return err(status::io);
    }
    MV_TRY_VOID(copy_tables(db, o, n, c));
    if (!exec(db, "DETACH src")) return err(status::io);
    stmt q(db, "PRAGMA quick_check(1)");
    if (!q.next() || q.text(0) != "ok") return err(status::corrupt);
  }
  return {};
}

// A faces.db a read-only engine opens as its own (faces_db::open_read_only),
// made from the app's schema: named people and which assets and moments they
// are in. Never an embedding (owner call 7): `emb` is empty and the box is
// dropped, so nothing of a face leaves this machine but who it was.
expected make_people(const build_options& o, const std::string& catalogue, const std::string& file, build_counts& n) {
  {
    MV_TRY(auto fresh, faces_db::open(file, face_tuning{}, 128, "names-only"));
    (void)fresh;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(status::io);
  }
  db_closer closer{db};
  sqlite3_busy_timeout(db, 5000);
  if (!exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=FULL; PRAGMA foreign_keys=OFF;")) return err(status::io);
  {
    stmt a(db, "ATTACH ?1 AS src");
    if (!a.bind(1, o.faces_db).run()) return err(status::io);
  }
  {
    stmt a(db, "ATTACH ?1 AS cat");
    if (!a.bind(1, catalogue).run()) return err(status::io);
  }
  if (!exec(db, "BEGIN") ||
      !exec(db, "INSERT INTO main.people(id, name, created_at) SELECT id, name, created_at FROM src.people WHERE name != ''") ||
      !exec(db, "INSERT INTO main.faces(id, asset_id, path, pts_ms, x, y, w, h, score, person_id, emb, pinned, quality, tta, spec)"
                " SELECT f.id, f.asset_id, a.path, f.pts_ms, 0, 0, 0, 0, f.score, f.person_id, zeroblob(0), 0, NULL, 0, 'names-only'"
                " FROM src.faces f JOIN cat.assets a ON a.id = f.asset_id"
                " WHERE f.person_id IN (SELECT id FROM main.people)") ||
      !exec(db, "COMMIT")) {
    return err(status::io);
  }
  {
    stmt q(db, "SELECT COUNT(*) FROM main.people");
    if (!q.next()) return err(status::io);
    n.people = static_cast<std::uint64_t>(q.i64(0));
  }
  if (!exec(db, "DETACH cat") || !exec(db, "DETACH src")) return err(status::io);
  return {};
}

void put_u32(std::ostream& out, std::uint32_t v) {
  char b[4];
  for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFFu);
  out.write(b, 4);
}
void put_u64(std::ostream& out, std::uint64_t v) {
  char b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFFu);
  out.write(b, 8);
}

constexpr char kTinyMagic[8] = {'M', 'V', 'T', 'I', 'N', 'Y', '0', '1'};
constexpr char kIdxMagic[8] = {'M', 'V', 'T', 'I', 'D', 'X', '0', '1'};
constexpr std::size_t kPackNameBytes = 40;   // "t-" + 32 hex + ".pack", NUL padded
constexpr std::size_t kTinyRow = 24;         // asset, offset, length, 0
constexpr std::size_t kIdxRow = 24;          // asset, pack, length, offset

// tiny.pack and thumbs.idx from the ledger, for the assets of this catalogue,
// sorted by asset id. Tiles come from a pack already sealed, or not at all.
expected write_thumb_files(const build_options& o, const std::string& catalogue, std::vector<std::string>& names,
                           build_counts& n) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(catalogue.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(db);
    return err(status::io);
  }
  db_closer closer{db};
  sqlite3_busy_timeout(db, 5000);
  for (const auto& [as, file] : {std::pair{"src", o.index_db}, std::pair{"led", o.thumbs->ledger_path()}}) {
    stmt a(db, std::string("ATTACH ?1 AS ") + as);
    if (!a.bind(1, file).run()) return err(status::io);
  }
  const char* const kJoin =
      " FROM main.assets m JOIN src.assets a ON a.id = m.id"
      " JOIN led.thumbs l ON l.path = a.path AND l.size = a.size AND l.mtime = a.mtime"
      " WHERE l.pack IS NOT NULL AND l.length > 0";
  std::vector<std::string> packs;
  std::map<std::string, std::pair<std::uint64_t, std::string>> pack_facts;  // size, hash: a delivered one is not kept here
  {
    stmt q(db, "SELECT name, size, sha256 FROM led.packs");
    while (q.next()) pack_facts[q.text(0)] = {static_cast<std::uint64_t>(q.i64(1)), q.text(2)};
    if (q.failed) return err(status::io);
  }
  {
    std::uint64_t tiny_count = 0;
    {
      stmt q(db, std::string("SELECT COUNT(*)") + kJoin + " AND length(l.tiny) > 0");
      if (!q.next()) return err(status::io);
      tiny_count = static_cast<std::uint64_t>(q.i64(0));
    }
    std::ofstream out(fs_path(o.out_dir + "/tiny.pack"), std::ios::binary | std::ios::trunc);
    out.write(kTinyMagic, 8);
    put_u32(out, static_cast<std::uint32_t>(tiny_count));
    put_u32(out, 0);
    std::uint64_t at = 16 + tiny_count * kTinyRow;
    {
      stmt q(db, std::string("SELECT m.id, length(l.tiny)") + kJoin + " AND length(l.tiny) > 0 ORDER BY m.id");
      while (q.next()) {
        put_u64(out, static_cast<std::uint64_t>(q.i64(0)));
        put_u64(out, at);
        put_u32(out, static_cast<std::uint32_t>(q.i64(1)));
        put_u32(out, 0);
        at += static_cast<std::uint64_t>(q.i64(1));
      }
      if (q.failed) return err(status::io);
    }
    {
      stmt q(db, std::string("SELECT l.tiny") + kJoin + " AND length(l.tiny) > 0 ORDER BY m.id");
      while (q.next()) {
        const auto b = q.blob(0);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
      }
      if (q.failed) return err(status::io);
    }
    out.flush();
    if (!out) return err(status::io);
    n.thumbs = tiny_count;
  }
  {
    stmt q(db, std::string("SELECT DISTINCT l.pack") + kJoin + " ORDER BY l.pack");
    while (q.next()) packs.push_back(q.text(0));
    if (q.failed) return err(status::io);
  }
  std::uint64_t entries = 0;
  {
    stmt q(db, std::string("SELECT COUNT(*)") + kJoin);
    if (!q.next()) return err(status::io);
    entries = static_cast<std::uint64_t>(q.i64(0));
  }
  std::ofstream out(fs_path(o.out_dir + "/thumbs.idx"), std::ios::binary | std::ios::trunc);
  out.write(kIdxMagic, 8);
  put_u32(out, static_cast<std::uint32_t>(packs.size()));
  put_u32(out, static_cast<std::uint32_t>(entries));
  for (const std::string& name : packs) {
    std::string fixed = name;
    fixed.resize(kPackNameBytes, '\0');
    out.write(fixed.data(), static_cast<std::streamsize>(kPackNameBytes));
  }
  {
    std::map<std::string, std::uint32_t> pack_at;
    for (std::uint32_t i = 0; i < packs.size(); ++i) pack_at[packs[i]] = i;
    stmt q(db, std::string("SELECT m.id, l.pack, l.length, l.offset") + kJoin + " ORDER BY m.id");
    while (q.next()) {
      put_u64(out, static_cast<std::uint64_t>(q.i64(0)));
      put_u32(out, pack_at[q.text(1)]);
      put_u32(out, static_cast<std::uint32_t>(q.i64(2)));
      put_u64(out, static_cast<std::uint64_t>(q.i64(3)));
    }
    if (q.failed) return err(status::io);
  }
  out.flush();
  if (!out) return err(status::io);
  names.push_back("tiny.pack");
  names.push_back("thumbs.idx");
  for (const std::string& name : packs) {
    const std::string local = o.thumbs->dir() + "/" + name;
    shared_file f;
    f.source = local;
    f.rel = "thumbs/" + name;
    const auto facts = pack_facts.find(name);
    if (facts == pack_facts.end()) return err(status::corrupt);
    f.size = facts->second.first;
    f.sha256 = facts->second.second;
    n.shared.push_back(std::move(f));
  }
  return {};
}

void add_file_row(json::writer& w, const std::string& dir, const std::string& name) {
  std::error_code ec;
  const auto size = fs::file_size(fs_path(dir + "/" + name), ec);
  const auto hash = sha256_file_hex(dir + "/" + name);
  w.begin_object();
  w.key("name").string(name);
  w.key("size").integer(ec ? 0 : static_cast<std::int64_t>(size));
  w.key("sha256").string(hash.value_or(std::string()));
  w.end_object();
}

}  // namespace

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> make_keypair() {
  (void)sodium_init();
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES), sec(crypto_sign_ed25519_SECRETKEYBYTES);
  crypto_sign_ed25519_keypair(pub.data(), sec.data());
  return {std::move(pub), std::move(sec)};
}

std::vector<std::uint8_t> public_of(std::span<const std::uint8_t> secret) {
  if (secret.size() != crypto_sign_ed25519_SECRETKEYBYTES) return {};
  std::vector<std::uint8_t> pub(crypto_sign_ed25519_PUBLICKEYBYTES);
  if (crypto_sign_ed25519_sk_to_pk(pub.data(), secret.data()) != 0) return {};
  return pub;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15u]);
  }
  return out;
}

std::string fingerprint(std::span<const std::uint8_t> public_key) {
  sha256 h;
  h.update(public_key);
  const std::string hex = h.finish_hex();
  return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4);
}

namespace {

// Exactly the bytes a signature covers. Each text field has its length first,
// so no two different sets of fields can read as the same message.
std::string signed_message(const current_info& c) {
  const auto number = [](std::uint64_t v) {
    std::string s(8, '\0');
    for (int i = 0; i < 8; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFFu);
    return s;
  };
  std::string m = "mediaviewer.catalog.current.v1";
  m += number(c.library_id.size()) + c.library_id;
  m += number(c.epoch);
  m += number(c.generation);
  m += number(c.manifest_sha256.size()) + c.manifest_sha256;
  return m;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  if (hex.size() % 2 != 0) return {};
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return {};
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return out;
}

}  // namespace

std::string signed_current_json(const current_info& c, std::span<const std::uint8_t> secret_key) {
  if (secret_key.size() != crypto_sign_ed25519_SECRETKEYBYTES) return {};
  const std::string message = signed_message(c);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(), secret_key.data()) != 0) {
    return {};
  }
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(c.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(c.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(c.generation));
  w.key("manifest_sha256").string(c.manifest_sha256);
  w.key("signature").string(to_hex(signature));
  w.end_object();
  return w.take();
}

result<current_info> verify_current(std::string_view text, std::span<const std::uint8_t> public_key) {
  (void)sodium_init();
  if (public_key.size() != crypto_sign_ed25519_PUBLICKEYBYTES) return err(status::invalid_arg);
  const auto doc = json::parse(text, 4);
  if (!doc) return err(status::corrupt);
  const std::string* id = doc->str("library_id");
  const std::string* manifest = doc->str("manifest_sha256");
  const std::string* signature = doc->str("signature");
  const auto epoch = doc->integer("epoch");
  const auto generation = doc->integer("generation");
  if (!id || !manifest || !signature || !epoch || !generation || *epoch < 0 || *generation < 0) return err(status::corrupt);
  const std::vector<std::uint8_t> sig = from_hex(*signature);
  if (sig.size() != crypto_sign_ed25519_BYTES) return err(status::corrupt);
  current_info c;
  c.library_id = *id;
  c.epoch = static_cast<std::uint64_t>(*epoch);
  c.generation = static_cast<std::uint64_t>(*generation);
  c.manifest_sha256 = *manifest;
  const std::string m = signed_message(c);
  if (crypto_sign_ed25519_verify_detached(sig.data(), reinterpret_cast<const unsigned char*>(m.data()), m.size(),
                                          public_key.data()) != 0) {
    return err(status::permission_denied);
  }
  return c;
}

result<build_counts> build(const build_options& o, const control& c) {
  if (o.roots.empty() || o.active_spec.empty()) return err(status::invalid_arg);
  std::error_code ec;
  fs::remove_all(fs_path(o.out_dir), ec);
  fs::create_directories(fs_path(o.out_dir), ec);
  if (ec) return err(status::io);
  const auto fail = [&](status s) -> result<build_counts> {
    std::error_code e2;
    fs::remove_all(fs_path(o.out_dir), e2);
    return err(s);
  };
  build_counts n;
  const std::string file = o.out_dir + "/catalog.db";
  if (const expected made = make_database(o, file, n, c); !made) return fail(made.error());

  std::vector<std::string> names{"catalog.db"};
  if (!write_text(o.out_dir + "/settings.json", o.settings_json)) return fail(status::io);
  names.push_back("settings.json");
  if (!o.faces_db.empty() && fs::exists(fs_path(o.faces_db), ec)) {
    if (const expected made = make_people(o, file, o.out_dir + "/faces.db", n); !made) return fail(made.error());
    names.push_back("faces.db");
  }
  if (o.thumbs) {
    if (const expected made = write_thumb_files(o, file, names, n); !made) return fail(made.error());
  }
  if (!o.labels_file.empty() && fs::exists(fs_path(o.labels_file), ec)) {
    fs::copy_file(fs_path(o.labels_file), fs_path(o.out_dir + "/labels.f32"), ec);
    if (ec) return fail(status::io);
    names.push_back("labels.f32");
  }
  if (cancelled(c)) return fail(status::cancelled);

  // Every shared file a reader uses goes under the signature: an ONNX model is
  // parsed by a native runtime, so nothing outside the manifest reaches a phone.
  struct named {
    std::string name, rel, sha256;
    std::uint64_t size = 0;
  };
  const auto describe = [&](shared_file& f, const std::string& name) -> result<named> {
    if (!f.sha256.empty()) return named{name, f.rel, f.sha256, f.size};
    std::error_code e;
    f.size = static_cast<std::uint64_t>(fs::file_size(fs_path(f.source), e));
    if (e) return err(status::io);
    std::string hash;
    if (o.thumbs) {
      if (auto kept = o.thumbs->hash_of(f.source)) hash = std::move(*kept);
    }
    if (hash.empty()) {
      const auto made = sha256_file_hex(f.source);
      if (!made) return err(status::io);
      hash = *made;
    }
    f.sha256 = hash;
    return named{name, f.rel, hash, f.size};
  };
  std::vector<named> pack_rows, preview_rows, model_rows;
  for (shared_file& f : n.shared) {
    auto d = describe(f, utf8_of(fs_path(f.rel).filename()));
    if (!d) return fail(d.error());
    pack_rows.push_back(std::move(*d));
  }
  for (const std::string& pack : o.preview_packs) {
    shared_file f;
    f.source = pack;
    const std::string name = utf8_of(fs_path(pack).filename());
    f.rel = "previews/" + name;
    auto d = describe(f, name);
    if (!d) return fail(d.error());
    preview_rows.push_back(std::move(*d));
    n.shared.push_back(std::move(f));
  }
  for (const std::string& model : o.model_files) {
    shared_file f;
    f.source = model;
    const std::string name = utf8_of(fs_path(model).filename());
    auto d = describe(f, name);
    if (!d) return fail(d.error());
    // Named by its content, so a file that is there is the file that is meant.
    d->rel = "models/" + o.model_spec + "/" + d->sha256.substr(0, 16) + "-" + name;
    f.rel = d->rel;
    model_rows.push_back(std::move(*d));
    n.shared.push_back(std::move(f));
    if (cancelled(c)) return fail(status::cancelled);
  }

  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(o.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(o.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(o.generation));
  w.key("created").integer(o.created_unix);
  w.key("schema").integer(n.schema);
  w.key("active_spec").string(o.active_spec);
  w.key("assets").integer(static_cast<std::int64_t>(n.assets));
  w.key("frames").integer(static_cast<std::int64_t>(n.frames));
  w.key("thumbs").integer(static_cast<std::int64_t>(n.thumbs));
  w.key("people").integer(static_cast<std::int64_t>(n.people));
  w.key("places").integer(static_cast<std::int64_t>(n.places));
  const auto rows = [&](const char* key, const std::vector<named>& list) {
    w.key(key).begin_array();
    for (const named& f : list) {
      w.begin_object();
      w.key("name").string(f.name);
      w.key("path").string(f.rel);
      w.key("size").integer(static_cast<std::int64_t>(f.size));
      w.key("sha256").string(f.sha256);
      w.end_object();
    }
    w.end_array();
  };
  rows("thumb_packs", pack_rows);
  rows("preview_packs", preview_rows);
  w.key("model_spec").string(o.model_spec);
  rows("model_files", model_rows);
  w.key("roots").begin_array();
  for (const root_map& r : o.roots) {
    w.begin_object();
    w.key("id").string(r.uuid);
    w.key("share").string(r.share);
    w.key("sub").string(r.sub);
    w.end_object();
  }
  w.end_array();
  w.key("files").begin_array();
  for (const std::string& name : names) add_file_row(w, o.out_dir, name);
  w.end_array();
  w.end_object();
  if (!write_text(o.out_dir + "/manifest.json", w.str())) return fail(status::io);
  for (const std::string& name : names) n.bytes += static_cast<std::uint64_t>(fs::file_size(fs_path(o.out_dir + "/" + name), ec));
  return n;
}

namespace {

std::string base_of(const std::string& destination) { return trimmed(destination) + "/.mediaviewer"; }

// The generation directories present, by number; a .part or a stranger is not one.
std::vector<std::uint64_t> generations_in(const std::string& gen_root) {
  std::vector<std::uint64_t> out;
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    std::uint64_t g = 0;
    const char* const end_of_name = name.data() + name.size();
    const auto [stop, why] = std::from_chars(name.data(), end_of_name, g);
    if (name.empty() || why != std::errc{} || stop != end_of_name) continue;  // a stranger, or too long to be ours
    out.push_back(g);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string gen_name(std::uint64_t g) {
  std::string s = std::to_string(g);
  return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

// Every generation but the newest two goes once it is old enough, and so does
// a part folder a dead run left. Failing to tidy never fails a publish.
void tidy(const deliver_options& o, const std::string& gen_root) {
  const auto gens = generations_in(gen_root);
  const auto stale = [&](const fs::path& p) {
    std::error_code ec;
    const auto when = fs::last_write_time(p, ec);
    if (ec) return false;
    const auto age = fs::file_time_type::clock::now() - when;
    return std::chrono::duration_cast<std::chrono::seconds>(age).count() >= o.keep_seconds;
  };
  std::error_code ec;
  // A reader may have started on a generation until the one after it arrived,
  // so the grace runs from then, not from when this one was written.
  for (std::size_t i = 0; i + 2 < gens.size(); ++i) {
    if (stale(fs_path(gen_root + "/" + gen_name(gens[i + 1])))) {
      fs::remove_all(fs_path(gen_root + "/" + gen_name(gens[i])), ec);
    }
  }
  for (fs::directory_iterator it(fs_path(gen_root), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    if (name.size() > 5 && name.ends_with(".part") && name != gen_name(o.generation) + ".part" && stale(it->path())) {
      std::error_code e2;
      fs::remove_all(it->path(), e2);
    }
  }
}

}  // namespace

result<std::uint64_t> published_generation(const std::string& destination, const std::string& library_id,
                                           std::span<const std::uint8_t> public_key) {
  std::error_code ec;
  if (!fs::is_directory(fs_path(destination), ec)) return err(status::io);
  const std::string base = base_of(destination);
  if (fs::exists(fs_path(base + "/library.json"), ec)) {
    // Only a file that reads and names someone else is another library's.
    const auto lib = json::parse(slurp(base + "/library.json"), 4);
    const std::string* id = lib ? lib->str("library_id") : nullptr;
    if (!id) return err(status::io);
    const std::string* key = lib->str("public_key");
    if (*id != library_id || !key || *key != to_hex(public_key)) return err(status::permission_denied);
  }
  if (!fs::exists(fs_path(base + "/current.json"), ec)) return std::uint64_t{0};
  const auto cur = json::parse(slurp(base + "/current.json"), 4);
  if (!cur) return std::uint64_t{0};
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, cur->integer("generation").value_or(0)));
}

expected deliver(const deliver_options& o, const control& c) {
  if (o.secret_key.size() != kSecretKeyBytes || o.public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  MV_TRY(const std::uint64_t there, published_generation(o.destination, o.library_id, o.public_key));
  if (there >= o.generation) return err(status::invalid_arg);  // numbers only go up

  const std::string base = base_of(o.destination);
  const std::string gen_root = base + "/gen";
  const std::string name = gen_name(o.generation);
  const std::string part = gen_root + "/" + name + ".part";
  const std::string done = gen_root + "/" + name;
  MV_TRY_VOID(o.make_directories(gen_root));
  std::error_code ec;
  fs::remove_all(fs_path(part), ec);
  ec.clear();
  MV_TRY_VOID(o.make_directories(part));

  if (!fs::exists(fs_path(base + "/library.json"), ec)) {
    json::writer w;
    w.begin_object();
    w.key("format").integer(kFormat);
    w.key("library_id").string(o.library_id);
    w.key("public_key").string(to_hex(o.public_key));
    w.end_object();
    const std::string tmp = base + "/library.json.part";
    if (!write_text(tmp, w.str())) return err(status::io);
    fs::rename(fs_path(tmp), fs_path(base + "/library.json"), ec);
    if (ec) return err(status::io);
  }

  // What the generation will name is on the share before anything names it.
  // A file that is there at its full size came from here: the name holds its
  // hash and it arrived through a verified copy.
  for (const shared_file& f : o.shared) {
    if (cancelled(c)) return err(status::cancelled);
    pause_for_viewer(c);
    const std::string to = base + "/" + f.rel;
    std::error_code e;
    if (fs::exists(fs_path(to), e)) {
      if (static_cast<std::uint64_t>(fs::file_size(fs_path(to), e)) == f.size && !e) continue;
      fs::remove(fs_path(to), e);
    }
    if (!fs::exists(fs_path(f.source), e)) return err(status::not_found);
    MV_TRY_VOID(o.make_directories(utf8_of(fs_path(to).parent_path())));
    MV_TRY_VOID(o.copy(f.source, to));
  }

  // The manifest goes last, so a part folder with one has every file.
  std::vector<std::string> names;
  for (fs::directory_iterator it(fs_path(o.gen_dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = utf8_of(it->path().filename());
    if (n != "manifest.json") names.push_back(n);
  }
  if (ec) return err(status::io);
  std::sort(names.begin(), names.end());
  names.push_back("manifest.json");
  double i = 0;
  for (const std::string& n : names) {
    if (cancelled(c)) return err(status::cancelled);
    report(c, i++ / static_cast<double>(names.size()));
    MV_TRY_VOID(o.copy(o.gen_dir + "/" + n, part + "/" + n));
  }
  const auto manifest_hash = sha256_file_hex(part + "/manifest.json");
  if (!manifest_hash) return err(status::io);

  // The two renames are the commit: first the generation exists, then it is
  // current. A folder already under this name is a run that got no further:
  // current.json names an older one (checked above), so no reader is on it.
  fs::remove_all(fs_path(done), ec);
  ec.clear();
  fs::rename(fs_path(part), fs_path(done), ec);
  if (ec) return err(status::io);
  current_info info;
  info.library_id = o.library_id;
  info.epoch = o.epoch;
  info.generation = o.generation;
  info.manifest_sha256 = *manifest_hash;
  const std::string message = signed_message(info);
  std::vector<std::uint8_t> signature(crypto_sign_ed25519_BYTES);
  if (crypto_sign_ed25519_detached(signature.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(), o.secret_key.data()) != 0) {
    return err(status::io);
  }
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(o.library_id);
  w.key("epoch").integer(static_cast<std::int64_t>(o.epoch));
  w.key("generation").integer(static_cast<std::int64_t>(o.generation));
  w.key("manifest_sha256").string(*manifest_hash);
  w.key("signature").string(to_hex(signature));
  w.end_object();
  const std::string tmp = base + "/current.json.part";
  if (!write_text(tmp, w.str())) return err(status::io);
  fs::rename(fs_path(tmp), fs_path(base + "/current.json"), ec);
  if (ec) return err(status::io);
  report(c, 1.0);
  tidy(o, gen_root);
  if (!o.retention_file.empty()) retain_shared(base, o.retention_file, o.now_unix);
  return {};
}

void retain_shared(const std::string& base, const std::string& state_file, std::int64_t now_unix,
                   std::int64_t unused_seconds) {
  // What the generations still on the share name. One that cannot be read
  // might name anything: this round deletes nothing.
  std::set<std::string> named;
  for (const std::uint64_t g : generations_in(base + "/gen")) {
    const auto m = json::parse(slurp(base + "/gen/" + gen_name(g) + "/manifest.json"), 8);
    if (!m || m->k != json::kind::object) return;
    for (const char* key : {"thumb_packs", "preview_packs", "model_files"}) {
      if (const json::value* list = m->find(key); list && list->k == json::kind::array) {
        for (const json::value& v : list->a) {
          if (const std::string* rel = v.str("path")) named.insert(*rel);
        }
      }
    }
  }
  std::map<std::string, std::int64_t> since;
  if (const auto kept = json::parse(slurp(state_file), 4); kept && kept->k == json::kind::object) {
    for (const auto& [rel, v] : kept->o) {
      if (v.k == json::kind::number) since[rel] = v.i;
    }
  }
  bool listed = true;
  std::map<std::string, std::int64_t> next;
  const fs::path root = fs_path(base);
  for (const char* dir : {"thumbs", "previews", "models"}) {
    std::error_code ec;
    if (!fs::is_directory(root / dir, ec)) continue;
    fs::recursive_directory_iterator it(root / dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code e;
      if (!it->is_regular_file(e) || e) continue;
      const std::string rel = utf8_of(fs::relative(it->path(), root, e));
      if (e || named.count(rel)) continue;
      const auto seen = since.find(rel);
      const std::int64_t first = seen == since.end() ? now_unix : seen->second;
      if (now_unix - first >= unused_seconds) {
        fs::remove(it->path(), e);
      } else {
        next[rel] = first;
      }
    }
    if (ec) listed = false;
  }
  if (!listed) return;  // a share that dropped mid-listing keeps every date it had
  json::writer w;
  w.begin_object();
  for (const auto& [rel, first] : next) w.key(rel).integer(first);
  w.end_object();
  const std::string tmp = state_file + ".tmp";
  std::error_code e2;
  if (write_text(tmp, w.str())) fs::rename(fs_path(tmp), fs_path(state_file), e2);
}

// ---- the thumbnail ledger ----------------------------------------------------------------

struct thumb_store::sqlite3_ref {
  sqlite3* db = nullptr;
  ~sqlite3_ref() { sqlite3_close_v2(db); }
};

thumb_store::~thumb_store() = default;

result<std::unique_ptr<thumb_store>> thumb_store::open(const std::string& dir) {
  std::error_code ec;
  fs::create_directories(fs_path(dir), ec);
  if (ec) return err(status::io);
  std::unique_ptr<thumb_store> t(new thumb_store());
  t->dir_ = dir;
  t->ledger_ = dir + "/ledger.db";
  t->db_ = std::make_unique<sqlite3_ref>();
  if (sqlite3_open_v2(t->ledger_.c_str(), &t->db_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3* db = t->db_->db;
  sqlite3_busy_timeout(db, 5000);
  // A tile not yet in a pack lives here (`jpeg`), so a kill loses at most the
  // transaction under way. `pack` set: the bytes are in that pack at `offset`.
  // `length` 0 with no pack: the file has no picture we can make.
  if (!exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                "CREATE TABLE IF NOT EXISTS thumbs(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, size INTEGER NOT NULL,"
                " mtime INTEGER NOT NULL, pack TEXT, offset INTEGER NOT NULL DEFAULT 0, length INTEGER NOT NULL DEFAULT 0,"
                " jpeg BLOB, tiny BLOB);"
                "CREATE INDEX IF NOT EXISTS thumbs_pack ON thumbs(pack);"
                "CREATE TABLE IF NOT EXISTS packs(name TEXT PRIMARY KEY, size INTEGER NOT NULL, sha256 TEXT NOT NULL,"
                " delivered INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE IF NOT EXISTS hashes(path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
                " sha256 TEXT NOT NULL);")) {
    return err(status::corrupt);
  }
  // A kill between writing a pack and recording it leaves a stranger.
  std::set<std::string> known;
  {
    stmt q(db, "SELECT name FROM packs");
    while (q.next()) known.insert(q.text(0));
  }
  for (fs::directory_iterator it(fs_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8_of(it->path().filename());
    if (name == "seal.tmp" || (name.starts_with("t-") && name.ends_with(".pack") && !known.count(name))) {
      std::error_code e;
      fs::remove(it->path(), e);
    }
  }
  return t;
}

result<std::vector<thumb_want>> thumb_store::missing(const std::string& index_db, std::span<const std::int64_t> roots) {
  sqlite3* db = db_->db;
  {
    stmt a(db, "ATTACH ?1 AS src");
    if (!a.bind(1, index_db).run()) return err(status::io);
  }
  std::vector<thumb_want> out;
  bool ok = exec(db, "DELETE FROM thumbs WHERE path NOT IN (SELECT path FROM src.assets)");
  if (ok) {
    std::string in;
    for (const std::int64_t r : roots) in += (in.empty() ? "" : ",") + std::to_string(r);
    // Read whole and released: no read stays open across hours of decoding.
    stmt q(db, "SELECT a.path, a.size, a.mtime, a.kind, a.duration_ms FROM src.assets a"
               " LEFT JOIN thumbs l ON l.path = a.path AND l.size = a.size AND l.mtime = a.mtime"
               " WHERE l.id IS NULL AND a.cloud = 0 AND a.root_id IN (" + in + ") ORDER BY a.mtime DESC, a.id DESC");
    while (q.next()) {
      thumb_want w;
      w.path = q.text(0);
      w.size = static_cast<std::uint64_t>(q.i64(1));
      w.mtime = q.i64(2);
      w.kind = static_cast<int>(q.i64(3));
      w.duration_ms = q.i64(4);
      out.push_back(std::move(w));
    }
    ok = !q.failed;
  }
  (void)exec(db, "DETACH src");
  if (!ok) return err(status::io);
  return out;
}

result<thumb_fill_result> thumb_store::fill(std::span<const thumb_want> wants, const thumb_fill_io& io, const control& c,
                                            double budget_seconds) {
  static constexpr std::size_t kCommitEvery = 16;
  static constexpr std::uint64_t kPackCap = 64u << 20;
  static constexpr int kTransientLimit = 16;  // the share has gone: the next run carries on
  sqlite3* db = db_->db;
  thumb_fill_result out;
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t tail = 0;
  {
    stmt q(db, "SELECT COALESCE(SUM(length), 0) FROM thumbs WHERE pack IS NULL");
    if (!q.next()) return err(status::io);
    tail = static_cast<std::uint64_t>(q.i64(0));
  }
  stmt ins(db, "INSERT OR REPLACE INTO thumbs(path, size, mtime, pack, offset, length, jpeg, tiny) VALUES(?1, ?2, ?3, NULL, 0, ?4, ?5, ?6)");
  if (!exec(db, "BEGIN")) return err(status::io);
  std::size_t in_batch = 0;
  std::size_t i = 0;
  int transient = 0;
  bool any_transient = false;
  bool stopped = false;
  for (; i < wants.size(); ++i) {
    if (io.yield) io.yield();
    if (cancelled(c)) {
      stopped = true;
      break;
    }
    if (budget_seconds > 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= budget_seconds) {
      stopped = true;
      break;
    }
    const thumb_want& w = wants[i];
    auto jpeg = io.jpeg512(w);
    if (!jpeg && jpeg.error() == status::cancelled) {
      stopped = true;
      break;
    }
    ins.reset();
    ins.bind(1, w.path).bind(2, static_cast<std::int64_t>(w.size)).bind(3, w.mtime);
    if (jpeg && !jpeg->empty()) {
      std::vector<std::uint8_t> tiny;
      if (io.tiny) {
        if (auto t = io.tiny(*jpeg)) tiny = std::move(*t);
      }
      ins.bind(4, static_cast<std::int64_t>(jpeg->size())).bind_blob(5, *jpeg).bind_blob(6, tiny);
      tail += jpeg->size();
      ++out.made;
      transient = 0;
    } else if (!jpeg && jpeg.error() != status::corrupt && jpeg.error() != status::unsupported_format) {
      // Not the file's fault: asked again next time.
      any_transient = true;
      if (++transient >= kTransientLimit) {
        stopped = true;
        break;
      }
      continue;
    } else {
      ins.bind(4, std::int64_t{0});  // pictureless; a changed file is another row
      ++out.failed;
      transient = 0;
    }
    if (!ins.run()) {
      (void)exec(db, "ROLLBACK");
      return err(status::io);
    }
    if (++in_batch >= kCommitEvery) {
      if (!exec(db, "COMMIT") || !exec(db, "BEGIN")) return err(status::io);
      in_batch = 0;
      report(c, static_cast<double>(i + 1) / static_cast<double>(wants.size()));
      if (tail >= kPackCap) {
        if (!exec(db, "COMMIT")) return err(status::io);
        MV_TRY_VOID(seal(0));
        if (!exec(db, "BEGIN")) return err(status::io);
        tail = 0;
      }
    }
  }
  if (!exec(db, "COMMIT")) return err(status::io);
  out.complete = !stopped && !any_transient;
  return out;
}

expected thumb_store::seal(std::uint64_t fold_bytes) {
  sqlite3* db = db_->db;
  {
    stmt q(db, "SELECT 1 FROM thumbs WHERE pack IS NULL AND length > 0 LIMIT 1");
    if (!q.next()) return q.failed ? expected(err(status::io)) : expected{};
  }
  const std::string tmp = dir_ + "/seal.tmp";
  std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
  std::uint64_t at = 0;
  std::string small;
  if (fold_bytes > 0) {
    stmt q(db, "SELECT name, size FROM packs WHERE size < ?1 ORDER BY size LIMIT 1");
    q.bind(1, static_cast<std::int64_t>(fold_bytes));
    if (q.next()) {
      std::ifstream in(fs_path(dir_ + "/" + q.text(0)), std::ios::binary);
      if (in) {  // gone from here means it stays as it is; the rule is an economy
        out << in.rdbuf();
        small = q.text(0);
        at = static_cast<std::uint64_t>(q.i64(1));
      }
    }
  }
  struct place {
    std::int64_t id;
    std::uint64_t offset;
  };
  std::vector<place> rows;
  {
    stmt q(db, "SELECT id, jpeg FROM thumbs WHERE pack IS NULL AND length > 0 ORDER BY id");
    while (q.next()) {
      const auto b = q.blob(1);
      out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
      rows.push_back({q.i64(0), at});
      at += b.size();
    }
    if (q.failed) return err(status::io);
  }
  out.flush();
  if (!out) return err(status::io);
  out.close();
  const auto hash = sha256_file_hex(tmp);
  if (!hash) return err(status::io);
  const std::string name = "t-" + hash->substr(0, 32) + ".pack";
  std::error_code ec;
  fs::rename(fs_path(tmp), fs_path(dir_ + "/" + name), ec);
  if (ec) return err(status::io);
  if (!exec(db, "BEGIN")) return err(status::io);
  bool ok = true;
  {
    stmt up(db, "UPDATE thumbs SET pack = ?2, offset = ?3, jpeg = NULL WHERE id = ?1");
    for (const place& p : rows) {
      up.reset();
      ok = ok && up.bind(1, p.id).bind(2, name).bind(3, static_cast<std::int64_t>(p.offset)).run();
    }
    stmt add(db, "INSERT OR REPLACE INTO packs(name, size, sha256, delivered) VALUES(?1, ?2, ?3, 0)");
    ok = ok && add.bind(1, name).bind(2, static_cast<std::int64_t>(at)).bind(3, *hash).run();
    if (!small.empty()) {
      stmt move(db, "UPDATE thumbs SET pack = ?2 WHERE pack = ?1");
      ok = ok && move.bind(1, small).bind(2, name).run();
      stmt gone(db, "DELETE FROM packs WHERE name = ?1");
      ok = ok && gone.bind(1, small).run();
    }
  }
  if (!ok) {
    (void)exec(db, "ROLLBACK");
    return err(status::io);
  }
  if (!exec(db, "COMMIT")) return err(status::io);
  if (!small.empty()) fs::remove(fs_path(dir_ + "/" + small), ec);
  return {};
}

expected thumb_store::delivered(std::uint64_t fold_bytes) {
  sqlite3* db = db_->db;
  std::vector<std::pair<std::string, std::uint64_t>> now;
  {
    stmt q(db, "SELECT name, size FROM packs WHERE delivered = 0");
    while (q.next()) now.emplace_back(q.text(0), static_cast<std::uint64_t>(q.i64(1)));
    if (q.failed) return err(status::io);
  }
  if (!exec(db, "UPDATE packs SET delivered = 1")) return err(status::io);
  for (const auto& [name, size] : now) {
    if (size >= fold_bytes) {
      std::error_code ec;
      fs::remove(fs_path(dir_ + "/" + name), ec);
    }
  }
  return {};
}

expected thumb_store::reconcile(const std::string& destination) {
  sqlite3* db = db_->db;
  const std::string there = base_of(destination) + "/thumbs/";
  std::vector<std::string> lost;
  {
    stmt q(db, "SELECT name FROM packs");
    while (q.next()) {
      std::error_code a, b;
      const bool shared = fs::exists(fs_path(there + q.text(0)), a);
      const bool local = fs::exists(fs_path(dir_ + "/" + q.text(0)), b);
      if (!shared && !local) lost.push_back(q.text(0));
    }
    if (q.failed) return err(status::io);
  }
  for (const std::string& name : lost) {
    stmt del(db, "DELETE FROM thumbs WHERE pack = ?1");
    stmt gone(db, "DELETE FROM packs WHERE name = ?1");
    if (!del.bind(1, name).run() || !gone.bind(1, name).run()) return err(status::io);
  }
  return {};
}

result<std::string> thumb_store::hash_of(const std::string& file) {
  std::error_code e1, e2;
  const auto size = fs::file_size(fs_path(file), e1);
  const auto when = fs::last_write_time(fs_path(file), e2);
  if (e1 || e2) return err(status::io);
  const auto mtime = static_cast<std::int64_t>(when.time_since_epoch().count());
  sqlite3* db = db_->db;
  stmt q(db, "SELECT sha256 FROM hashes WHERE path = ?1 AND size = ?2 AND mtime = ?3");
  if (q.bind(1, file).bind(2, static_cast<std::int64_t>(size)).bind(3, mtime).next()) return q.text(0);
  auto hash = sha256_file_hex(file);
  if (!hash) return err(status::io);
  stmt ins(db, "INSERT OR REPLACE INTO hashes(path, size, mtime, sha256) VALUES(?1, ?2, ?3, ?4)");
  (void)ins.bind(1, file).bind(2, static_cast<std::int64_t>(size)).bind(3, mtime).bind(4, *hash).run();
  return *hash;
}

namespace {
std::uint64_t get_u(std::span<const std::uint8_t> b, std::size_t at, int bytes) {
  std::uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | b[at + static_cast<std::size_t>(i)];
  return v;
}
}  // namespace

result<std::optional<thumb_location>> find_thumb(std::span<const std::uint8_t> b, std::int64_t asset) {
  if (b.size() < 16 || !std::equal(kIdxMagic, kIdxMagic + 8, b.begin())) return err(status::corrupt);
  const std::uint64_t packs = get_u(b, 8, 4), entries = get_u(b, 12, 4);
  const std::uint64_t table = 16 + packs * kPackNameBytes;
  if (table + entries * kIdxRow != b.size()) return err(status::corrupt);
  std::uint64_t lo = 0, hi = entries;
  while (lo < hi) {
    const std::uint64_t mid = lo + (hi - lo) / 2;
    const auto id = static_cast<std::int64_t>(get_u(b, table + mid * kIdxRow, 8));
    if (id < asset) {
      lo = mid + 1;
    } else if (id > asset) {
      hi = mid;
    } else {
      const std::size_t row = table + mid * kIdxRow;
      const std::uint64_t pack = get_u(b, row + 8, 4);
      if (pack >= packs) return err(status::corrupt);
      thumb_location out;
      const char* name = reinterpret_cast<const char*>(b.data()) + 16 + pack * kPackNameBytes;
      out.pack.assign(name, strnlen(name, kPackNameBytes));
      out.length = static_cast<std::uint32_t>(get_u(b, row + 12, 4));
      out.offset = get_u(b, row + 16, 8);
      return std::optional<thumb_location>(std::move(out));
    }
  }
  return std::optional<thumb_location>();
}

result<std::optional<std::span<const std::uint8_t>>> find_tiny(std::span<const std::uint8_t> b, std::int64_t asset) {
  if (b.size() < 16 || !std::equal(kTinyMagic, kTinyMagic + 8, b.begin())) return err(status::corrupt);
  const std::uint64_t entries = get_u(b, 8, 4);
  if (16 + entries * kTinyRow > b.size()) return err(status::corrupt);
  std::uint64_t lo = 0, hi = entries;
  while (lo < hi) {
    const std::uint64_t mid = lo + (hi - lo) / 2;
    const std::size_t row = 16 + static_cast<std::size_t>(mid) * kTinyRow;
    const auto id = static_cast<std::int64_t>(get_u(b, row, 8));
    if (id < asset) {
      lo = mid + 1;
    } else if (id > asset) {
      hi = mid;
    } else {
      const std::uint64_t at = get_u(b, row + 8, 8), len = get_u(b, row + 16, 4);
      if (at > b.size() || len > b.size() - at) return err(status::corrupt);
      return std::optional<std::span<const std::uint8_t>>(b.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(len)));
    }
  }
  return std::optional<std::span<const std::uint8_t>>();
}

}  // namespace mv::ai::catalog
