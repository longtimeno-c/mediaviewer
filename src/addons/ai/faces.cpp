// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/faces.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
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
  [[nodiscard]] bool null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
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

// The mean cosine of `v` to a cluster's members: sum . v / n. Pairwise units,
// the ones same_person is quoted in. (Cosine to the normalised centroid, used
// before 2026-09-28, divides by |mean| instead of 1, which inflates a
// stranger's score by 1/sqrt(the cluster's own mean similarity): ~1.4x for a
// typical person, so a 0.30 lookalike read as 0.42 and joined.)
float mean_cosine(const std::vector<float>& sum, std::uint32_t n, std::span<const float> v) {
  if (n == 0) return 0.0f;
  double d = 0;
  for (std::size_t i = 0; i < v.size() && i < sum.size(); ++i) d += static_cast<double>(sum[i]) * v[i];
  return static_cast<float>(d / n);
}

// A second person this close to the best makes a new face ambiguous: it waits
// unassigned for the refinement, which judges it against both persons' cores.
constexpr float kAmbiguous = 0.03f;

std::atomic<std::uint64_t> g_serial{0};

float iou(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
  const float x1 = std::max(ax, bx), y1 = std::max(ay, by);
  const float x2 = std::min(ax + aw, bx + bw), y2 = std::min(ay + ah, by + bh);
  const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
  const float uni = aw * ah + bw * bh - inter;
  return uni > 0 ? inter / uni : 0.0f;
}

std::string folded(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

constexpr const char* kFaceCols =
    "id, asset_id, path, pts_ms, x, y, w, h, score, COALESCE(person_id, 0), pinned";

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
  f.pinned = s.i64(10) != 0;
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
  d->serial_ = ++g_serial;
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
      " person_id INTEGER, emb BLOB NOT NULL, pinned INTEGER NOT NULL DEFAULT 0, quality REAL,"
      " tta INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS faces_person ON faces(person_id);"
      "CREATE INDEX IF NOT EXISTS faces_asset ON faces(asset_id);"
      "CREATE TABLE IF NOT EXISTS rejected(face_id INTEGER NOT NULL, person_id INTEGER NOT NULL,"
      " PRIMARY KEY(face_id, person_id));"
      "CREATE TABLE IF NOT EXISTS no_merge(a INTEGER NOT NULL, b INTEGER NOT NULL, PRIMARY KEY(a, b));"
      "CREATE TABLE IF NOT EXISTS scanned(asset_id INTEGER NOT NULL, spec TEXT NOT NULL,"
      " PRIMARY KEY(asset_id, spec));";
  if (!d->exec(schema) || !d->migrate()) return err(status::corrupt);
  std::lock_guard lock(d->m_);
  d->load_locked();
  return d;
}

