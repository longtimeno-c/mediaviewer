// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/faces.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace mv::ai {
namespace {

class stmt {
 public:
  stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
  ~stmt() { sqlite3_finalize(s_); }
  stmt(const stmt&) = delete;
  stmt& operator=(const stmt&) = delete;
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
  [[nodiscard]] std::vector<float> floats(int col) const {
    const void* p = sqlite3_column_blob(s_, col);
    const int n = sqlite3_column_bytes(s_, col);
    std::vector<float> v(static_cast<std::size_t>(n > 0 ? n : 0) / sizeof(float));
    if (p && !v.empty()) std::memcpy(v.data(), p, v.size() * sizeof(float));
    return v;
  }

 private:
  sqlite3_stmt* s_ = nullptr;
};

float cosine_to(const std::vector<float>& sum, std::span<const float> v) {
  double d = 0, n = 0;
  for (std::size_t i = 0; i < v.size() && i < sum.size(); ++i) {
    d += static_cast<double>(sum[i]) * v[i];
    n += static_cast<double>(sum[i]) * sum[i];
  }
  return n > 0 ? static_cast<float>(d / std::sqrt(n)) : 0.0f;
}

std::string folded(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

constexpr const char* kFaceCols = "id, asset_id, path, pts_ms, x, y, w, h, score, COALESCE(person_id, 0)";

face_row face_from(const stmt& s) {
  face_row f;
  f.id = s.i64(0);
  f.asset = s.i64(1);
  f.path = s.text(2);
  f.pts_ms = s.i64(3);
  f.x = static_cast<float>(s.real(4));
  f.y = static_cast<float>(s.real(5));
  f.w = static_cast<float>(s.real(6));
  f.h = static_cast<float>(s.real(7));
  f.score = static_cast<float>(s.real(8));
  f.person = s.i64(9);
  return f;
}

}  // namespace

faces_db::~faces_db() {
  if (db_) sqlite3_close(db_);
}

bool faces_db::exec(const char* sql) {
  return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void faces_db::destroy(const std::string& path) {
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    std::error_code ec;
    const std::string p = path + suffix;
    std::filesystem::remove(std::filesystem::path(std::u8string(p.begin(), p.end())), ec);
  }
}

result<std::unique_ptr<faces_db>> faces_db::open(const std::string& path, float same_person,
                                                 std::uint32_t dim) {
  std::unique_ptr<faces_db> d(new faces_db());
  d->path_ = path;
  d->same_ = same_person;
  d->dim_ = dim;
  if (sqlite3_open_v2(path.c_str(), &d->db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return err(status::io);
  }
  sqlite3_busy_timeout(d->db_, 5000);
  const char* schema =
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA secure_delete=ON;"
      "CREATE TABLE IF NOT EXISTS people(id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT '',"
      " created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')));"
      "CREATE TABLE IF NOT EXISTS faces(id INTEGER PRIMARY KEY, asset_id INTEGER NOT NULL,"
      " path TEXT NOT NULL, pts_ms INTEGER NOT NULL, x REAL, y REAL, w REAL, h REAL, score REAL,"
      " person_id INTEGER, emb BLOB NOT NULL);"
      "CREATE INDEX IF NOT EXISTS faces_person ON faces(person_id);"
      "CREATE INDEX IF NOT EXISTS faces_asset ON faces(asset_id);"
      "CREATE TABLE IF NOT EXISTS rejected(face_id INTEGER NOT NULL, person_id INTEGER NOT NULL,"
      " PRIMARY KEY(face_id, person_id));"
      "CREATE TABLE IF NOT EXISTS no_merge(a INTEGER NOT NULL, b INTEGER NOT NULL, PRIMARY KEY(a, b));"
      "CREATE TABLE IF NOT EXISTS scanned(asset_id INTEGER NOT NULL, spec TEXT NOT NULL,"
      " PRIMARY KEY(asset_id, spec));";
  if (!d->exec(schema)) return err(status::corrupt);
  std::lock_guard lock(d->m_);
  d->load_locked();
  return d;
}

void faces_db::load_locked() {
  clusters_.clear();
  stmt s(db_, "SELECT person_id, emb FROM faces WHERE person_id IS NOT NULL");
  while (s.step_row()) {
    const std::int64_t p = s.i64(0);
    const std::vector<float> e = s.floats(1);
    cluster& c = clusters_[p];
    if (c.sum.empty()) c.sum.assign(e.size(), 0.0f);
    if (c.sum.size() != e.size()) continue;
    for (std::size_t i = 0; i < e.size(); ++i) c.sum[i] += e[i];
    ++c.n;
  }
}

std::int64_t faces_db::assign_locked(std::span<const float> emb, const std::set<std::int64_t>& rejected) {
  std::int64_t best = 0;
  float best_score = same_;
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0 || rejected.count(id)) continue;
    const float s = cosine_to(c.sum, emb);
    if (s >= best_score) {
      best = id;
      best_score = s;
    }
  }
  if (best == 0) {
    stmt ins(db_, "INSERT INTO people(name) VALUES('')");
    if (!ins.run()) return 0;
    best = sqlite3_last_insert_rowid(db_);
  }
  cluster& c = clusters_[best];
  if (c.sum.empty()) c.sum.assign(emb.size(), 0.0f);
  for (std::size_t i = 0; i < emb.size() && i < c.sum.size(); ++i) c.sum[i] += emb[i];
  ++c.n;
  return best;
}

void faces_db::recompute_locked(std::int64_t person) {
  cluster c;
  stmt s(db_, "SELECT emb FROM faces WHERE person_id = ?1");
  s.bind(1, person);
  while (s.step_row()) {
    const std::vector<float> e = s.floats(0);
    if (c.sum.empty()) c.sum.assign(e.size(), 0.0f);
    if (c.sum.size() != e.size()) continue;
    for (std::size_t i = 0; i < e.size(); ++i) c.sum[i] += e[i];
    ++c.n;
  }
  if (c.n == 0) {
    clusters_.erase(person);
    stmt d(db_, "DELETE FROM people WHERE id = ?1 AND name = ''");
    (void)d.bind(1, person).run();
  } else {
    clusters_[person] = std::move(c);
  }
}

bool faces_db::scanned(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT 1 FROM scanned WHERE asset_id = ?1 AND spec = ?2");
  return s.bind(1, asset).bind(2, spec).step_row();
}

std::vector<std::int64_t> faces_db::scanned_assets(const std::string& spec) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> out;
  stmt s(db_, "SELECT asset_id FROM scanned WHERE spec = ?1");
  s.bind(1, spec);
  while (s.step_row()) out.push_back(s.i64(0));
  return out;
}

expected faces_db::add(std::int64_t asset, const std::string& path, std::int64_t pts_ms,
                       std::span<const face_in> faces) {
  std::lock_guard lock(m_);
  if (faces.empty()) return {};
  if (!exec("BEGIN")) return err(status::io);
  stmt ins(db_, "INSERT INTO faces(asset_id, path, pts_ms, x, y, w, h, score, person_id, emb)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
  for (const face_in& f : faces) {
    if (f.emb.size() != dim_) continue;
    const std::int64_t person = assign_locked(f.emb, {});
    ins.reset();
    ins.bind(1, asset).bind(2, path).bind(3, pts_ms).bind_real(4, f.x).bind_real(5, f.y)
        .bind_real(6, f.w).bind_real(7, f.h).bind_real(8, f.score).bind(9, person)
        .bind_blob(10, f.emb.data(), f.emb.size() * sizeof(float));
    if (!ins.run()) {
      exec("ROLLBACK");
      load_locked();
      return err(status::io);
    }
  }
  return exec("COMMIT") ? expected{} : err(status::io);
}

expected faces_db::mark_scanned(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt s(db_, "INSERT OR IGNORE INTO scanned(asset_id, spec) VALUES(?1, ?2)");
  return s.bind(1, asset).bind(2, spec).run() ? expected{} : err(status::io);
}

expected faces_db::forget_asset(std::int64_t asset) {
  std::lock_guard lock(m_);
  std::set<std::int64_t> touched;
  {
    stmt q(db_, "SELECT DISTINCT person_id FROM faces WHERE asset_id = ?1 AND person_id IS NOT NULL");
    q.bind(1, asset);
    while (q.step_row()) touched.insert(q.i64(0));
  }
  stmt d(db_, "DELETE FROM faces WHERE asset_id = ?1");
  stmt s(db_, "DELETE FROM scanned WHERE asset_id = ?1");
  if (!d.bind(1, asset).run() || !s.bind(1, asset).run()) return err(status::io);
  for (std::int64_t p : touched) recompute_locked(p);
  return {};
}

std::vector<person_row> faces_db::people(std::uint32_t min_faces) {
  std::lock_guard lock(m_);
  std::vector<person_row> out;
  stmt s(db_, "SELECT p.id, p.name, COUNT(f.id) FROM people p JOIN faces f ON f.person_id = p.id"
              " GROUP BY p.id HAVING COUNT(f.id) >= ?1 OR p.name != ''"
              " ORDER BY p.name = '' ASC, COUNT(f.id) DESC");
  s.bind(1, std::int64_t{min_faces});
  while (s.step_row()) {
    person_row p;
    p.id = s.i64(0);
    p.name = s.text(1);
    p.faces = static_cast<std::uint32_t>(s.i64(2));
    out.push_back(std::move(p));
  }
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          " FROM faces WHERE person_id = ?1 ORDER BY score * w * h DESC LIMIT 1";
  for (person_row& p : out) {
    stmt c(db_, sql.c_str());
    if (c.bind(1, p.id).step_row()) p.cover = face_from(c);
  }
  return out;
}

std::vector<face_row> faces_db::faces_of(std::int64_t person) {
  std::lock_guard lock(m_);
  std::vector<face_row> out;
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          " FROM faces WHERE person_id = ?1 ORDER BY score DESC";
  stmt s(db_, sql.c_str());
  s.bind(1, person);
  while (s.step_row()) out.push_back(face_from(s));
  return out;
}

result<face_row> faces_db::face(std::int64_t id) {
  std::lock_guard lock(m_);
  const std::string sql = std::string("SELECT ") + kFaceCols + " FROM faces WHERE id = ?1";
  stmt s(db_, sql.c_str());
  if (!s.bind(1, id).step_row()) return err(status::invalid_arg);
  return face_from(s);
}

expected faces_db::rename(std::int64_t person, const std::string& name) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE people SET name = ?2 WHERE id = ?1");
  return s.bind(1, person).bind(2, name).run() ? expected{} : err(status::io);
}