result<std::unique_ptr<faces_db>> faces_db::open_read_only(const std::string& path, float same_person,
                                                           std::uint32_t dim) {
  std::unique_ptr<faces_db> d(new faces_db());
  d->path_ = path;
  d->serial_ = ++g_serial;
  d->same_ = same_person;
  d->dim_ = dim;
  if (sqlite3_open_v2(path.c_str(), &d->db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    return err(status::unsupported_format);
  }
  sqlite3_busy_timeout(d->db_, 5000);
  std::set<std::string> cols;
  {
    stmt s(d->db_, "PRAGMA table_info(faces)");
    while (s.step_row()) cols.insert(s.text(1));
  }
  if (!cols.count("pinned") || !cols.count("quality") || !cols.count("tta")) return err(status::unsupported_format);
  std::lock_guard lock(d->m_);
  d->load_locked();
  return d;
}

// faces.db from before the refinement (2026-09-28): the columns, and the
// cover of every named person pinned, since that is the face the user saw
// when they named it.
bool faces_db::migrate() {
  std::set<std::string> cols;
  {
    stmt s(db_, "PRAGMA table_info(faces)");
    while (s.step_row()) cols.insert(s.text(1));
  }
  if (cols.count("pinned") && cols.count("quality") && cols.count("tta")) return true;
  if (!exec("BEGIN")) return false;
  bool ok = true;
  if (!cols.count("pinned")) ok = ok && exec("ALTER TABLE faces ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0");
  if (!cols.count("quality")) ok = ok && exec("ALTER TABLE faces ADD COLUMN quality REAL");
  if (!cols.count("tta")) ok = ok && exec("ALTER TABLE faces ADD COLUMN tta INTEGER NOT NULL DEFAULT 0");
  if (ok && !cols.count("pinned")) {
    std::vector<std::int64_t> named;
    {
      stmt s(db_, "SELECT id FROM people WHERE name != ''");
      while (s.step_row()) named.push_back(s.i64(0));
    }
    for (std::int64_t p : named) pin_cover_locked(p);
  }
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

void faces_db::touch_locked(std::int64_t person) {
  if (person <= 0) return;
  dirty_.insert(person);
  touched_at_[person] = ++touch_gen_;
  protos_.erase(person);
}

void faces_db::pin_cover_locked(std::int64_t person) {
  stmt s(db_, "UPDATE faces SET pinned = 1 WHERE id = (SELECT id FROM faces WHERE person_id = ?1"
              " ORDER BY pinned DESC, score * w * h DESC LIMIT 1)");
  (void)s.bind(1, person).run();
}

void faces_db::load_locked() {
  clusters_.clear();
  full_due_ = true;
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

expected faces_db::import_with(const std::function<expected(sqlite3*)>& fn) {
  std::lock_guard lock(m_);
  expected r = fn(db_);
  // Whatever landed, the clusters come from the rows again, and a refinement
  // snapshot taken before the import can no longer commit (serial).
  serial_ = ++g_serial;
  dirty_.clear();
  loose_.clear();
  protos_.clear();
  touched_at_.clear();
  load_locked();
  return r;
}

std::int64_t faces_db::assign_locked(std::span<const float> emb, const std::set<std::int64_t>& rejected) {
  std::int64_t best = 0;
  float best_score = -2.0f, second = -2.0f;
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0 || rejected.count(id)) continue;
    const float s = mean_cosine(c.sum, c.n, emb);
    if (s > best_score) {
      second = best_score;
      best = id;
      best_score = s;
    } else if (s > second) {
      second = s;
    }
  }
  if (best_score < same_) {
    best = 0;
  } else if (second > best_score - kAmbiguous) {
    return 0;  // two people fit about as well: unassigned until refined
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
  touch_locked(best);
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
    protos_.erase(person);
    dirty_.erase(person);
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
  // A re-analysis (rescan) finds the same boxes: those rows take the new
  // vector and keep their id, person and pin.
  struct old_face {
    std::int64_t id, person;
    float x, y, w, h;
    bool taken;
  };
  std::vector<old_face> old;
  {
    stmt q(db_, "SELECT id, COALESCE(person_id, 0), x, y, w, h FROM faces WHERE asset_id = ?1 AND pts_ms = ?2");
    q.bind(1, asset).bind(2, pts_ms);
    while (q.step_row()) {
      old.push_back(old_face{q.i64(0), q.i64(1), static_cast<float>(q.real(2)), static_cast<float>(q.real(3)),
                             static_cast<float>(q.real(4)), static_cast<float>(q.real(5)), false});
    }
  }
  if (!exec("BEGIN")) return err(status::io);
  stmt ins(db_, "INSERT INTO faces(asset_id, path, pts_ms, x, y, w, h, score, person_id, emb, quality, tta)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
  stmt upd(db_, "UPDATE faces SET x = ?2, y = ?3, w = ?4, h = ?5, score = ?6, emb = ?7, quality = ?8,"
                " tta = ?9 WHERE id = ?1");
  std::set<std::int64_t> refreshed;
  std::vector<std::int64_t> loose;
  for (const face_in& f : faces) {
    if (f.emb.size() != dim_) continue;
    old_face* same = nullptr;
    for (old_face& o : old) {
      if (!o.taken && iou(o.x, o.y, o.w, o.h, f.x, f.y, f.w, f.h) >= 0.5f) {
        same = &o;
        break;
      }
    }
    bool ok = true;
    if (same) {
      same->taken = true;
      upd.reset();
      upd.bind(1, same->id).bind_real(2, f.x).bind_real(3, f.y).bind_real(4, f.w).bind_real(5, f.h)
          .bind_real(6, f.score).bind_blob(7, f.emb.data(), f.emb.size() * sizeof(float));
      if (f.quality >= 0) {
        upd.bind_real(8, f.quality);
      } else {
        upd.bind_null(8);
      }
      upd.bind(9, std::int64_t{f.tta ? 1 : 0});
      ok = upd.run();
      if (same->person > 0) refreshed.insert(same->person);
    } else {
      const std::int64_t person = assign_locked(f.emb, {});
      ins.reset();
      ins.bind(1, asset).bind(2, path).bind(3, pts_ms).bind_real(4, f.x).bind_real(5, f.y)
          .bind_real(6, f.w).bind_real(7, f.h).bind_real(8, f.score)
          .bind_blob(10, f.emb.data(), f.emb.size() * sizeof(float));
      if (person > 0) {
        ins.bind(9, person);
      } else {
        ins.bind_null(9);
      }
      if (f.quality >= 0) {
        ins.bind_real(11, f.quality);
      } else {
        ins.bind_null(11);
      }
      ins.bind(12, std::int64_t{f.tta ? 1 : 0});
      ok = ins.run();
      if (ok && person == 0) loose.push_back(sqlite3_last_insert_rowid(db_));
    }
    if (!ok) {
      exec("ROLLBACK");
      load_locked();
      return err(status::io);
    }
  }
  if (!exec("COMMIT")) return err(status::io);
  loose_.insert(loose.begin(), loose.end());
  for (std::int64_t p : refreshed) {
    recompute_locked(p);
    touch_locked(p);
  }
  return {};
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
  for (std::int64_t p : touched) {
    recompute_locked(p);
    touch_locked(p);
  }
  return {};
}

std::vector<person_row> faces_db::people(std::uint32_t min_faces, const std::set<std::int64_t>* assets) {
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
  // The cover: the user's pinned face first, else the largest confident one.
  // In a folder scope the same order, over that folder's faces only, and the
  // count is theirs too; nobody there, no card.
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          " FROM faces WHERE person_id = ?1 ORDER BY pinned DESC, score * w * h DESC" +
                          (assets ? "" : " LIMIT 1");
  for (person_row& p : out) {
    stmt c(db_, sql.c_str());
    c.bind(1, p.id);
    if (!assets) {
      if (c.step_row()) p.cover = face_from(c);
      continue;
    }
    std::uint32_t here = 0;
    while (c.step_row()) {
      if (assets->count(c.i64(1)) == 0) continue;
      if (here++ == 0) p.cover = face_from(c);
    }
    p.faces = here;
  }
  if (assets) {
    out.erase(std::remove_if(out.begin(), out.end(), [](const person_row& p) { return p.faces == 0; }),
              out.end());
    std::stable_sort(out.begin(), out.end(), [](const person_row& a, const person_row& b) {
      if (a.name.empty() != b.name.empty()) return !a.name.empty();
      return a.faces > b.faces;
    });
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
  if (!s.bind(1, person).bind(2, name).run()) return err(status::io);
  // The cover is the face the user looked at when naming: an anchor.
  if (!folded(name).empty()) {
    pin_cover_locked(person);
    touch_locked(person);
  }
  return {};
}

expected faces_db::merge(std::int64_t into, std::int64_t from) {
  std::lock_guard lock(m_);
  if (into == from) return {};
  // Both covers were on screen when the user said "same person": anchors.
  pin_cover_locked(into);
  pin_cover_locked(from);
  return merge_locked(into, from);
}

result<bool> faces_db::merge_auto(std::int64_t into, std::int64_t from) {
  std::lock_guard lock(m_);
  if (into == from || !clusters_.count(into) || !clusters_.count(from)) return false;
  // The user kept them apart (a split), or said a face of one is not the other.
  stmt nm(db_, "SELECT 1 FROM no_merge WHERE a = ?1 AND b = ?2");
  if (nm.bind(1, std::min(into, from)).bind(2, std::max(into, from)).step_row()) return false;
  stmt rj(db_, "SELECT 1 FROM rejected r JOIN faces f ON f.id = r.face_id"
               " WHERE (f.person_id = ?1 AND r.person_id = ?2) OR (f.person_id = ?2 AND r.person_id = ?1)");
  if (rj.bind(1, into).bind(2, from).step_row()) return false;
  MV_TRY_VOID(merge_locked(into, from));
  return true;
}

std::vector<std::pair<std::int64_t, std::int64_t>> faces_db::merge_blocks() {
  std::lock_guard lock(m_);
  std::set<std::pair<std::int64_t, std::int64_t>> out;
  stmt nm(db_, "SELECT a, b FROM no_merge");
  while (nm.step_row()) out.insert({std::min(nm.i64(0), nm.i64(1)), std::max(nm.i64(0), nm.i64(1))});
  stmt rj(db_, "SELECT DISTINCT f.person_id, r.person_id FROM rejected r JOIN faces f ON f.id = r.face_id"
               " WHERE f.person_id IS NOT NULL AND f.person_id != r.person_id");
  while (rj.step_row()) out.insert({std::min(rj.i64(0), rj.i64(1)), std::max(rj.i64(0), rj.i64(1))});
  return {out.begin(), out.end()};
}

expected faces_db::merge_locked(std::int64_t into, std::int64_t from) {
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
  protos_.erase(from);
  dirty_.erase(from);
  touch_locked(into);
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
  stmt clear(db_, "UPDATE faces SET person_id = NULL, pinned = 0 WHERE id = ?1");
  if (!clear.bind(1, face).run()) return err(status::io);
  recompute_locked(person);
  touch_locked(person);
  std::set<std::int64_t> no;
  stmt all(db_, "SELECT person_id FROM rejected WHERE face_id = ?1");
  all.bind(1, face);
  while (all.step_row()) no.insert(all.i64(0));
  const std::int64_t next = assign_locked(emb, no);
  if (next == 0) {
    loose_.insert(face);
    return {};
  }
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
    // The user chose these faces: pinned, never moved by the refinement.
    stmt u(db_, "UPDATE faces SET person_id = ?2, pinned = 1 WHERE id = ?1");
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
  touch_locked(fresh);
  for (std::int64_t o : old) {
    recompute_locked(o);
    touch_locked(o);
  }
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

std::vector<std::pair<std::int64_t, std::string>> faces_db::names() {
  std::lock_guard lock(m_);
  std::vector<std::pair<std::int64_t, std::string>> out;
  stmt s(db_, "SELECT id, name FROM people WHERE name != ''");
  while (s.step_row()) out.emplace_back(s.i64(0), s.text(1));
  return out;
}

std::int64_t faces_db::nearest_person(std::span<const float> emb) {
  std::lock_guard lock(m_);
  std::int64_t best = 0;
  float best_score = same_;
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0) continue;
    const float s = mean_cosine(c.sum, c.n, emb);
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
      // Mean cosine across the two (average linkage): sum_a . sum_b / (n_a n_b).
      // It stays an average after a merge, so a chain of merges cannot walk.
      const float across = b->second.n == 0 ? 0.0f
                                             : mean_cosine(a->second.sum, a->second.n, b->second.sum) /
                                                   static_cast<float>(b->second.n);
      if (both_named || forbid.count({std::min(ia, ib), std::max(ia, ib)}) || across < merge_at) {
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
      protos_.erase(ib);
      dirty_.erase(ib);
      touch_locked(ia);
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

// ---- refinement ---------------------------------------------------------------------

namespace {

// Full passes: the first after open, then when a quarter of the people are
// dirty or after this many incremental ones (cached prototypes drift as the
// persons they did not rebuild gain faces).
constexpr std::uint32_t kIncrementalPerFull = 64;
constexpr std::size_t kMaxRecheckAssets = 16;

}  // namespace

bool faces_db::refine_due() {
  std::lock_guard lock(m_);
  return full_due_ || !dirty_.empty() || !loose_.empty();
}

bool faces_db::refine_full_due() {
  std::lock_guard lock(m_);
  return full_due_ || incremental_since_full_ >= kIncrementalPerFull ||
         dirty_.size() * 4 > clusters_.size();
}

refine_snapshot faces_db::refine_begin(bool full) {
  std::lock_guard lock(m_);
  refine_snapshot snap;
  snap.serial = serial_;
  snap.gen = touch_gen_;
  snap.full = full || full_due_ || incremental_since_full_ >= kIncrementalPerFull ||
              dirty_.size() * 4 > clusters_.size();
  {
    stmt s(db_, "SELECT id FROM people WHERE name != '' ORDER BY id");
    while (s.step_row()) snap.named.push_back(s.i64(0));
  }
  std::map<std::int64_t, std::vector<std::int64_t>> rejected;
  {
    stmt s(db_, "SELECT face_id, person_id FROM rejected ORDER BY face_id, person_id");
    while (s.step_row()) rejected[s.i64(0)].push_back(s.i64(1));
  }
  // Persons rebuilt: all of them, or the dirty ones and any without a cache.
  if (snap.full) {
    for (const auto& [id, c] : clusters_) snap.rebuilt.insert(id);
  } else {
    snap.rebuilt = dirty_;
    for (const auto& [id, c] : clusters_) {
      if (!protos_.count(id)) snap.rebuilt.insert(id);
    }
    for (const auto& [id, pr] : protos_) {
      if (!snap.rebuilt.count(id) && clusters_.count(id)) snap.fixed.push_back(pr);
    }
  }
  const auto take = [&](stmt& s) {
    while (s.step_row()) {
      std::vector<float> e = s.floats(6);
      if (e.size() != dim_) continue;
      refine_face f;
      f.id = s.i64(0);
      f.person = s.i64(1);
      f.pinned = s.i64(2) != 0;
      f.quality = s.null(3) ? face_quality_proxy(static_cast<float>(s.real(4)), static_cast<float>(s.real(5)),
                                                 static_cast<float>(s.real(7)))
                            : static_cast<float>(s.real(3));
      f.row = static_cast<std::uint32_t>(snap.emb.size() / dim_);
      if (auto it = rejected.find(f.id); it != rejected.end()) f.rejected = it->second;
      snap.emb.insert(snap.emb.end(), e.begin(), e.end());
      snap.faces.push_back(std::move(f));
    }
  };
  constexpr const char* kCols = "SELECT id, COALESCE(person_id, 0), pinned, quality, score, w, emb, h FROM faces";
  if (snap.full) {
    stmt s(db_, (std::string(kCols) + " ORDER BY id").c_str());
    take(s);
  } else {
    stmt s(db_, (std::string(kCols) + " WHERE person_id = ?1 ORDER BY id").c_str());
    for (std::int64_t p : snap.rebuilt) {
      s.reset();
      s.bind(1, p);
      take(s);
    }
    stmt l(db_, (std::string(kCols) + " WHERE id = ?1 AND person_id IS NULL").c_str());
    for (std::int64_t f : loose_) {
      l.reset();
      l.bind(1, f);
      take(l);
    }
  }
  return snap;
}

refine_stats faces_db::refine_commit(const refine_snapshot& snap, const refine_output& out) {
  std::lock_guard lock(m_);
  refine_stats st;
  if (snap.serial != serial_) return st;
  std::set<std::int64_t> gained_fixed, created, emptied_or_changed;
  std::map<std::uint32_t, std::int64_t> group_person;
  if (!exec("BEGIN")) return st;
  stmt cur(db_, "SELECT COALESCE(person_id, 0), pinned FROM faces WHERE id = ?1");
  stmt rej(db_, "SELECT 1 FROM rejected WHERE face_id = ?1 AND person_id = ?2");
  stmt set(db_, "UPDATE faces SET person_id = ?2 WHERE id = ?1");
  stmt clear(db_, "UPDATE faces SET person_id = NULL WHERE id = ?1");
  stmt ins(db_, "INSERT INTO people(name) VALUES('')");
  bool ok = true;
  for (const refine_move& m : out.moves) {
    cur.reset();
    if (!cur.bind(1, m.face).step_row() || cur.i64(1) != 0 || cur.i64(0) != m.from) {
      ++st.skipped;  // gone, pinned or moved since the snapshot: the user wins
      continue;
    }
    std::int64_t to = m.to;
    if (to < 0) {
      const auto g = static_cast<std::uint32_t>(-to);
      if (auto it = group_person.find(g); it != group_person.end()) {
        to = it->second;
      } else {
        ins.reset();
        if (!ins.run()) {
          ok = false;
          break;
        }
        to = sqlite3_last_insert_rowid(db_);
        group_person[g] = to;
        created.insert(to);
        ++st.groups;
      }
    } else if (to > 0) {
      rej.reset();
      if (!clusters_.count(to) || rej.bind(1, m.face).bind(2, to).step_row()) {
        ++st.skipped;
        continue;
      }
    }
    if (to > 0) {
      set.reset();
      ok = set.bind(1, m.face).bind(2, to).run();
    } else {
      clear.reset();
      ok = clear.bind(1, m.face).run();
    }
    if (!ok) break;
    if (m.from > 0) emptied_or_changed.insert(m.from);
    if (to > 0) {
      emptied_or_changed.insert(to);
      if (!snap.rebuilt.count(to) && !created.count(to)) gained_fixed.insert(to);
    }
    switch (m.why) {
      case refine_why::evict: ++st.evicted; break;
      case refine_why::move: ++st.moved; break;
      case refine_why::admit: ++st.admitted; break;
      case refine_why::regroup: ++st.regrouped; break;
    }
  }
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return refine_stats{};
  }
  for (std::int64_t p : emptied_or_changed) recompute_locked(p);
  // Prototypes of the rebuilt persons are current unless something touched
  // them after the snapshot; persons that only gained faces are rebuilt next.
  for (const person_proto& pr : out.protos) {
    const auto t = touched_at_.find(pr.person);
    if (!clusters_.count(pr.person) || (t != touched_at_.end() && t->second > snap.gen)) continue;
    protos_[pr.person] = pr;
    dirty_.erase(pr.person);
  }
  for (std::int64_t p : snap.rebuilt) {
    const auto t = touched_at_.find(p);
    if (!clusters_.count(p) && (t == touched_at_.end() || t->second <= snap.gen)) {
      dirty_.erase(p);
      protos_.erase(p);
    }
  }
  for (std::int64_t p : gained_fixed) touch_locked(p);
  for (std::int64_t p : created) touch_locked(p);
  for (const refine_face& f : snap.faces) {
    if (f.person == 0) loose_.erase(f.id);
  }
  if (snap.full) {
    full_due_ = false;
    incremental_since_full_ = 0;
  } else {
    ++incremental_since_full_;
  }
  // Borderline faces of stills whose vector predates flip averaging: worth
  // one re-analysis each (once per session per asset).
  stmt q(db_, "SELECT asset_id, pts_ms, tta FROM faces WHERE id = ?1");
  for (std::int64_t id : out.recheck) {
    if (st.recheck_assets.size() >= kMaxRecheckAssets) break;
    q.reset();
    if (!q.bind(1, id).step_row() || q.i64(1) != -1 || q.i64(2) != 0) continue;
    const std::int64_t asset = q.i64(0);
    if (rechecked_.insert(asset).second) st.recheck_assets.push_back(asset);
  }
  return st;
}

expected faces_db::rescan(std::int64_t asset, const std::string& spec) {
  std::lock_guard lock(m_);
  stmt s(db_, "DELETE FROM scanned WHERE asset_id = ?1 AND spec = ?2");
  return s.bind(1, asset).bind(2, spec).run() ? expected{} : err(status::io);
}

}  // namespace mv::ai