expected faces_db::merge(std::int64_t into, std::int64_t from) {
  std::lock_guard lock(m_);
  if (into == from) return {};
  if (!exec("BEGIN")) return err(status::io);
  stmt f(db_, "UPDATE faces SET person_id = ?1 WHERE person_id = ?2");
  stmt r(db_, "UPDATE OR IGNORE rejected SET person_id = ?1 WHERE person_id = ?2");
  stmt n(db_, "UPDATE people SET name = (SELECT CASE WHEN a.name = '' THEN b.name ELSE a.name END"
              " FROM people a, people b WHERE a.id = ?1 AND b.id = ?2) WHERE id = ?1");
  stmt d(db_, "DELETE FROM people WHERE id = ?2");
  const bool ok = f.bind(1, into).bind(2, from).run() && r.bind(1, into).bind(2, from).run() &&
                  n.bind(1, into).bind(2, from).run() && d.bind(2, from).run();
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return err(status::io);
  }
  recompute_locked(into);
  clusters_.erase(from);
  return {};
}

expected faces_db::reject(std::int64_t face) {
  std::lock_guard lock(m_);
  std::int64_t person = 0;
  std::vector<float> emb;
  {
    stmt q(db_, "SELECT COALESCE(person_id, 0), emb FROM faces WHERE id = ?1");
    if (!q.bind(1, face).step_row()) return err(status::invalid_arg);
    person = q.i64(0);
    emb = q.floats(1);
  }
  if (person == 0) return {};
  stmt r(db_, "INSERT OR IGNORE INTO rejected(face_id, person_id) VALUES(?1, ?2)");
  if (!r.bind(1, face).bind(2, person).run()) return err(status::io);
  // Off the cluster first, then re-assign against everyone it was not rejected from.
  stmt clear(db_, "UPDATE faces SET person_id = NULL WHERE id = ?1");
  if (!clear.bind(1, face).run()) return err(status::io);
  recompute_locked(person);
  std::set<std::int64_t> no;
  stmt all(db_, "SELECT person_id FROM rejected WHERE face_id = ?1");
  all.bind(1, face);
  while (all.step_row()) no.insert(all.i64(0));
  const std::int64_t next = assign_locked(emb, no);
  stmt set(db_, "UPDATE faces SET person_id = ?2 WHERE id = ?1");
  return set.bind(1, face).bind(2, next).run() ? expected{} : err(status::io);
}

result<std::int64_t> faces_db::split(std::span<const std::int64_t> faces) {
  std::lock_guard lock(m_);
  if (faces.empty()) return err(status::invalid_arg);
  std::set<std::int64_t> old;
  if (!exec("BEGIN")) return err(status::io);
  stmt ins(db_, "INSERT INTO people(name) VALUES('')");
  if (!ins.run()) {
    exec("ROLLBACK");
    return err(status::io);
  }
  const std::int64_t fresh = sqlite3_last_insert_rowid(db_);
  for (std::int64_t f : faces) {
    stmt q(db_, "SELECT COALESCE(person_id, 0) FROM faces WHERE id = ?1");
    if (q.bind(1, f).step_row() && q.i64(0) != 0) old.insert(q.i64(0));
    stmt u(db_, "UPDATE faces SET person_id = ?2 WHERE id = ?1");
    if (!u.bind(1, f).bind(2, fresh).run()) {
      exec("ROLLBACK");
      load_locked();
      return err(status::io);
    }
  }
  for (std::int64_t o : old) {
    stmt n(db_, "INSERT OR IGNORE INTO no_merge(a, b) VALUES(?1, ?2)");
    (void)n.bind(1, std::min(o, fresh)).bind(2, std::max(o, fresh)).run();
  }
  if (!exec("COMMIT")) return err(status::io);
  recompute_locked(fresh);
  for (std::int64_t o : old) recompute_locked(o);
  return fresh;
}

std::vector<std::int64_t> faces_db::people_named(const std::string& name) {
  std::lock_guard lock(m_);
  std::vector<std::int64_t> out;
  const std::string want = folded(name);
  if (want.empty()) return out;
  stmt s(db_, "SELECT id, name FROM people WHERE name != ''");
  while (s.step_row()) {
    if (folded(s.text(1)) == want) out.push_back(s.i64(0));
  }
  return out;
}

std::int64_t faces_db::nearest_person(std::span<const float> emb) {
  std::lock_guard lock(m_);
  std::int64_t best = 0;
  float best_score = same_;
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0) continue;
    const float s = cosine_to(c.sum, emb);
    if (s >= best_score) {
      best = id;
      best_score = s;
    }
  }
  return best;
}

std::size_t faces_db::consolidate(float merge_at) {
  std::lock_guard lock(m_);
  std::set<std::pair<std::int64_t, std::int64_t>> forbid;
  {
    stmt s(db_, "SELECT a, b FROM no_merge");
    while (s.step_row()) forbid.insert({s.i64(0), s.i64(1)});
  }
  std::map<std::int64_t, std::string> names;
  {
    stmt s(db_, "SELECT id, name FROM people");
    while (s.step_row()) names[s.i64(0)] = s.text(1);
  }
  std::size_t merged = 0;
  for (auto a = clusters_.begin(); a != clusters_.end(); ++a) {
    for (auto b = std::next(a); b != clusters_.end();) {
      const std::int64_t ia = a->first, ib = b->first;
      const bool both_named = !names[ia].empty() && !names[ib].empty();
      std::vector<float> nb = b->second.sum;
      double n = 0;
      for (float v : nb) n += static_cast<double>(v) * v;
      n = std::sqrt(n);
      if (n > 0) {
        for (float& v : nb) v = static_cast<float>(v / n);
      }
      if (both_named || forbid.count({std::min(ia, ib), std::max(ia, ib)}) ||
          cosine_to(a->second.sum, nb) < merge_at) {
        ++b;
        continue;
      }
      stmt f(db_, "UPDATE faces SET person_id = ?1 WHERE person_id = ?2");
      stmt nm(db_, "UPDATE people SET name = ?2 WHERE id = ?1 AND name = ''");
      stmt d(db_, "DELETE FROM people WHERE id = ?1");
      (void)f.bind(1, ia).bind(2, ib).run();
      (void)nm.bind(1, ia).bind(2, names[ib]).run();
      (void)d.bind(1, ib).run();
      for (std::size_t i = 0; i < a->second.sum.size() && i < b->second.sum.size(); ++i) {
        a->second.sum[i] += b->second.sum[i];
      }
      a->second.n += b->second.n;
      b = clusters_.erase(b);
      ++merged;
    }
  }
  return merged;
}

std::uint64_t faces_db::face_count() {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM faces");
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0;
}

std::uint32_t faces_db::person_count(std::uint32_t min_faces) {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM (SELECT p.id FROM people p JOIN faces f ON f.person_id = p.id"
              " GROUP BY p.id HAVING COUNT(f.id) >= ?1 OR p.name != '')");
  s.bind(1, std::int64_t{min_faces});
  return s.step_row() ? static_cast<std::uint32_t>(s.i64(0)) : 0;
}

}  // namespace mv::ai
