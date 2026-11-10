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

std::vector<std::int64_t> ids_of(const void* p, int bytes) {
  std::vector<std::int64_t> v(static_cast<std::size_t>(bytes > 0 ? bytes : 0) / sizeof(std::int64_t));
  if (p && !v.empty()) std::memcpy(v.data(), p, v.size() * sizeof(std::int64_t));
  return v;
}

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
  [[nodiscard]] std::vector<std::int64_t> ids(int col) const {
    return ids_of(sqlite3_column_blob(s_, col), sqlite3_column_bytes(s_, col));
  }
  stmt& bind_ids(int i, const std::vector<std::int64_t>& v) {
    return bind_blob(i, v.data(), v.size() * sizeof(std::int64_t));
  }
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

// Eight independent sums so the compiler vectorises it (face_refine.cpp's).
float dot8(const float* a, const float* b, std::size_t n) {
  float s[8] = {};
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    for (std::size_t k = 0; k < 8; ++k) s[k] += a[i + k] * b[i + k];
  }
  for (; i < n; ++i) s[0] += a[i] * b[i];
  return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
}

// The clustering rules faces.db's people were last settled under. A new
// value makes the next idle settle judge every face again (open). 3
// (2026-11-02): trusted pins, a clip's frames one sighting, one face per
// frame and person, rejections as negatives.
constexpr const char* kRules = "3";

// The embedder every row predates the `spec` column with (2026-10-03).
constexpr const char* kLegacySpec = "yunet-2023mar+sface-2021dec/pre1";

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
  face_tuning t;
  t.same_person = same_person;
  return open(path, t, dim, kLegacySpec);
}

result<std::unique_ptr<faces_db>> faces_db::open(const std::string& path, const face_tuning& tuning,
                                                 std::uint32_t dim, const std::string& spec) {
  std::unique_ptr<faces_db> d(new faces_db());
  d->path_ = path;
  d->serial_ = ++g_serial;
  d->same_ = tuning.same_person;
  d->ambiguous_ = tuning.ambiguous;
  d->rules_ = tuning.refine();
  d->dim_ = dim;
  d->spec_ = spec;
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
      " tta INTEGER NOT NULL DEFAULT 0, spec TEXT NOT NULL DEFAULT '');"
      "CREATE INDEX IF NOT EXISTS faces_person ON faces(person_id);"
      "CREATE INDEX IF NOT EXISTS faces_asset ON faces(asset_id);"
      "CREATE TABLE IF NOT EXISTS rejected(face_id INTEGER NOT NULL, person_id INTEGER NOT NULL,"
      " PRIMARY KEY(face_id, person_id));"
      "CREATE TABLE IF NOT EXISTS no_merge(a INTEGER NOT NULL, b INTEGER NOT NULL, PRIMARY KEY(a, b));"
      "CREATE TABLE IF NOT EXISTS rejected_at(asset_id INTEGER NOT NULL, pts_ms INTEGER NOT NULL, x REAL, y REAL,"
      " w REAL, h REAL, person_id INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS rejected_at_frame ON rejected_at(asset_id, pts_ms);"
      "CREATE TABLE IF NOT EXISTS merge_log(token INTEGER PRIMARY KEY, into_id INTEGER NOT NULL,"
      " from_id INTEGER NOT NULL, from_name TEXT NOT NULL, from_created INTEGER NOT NULL, into_name TEXT NOT NULL,"
      " faces BLOB, evicted BLOB, pinned BLOB, rejected BLOB, no_merge BLOB, into_no_merge BLOB);"
      "CREATE TABLE IF NOT EXISTS scanned(asset_id INTEGER NOT NULL, spec TEXT NOT NULL,"
      " PRIMARY KEY(asset_id, spec));"
      "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);";
  if (!d->exec(schema) || !d->migrate()) return err(status::corrupt);
  // People's reads without the rows (each carries a 2 KB vector): the cover
  // order, and the photos a person is in (people, person_count). After
  // migrate: it names columns an old faces.db gains there. ~0.4 s once at 36 k.
  if (!d->exec("CREATE INDEX IF NOT EXISTS faces_cover ON faces(person_id, pinned DESC,"
               " COALESCE(quality, 0) DESC, score * w * h DESC, asset_id)") ||
      !d->exec("CREATE INDEX IF NOT EXISTS faces_frame ON faces(asset_id, pts_ms, person_id)")) {
    return err(status::corrupt);
  }
  // Corrections that name what is gone: a rejection of a deleted face, or of
  // a person deleted (ids are reused: a stale row would bind a stranger), a
  // split pair of a person deleted, and last session's merge undo.
  d->exec("DELETE FROM rejected WHERE face_id NOT IN (SELECT id FROM faces)"
          " OR person_id NOT IN (SELECT id FROM people);"
          "DELETE FROM no_merge WHERE a NOT IN (SELECT id FROM people) OR b NOT IN (SELECT id FROM people);"
          "DELETE FROM rejected_at WHERE person_id NOT IN (SELECT id FROM people);"
          "DELETE FROM merge_log;");
  std::lock_guard lock(d->m_);
  // Faces from another embedder (a new model in the People piece): their
  // vectors are in another space, so nothing here compares them, and the
  // re-analysis that replaces them is a re-run (docs/design/17 "People model").
  {
    std::string last;
    {
      stmt m(d->db_, "SELECT value FROM meta WHERE key = 'spec'");
      if (m.step_row()) last = m.text(0);
    }
    if (last != spec) {
      // First open with this embedder (or with the spec column at all).
      stmt other(d->db_, "SELECT 1 FROM faces WHERE spec != ?1 LIMIT 1");
      if (other.bind(1, spec).step_row()) d->set_rerun_locked(true);
      stmt w(d->db_, "INSERT OR REPLACE INTO meta(key, value) VALUES('spec', ?1)");
      (void)w.bind(1, spec).run();
    }
    stmt r(d->db_, "SELECT value FROM meta WHERE key = 'rerun'");
    d->rerun_ = r.step_row() && r.text(0) == "1";
    // People filed under older rules (kRules) are judged again by the next
    // settle: no asset is read again, every face is checked against the
    // rules it has now (2026-10-06: unusable faces and weak-only people).
    stmt k(d->db_, "SELECT value FROM meta WHERE key = 'rules'");
    const bool current = k.step_row() && k.text(0) == kRules;
    stmt any(d->db_, "SELECT 1 FROM faces LIMIT 1");
    if (!current && any.step_row()) {
      d->resettle_ = true;
    } else if (!current) {
      d->rules_current_locked();
    }
  }
  d->load_locked();
  return d;
}

result<std::unique_ptr<faces_db>> faces_db::open_read_only(const std::string& path, float same_person,
                                                           std::uint32_t dim) {
  std::unique_ptr<faces_db> d(new faces_db());
  d->path_ = path;
  d->serial_ = ++g_serial;
  d->same_ = same_person;
  d->rules_.join = same_person;
  d->rules_.core = same_person;
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

void faces_db::rules_current_locked() {
  stmt w(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES('rules', ?1)");
  (void)w.bind(1, std::string(kRules)).run();
}

void faces_db::set_rerun_locked(bool on) {
  rerun_ = on;
  if (on) {
    exec("INSERT OR REPLACE INTO meta(key, value) VALUES('rerun', '1')");
  } else {
    exec("DELETE FROM meta WHERE key = 'rerun'");
  }
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
  if (cols.count("pinned") && cols.count("quality") && cols.count("tta") && cols.count("spec")) return true;
  if (!exec("BEGIN")) return false;
  bool ok = true;
  if (!cols.count("spec")) {
    // Which embedder made each vector (2026-10-03): the spec its asset was
    // scanned under, else the only one there was.
    ok = ok && exec("ALTER TABLE faces ADD COLUMN spec TEXT NOT NULL DEFAULT ''");
    stmt u(db_, "UPDATE faces SET spec = COALESCE((SELECT s.spec FROM scanned s WHERE s.asset_id = faces.asset_id"
                " ORDER BY s.spec LIMIT 1), ?1) WHERE spec = ''");
    ok = ok && u.bind(1, std::string(kLegacySpec)).run();
  }
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

void faces_db::pin_cover_locked(std::int64_t person, std::int64_t cover) {
  // The face the user saw: the one the chrome showed (`cover`), else the
  // grid's cover order (people). A face nobody can be told from is no anchor
  // (refine_params::unusable_quality); a weak one only when the user chose it.
  if (cover > 0) {
    stmt c(db_, "UPDATE faces SET pinned = 1 WHERE id = ?1 AND person_id = ?2 AND COALESCE(quality, 1) >= ?3");
    c.bind(1, cover).bind(2, person).bind_real(3, rules_.unusable_quality);
    if (c.run() && sqlite3_changes(db_) > 0) return;
  }
  stmt s(db_, "UPDATE faces SET pinned = 1 WHERE id = (SELECT id FROM faces WHERE person_id = ?1"
              " ORDER BY pinned DESC, COALESCE(quality, 0) DESC, score * w * h DESC LIMIT 1)"
              " AND COALESCE(quality, 1) >= ?2");
  (void)s.bind(1, person).bind_real(2, rules_.weak_quality).run();
}

void faces_db::load_locked() {
  clusters_.clear();
  exemplars_.clear();
  full_due_ = true;
  consolidate_full_ = true;
  stmt s(db_, "SELECT person_id, emb FROM faces WHERE person_id IS NOT NULL AND spec = ?1");
  s.bind(1, spec_);
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

faces_db::match faces_db::match_locked(std::span<const float> emb, const std::set<std::int64_t>& rejected,
                                        bool exemplars) {
  // Every person by the mean pairwise cosine (sum . v / n): the prefilter,
  // and the whole answer for a weak face.
  match m;
  std::vector<std::pair<float, std::int64_t>> shortlist;
  const float prefilter = same_ - 2 * rules_.margin;  // find_duplicates' "not the same person"
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0 || rejected.count(id)) continue;
    const float s = mean_cosine(c.sum, c.n, emb);
    if (s > m.best_score) {
      m.second = m.best_score;
      m.best = id;
      m.best_score = s;
    } else if (s > m.second) {
      m.second = s;
    }
    if (exemplars && s >= prefilter) shortlist.emplace_back(s, id);
  }
  // What the user rejected from a person counts against it: a face that
  // resembles those as much as the person is not filed there.
  const auto refused = [&](std::int64_t id, float score) {
    const person_proto* pr = exemplars_locked(id);
    if (!pr || pr->negatives.empty()) return false;
    const float ng = negative_support(emb, *pr, rules_.top_k);
    return ng > -1.0f && score - ng < rules_.margin;
  };
  if (!exemplars) {
    if (m.best != 0 && m.best_score >= same_ && refused(m.best, m.best_score)) m.best_score = -2.0f;
    return m;
  }
  // The shortlist, scored as the refinement scores a face: by the support of
  // each person's exemplars (pinned faces, then the good core), for a person
  // the user confirmed (a pinned face: the cover they named, the covers they
  // merged, the faces they split off). Anyone else answers by its mean: on
  // LFW, exemplars of unconfirmed people let a newcomer join a lookalike
  // (online BCubed F 0.9975 -> 0.9945, 2026-10-15). Ambiguity is judged
  // among the shortlist alone.
  const std::size_t keep = std::min<std::size_t>(shortlist.size(), std::max<std::uint32_t>(rules_.shortlist, 1));
  std::partial_sort(shortlist.begin(), shortlist.begin() + static_cast<std::ptrdiff_t>(keep), shortlist.end(),
                    [](const auto& a, const auto& b) {
                      return a.first != b.first ? a.first > b.first : a.second < b.second;
                    });
  match x;
  for (std::size_t i = 0; i < keep; ++i) {
    const auto [mean, id] = shortlist[i];
    const person_proto* pr = exemplars_locked(id);
    float s = mean;
    if (pr && pr->pinned && !pr->weak_only) {
      if (const float sup = exemplar_support(emb, *pr, 0, rules_.top_k); sup > -1.0f) s = sup;
    }
    if (refused(id, s)) continue;
    if (s > x.best_score) {
      x.second = x.best_score;
      x.best = id;
      x.best_score = s;
    } else if (s > x.second) {
      x.second = s;
    }
  }
  return x;
}

const person_proto* faces_db::exemplars_locked(std::int64_t person) {
  const auto c = clusters_.find(person);
  if (c == clusters_.end() || c->second.n == 0) return nullptr;
  const std::uint64_t n = c->second.n;
  if (const auto it = exemplars_.find(person); it != exemplars_.end()) {
    // Ordinary growth: the exemplars stand until the person is a quarter
    // larger (or smaller) than when they were built, so a scan does not
    // rebuild a big person once per face.
    const std::uint64_t was = it->second.n;
    if (n * 4 < was * 5 && n * 5 > was * 4) return &it->second.proto;
  }
  exemplar_cache& cache = exemplars_[person];
  cache.n = static_cast<std::uint32_t>(n);
  cache.proto = person_proto{};
  cache.proto.person = person;
  // The faces rejected from it (match_locked's negatives), clearest first.
  std::vector<float> negatives;
  {
    stmt s(db_, "SELECT f.emb FROM rejected r JOIN faces f ON f.id = r.face_id WHERE r.person_id = ?1"
                " AND f.spec = ?2 ORDER BY COALESCE(f.quality, 0) DESC LIMIT ?3");
    s.bind(1, person).bind(2, spec_).bind(3, std::int64_t{rules_.max_negatives});
    while (s.step_row()) {
      const std::vector<float> v = s.floats(0);
      if (v.size() == dim_) negatives.insert(negatives.end(), v.begin(), v.end());
    }
  }
  {
    // Unconfirmed: matched by its mean, so nothing to build (match_locked).
    stmt s(db_, "SELECT 1 FROM faces WHERE person_id = ?1 AND pinned = 1 AND spec = ?2 LIMIT 1");
    if (!s.bind(1, person).bind(2, spec_).step_row()) {
      // A rejection the person's own average vouches for as for one of its
      // own is no evidence against faces like it (refine_people's slip rule,
      // by the mean here).
      for (std::size_t k = 0; k * dim_ < negatives.size(); ++k) {
        const std::span<const float> v(negatives.data() + k * dim_, dim_);
        if (mean_cosine(c->second.sum, c->second.n, v) < same_ + 2 * rules_.margin) {
          cache.proto.negatives.insert(cache.proto.negatives.end(), v.begin(), v.end());
        }
      }
      return &cache.proto;
    }
  }
  // The refinement's refs (build_proto): pinned, then by quality, then id;
  // the good ones, or all when every one is weak; at most max_refs. Only
  // those vectors are read.
  struct ref {
    std::int64_t id;
    bool pinned;
    float quality;
    std::int64_t asset, pts_ms;
  };
  std::vector<ref> refs;
  {
    stmt s(db_, "SELECT id, pinned, quality, score, w, h, asset_id, pts_ms FROM faces WHERE person_id = ?1"
                " AND spec = ?2");
    s.bind(1, person).bind(2, spec_);
    while (s.step_row()) {
      const float q = s.null(2) ? face_quality_proxy(static_cast<float>(s.real(3)), static_cast<float>(s.real(4)),
                                                     static_cast<float>(s.real(5)))
                                : static_cast<float>(s.real(2));
      refs.push_back(ref{s.i64(0), s.i64(1) != 0, q, s.i64(6), s.i64(7)});
    }
  }
  std::sort(refs.begin(), refs.end(), [](const ref& a, const ref& b) {
    if (a.pinned != b.pinned) return a.pinned;
    if (a.quality != b.quality) return a.quality > b.quality;
    return a.id < b.id;
  });
  const bool any_good = std::any_of(refs.begin(), refs.end(),
                                    [&](const ref& r) { return r.pinned || r.quality >= rules_.weak_quality; });
  if (any_good) {
    refs.erase(std::remove_if(refs.begin(), refs.end(),
                              [&](const ref& r) { return !r.pinned && r.quality < rules_.weak_quality; }),
               refs.end());
  }
  if (refs.size() > rules_.max_refs) refs.resize(rules_.max_refs);
  std::vector<float> emb;
  std::vector<refine_face> faces;
  emb.reserve(refs.size() * dim_);
  {
    stmt e(db_, "SELECT emb FROM faces WHERE id = ?1");
    for (const ref& r : refs) {
      e.reset();
      if (!e.bind(1, r.id).step_row()) continue;
      const std::vector<float> v = e.floats(0);
      if (v.size() != dim_) continue;
      refine_face f;
      f.id = r.id;
      f.person = person;
      f.quality = r.quality;
      f.pinned = r.pinned;
      f.asset = r.asset;
      f.pts_ms = r.pts_ms;
      f.row = static_cast<std::uint32_t>(faces.size());
      emb.insert(emb.end(), v.begin(), v.end());
      faces.push_back(std::move(f));
    }
  }
  bool named = false;
  {
    stmt s(db_, "SELECT 1 FROM people WHERE id = ?1 AND name != ''");
    named = s.bind(1, person).step_row();
  }
  cache.proto = build_person_proto(dim_, emb, faces, person, named, rules_);
  cache.proto.core_mean = {};  // the prefilter is clusters_' mean
  for (std::size_t k = 0; k * dim_ < negatives.size(); ++k) {
    const std::span<const float> v(negatives.data() + k * dim_, dim_);
    if (exemplar_support(v, cache.proto, 0, rules_.top_k) < same_ + 2 * rules_.margin) {
      cache.proto.negatives.insert(cache.proto.negatives.end(), v.begin(), v.end());
    }
  }
  return &cache.proto;
}

std::int64_t faces_db::assign_locked(std::span<const float> emb, float quality,
                                     const std::set<std::int64_t>& rejected, bool may_start_person) {
  // A face nobody can be told from joins no one (refine_params::unusable_quality);
  // a weak one may join a person its betters made, never start one. Unknown
  // quality (< 0, a row from before it was recorded) counts as good.
  const bool known = quality >= 0;
  if (known && quality < rules_.unusable_quality) return 0;
  const bool may_start = (!known || quality >= rules_.weak_quality);
  // A weak face keeps the average rule: the refinement never admits or moves
  // one by exemplars either.
  const match m = match_locked(emb, rejected, may_start);
  std::int64_t best = m.best;
  if (m.best_score < same_) {
    best = 0;
  } else if (m.second > m.best_score - ambiguous_) {
    return 0;  // two people fit about as well: unassigned until refined
  }
  if (best == 0) {
    if (!may_start || !may_start_person) return 0;
    stmt ins(db_, "INSERT INTO people(name) VALUES('')");
    if (!ins.run()) return 0;
    best = sqlite3_last_insert_rowid(db_);
  }
  join_locked(best, emb);
  return best;
}

void faces_db::join_locked(std::int64_t person, std::span<const float> emb) {
  cluster& c = clusters_[person];
  if (c.sum.empty()) c.sum.assign(emb.size(), 0.0f);
  for (std::size_t i = 0; i < emb.size() && i < c.sum.size(); ++i) c.sum[i] += emb[i];
  ++c.n;
  touch_locked(person);
}

std::int64_t faces_db::track_locked(std::int64_t asset, std::int64_t pts_ms, std::span<const float> emb,
                                    const std::set<std::int64_t>& no) {
  if (pts_ms < 0) return 0;
  stmt q(db_, "SELECT person_id, emb FROM faces WHERE asset_id = ?1 AND pts_ms BETWEEN ?2 AND ?3"
              " AND person_id IS NOT NULL AND COALESCE(quality, 1) >= ?4 AND spec = ?5");
  q.bind(1, asset).bind(2, pts_ms - rules_.track_ms).bind(3, pts_ms + rules_.track_ms);
  q.bind_real(4, rules_.weak_quality).bind(5, spec_);
  std::int64_t to = 0;
  while (q.step_row()) {
    const std::vector<float> v = q.floats(1);
    if (v.size() != dim_ || mean_cosine(v, 1, emb) < same_) continue;
    if (to != 0 && to != q.i64(0)) return 0;  // the clip's frames disagree
    to = q.i64(0);
  }
  if (to == 0 || no.count(to)) return 0;
  // Not against what the user rejected from it.
  if (const person_proto* pr = exemplars_locked(to); pr && negative_support(emb, *pr, rules_.top_k) >= same_) return 0;
  return to;
}

void faces_db::recompute_locked(std::int64_t person) {
  // Faces left, moved or were re-analysed: its exemplars are stale.
  exemplars_.erase(person);
  cluster c;
  stmt s(db_, "SELECT emb FROM faces WHERE person_id = ?1 AND spec = ?2");
  s.bind(1, person).bind(2, spec_);
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
    // Gone only when no face is left at all: during a re-run its faces from
    // the old embedder still show the person until they are re-analysed.
    stmt d(db_, "DELETE FROM people WHERE id = ?1 AND name = '' AND NOT EXISTS"
                " (SELECT 1 FROM faces WHERE person_id = ?1)");
    if (d.bind(1, person).run() && sqlite3_changes(db_) > 0) drop_person_locked(person);
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
  // A re-analysis (rescan, or a re-run with another embedder) finds the same
  // boxes: those rows take the new vector and keep their id, person and pin,
  // so the people the user named, merged and split carry over.
  struct old_face {
    std::int64_t id, person;
    float x, y, w, h;
    bool taken;
    std::vector<float> emb;  // this embedder's, else empty
  };
  std::vector<old_face> old;
  {
    stmt q(db_, "SELECT id, COALESCE(person_id, 0), x, y, w, h, emb, spec FROM faces WHERE asset_id = ?1"
                " AND pts_ms = ?2");
    q.bind(1, asset).bind(2, pts_ms);
    while (q.step_row()) {
      old.push_back(old_face{q.i64(0), q.i64(1), static_cast<float>(q.real(2)), static_cast<float>(q.real(3)),
                             static_cast<float>(q.real(4)), static_cast<float>(q.real(5)), false,
                             q.text(7) == spec_ ? q.floats(6) : std::vector<float>{}});
    }
  }
  // Rejections whose face was forgotten when this file changed: they bind
  // the face found again in the same box.
  struct kept_rejection {
    float x, y, w, h;
    std::int64_t person;
  };
  std::vector<kept_rejection> kept;
  {
    stmt q(db_, "SELECT x, y, w, h, person_id FROM rejected_at WHERE asset_id = ?1 AND pts_ms = ?2");
    q.bind(1, asset).bind(2, pts_ms);
    while (q.step_row()) {
      kept.push_back(kept_rejection{static_cast<float>(q.real(0)), static_cast<float>(q.real(1)),
                                    static_cast<float>(q.real(2)), static_cast<float>(q.real(3)), q.i64(4)});
    }
  }
  // Each new face: the old row in its box, if any (best IoU >= 0.5, each row once).
  std::vector<old_face*> same(faces.size(), nullptr);
  for (std::size_t k = 0; k < faces.size(); ++k) {
    const face_in& f = faces[k];
    float best = 0.5f;
    for (old_face& o : old) {
      const float u = iou(o.x, o.y, o.w, o.h, f.x, f.y, f.w, f.h);
      if (!o.taken && u >= best) {
        if (same[k]) same[k]->taken = false;
        best = u;
        same[k] = &o;
        o.taken = true;
      }
    }
  }
  // Faces of one photo or frame are different people: a person that has one
  // of them already is no candidate for another, unless the two are the same
  // face twice (a mirror: refine_people's rule). The new ones are filed
  // clearest first.
  struct framed {
    std::int64_t person;
    std::span<const float> emb;
  };
  std::vector<framed> in_frame;
  for (const old_face& o : old) {
    if (o.person > 0 && !o.taken) in_frame.push_back(framed{o.person, o.emb});
  }
  for (std::size_t k = 0; k < faces.size(); ++k) {
    if (same[k] && same[k]->person > 0) in_frame.push_back(framed{same[k]->person, faces[k].emb});
  }
  std::vector<std::size_t> order(faces.size());
  for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return faces[a].quality > faces[b].quality; });
  if (!exec("BEGIN")) return err(status::io);
  stmt ins(db_, "INSERT INTO faces(asset_id, path, pts_ms, x, y, w, h, score, person_id, emb, quality, tta, spec)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)");
  stmt upd(db_, "UPDATE faces SET x = ?2, y = ?3, w = ?4, h = ?5, score = ?6, emb = ?7, quality = ?8,"
                " tta = ?9, spec = ?10, pinned = pinned * ?11 WHERE id = ?1");
  stmt rej(db_, "INSERT OR IGNORE INTO rejected(face_id, person_id) VALUES(?1, ?2)");
  std::set<std::int64_t> refreshed;
  std::vector<std::int64_t> loose;
  bool used_kept = false;
  for (std::size_t k : order) {
    const face_in& f = faces[k];
    if (f.emb.size() != dim_) continue;
    bool ok = true;
    if (same[k]) {
      upd.reset();
      upd.bind(1, same[k]->id).bind_real(2, f.x).bind_real(3, f.y).bind_real(4, f.w).bind_real(5, f.h)
          .bind_real(6, f.score).bind_blob(7, f.emb.data(), f.emb.size() * sizeof(float));
      if (f.quality >= 0) {
        upd.bind_real(8, f.quality);
      } else {
        upd.bind_null(8);
      }
      // The same box with a face this embedder calls someone else (the file
      // changed under it): the row keeps its person until the refinement
      // judges it, but the user's pin was about another face.
      const bool other_face = same[k]->emb.size() == dim_ && mean_cosine(f.emb, 1, same[k]->emb) < same_;
      upd.bind(9, std::int64_t{f.tta ? 1 : 0}).bind(10, spec_).bind(11, std::int64_t{other_face ? 0 : 1});
      ok = upd.run();
      if (same[k]->person > 0) refreshed.insert(same[k]->person);
    } else {
      std::set<std::int64_t> no;
      for (const framed& o : in_frame) {
        if (o.emb.size() != dim_ || mean_cosine(f.emb, 1, o.emb) < twice_locked()) no.insert(o.person);
      }
      std::vector<std::int64_t> rejected;
      for (const kept_rejection& r : kept) {
        if (iou(r.x, r.y, r.w, r.h, f.x, f.y, f.w, f.h) >= 0.5f) {
          no.insert(r.person);
          rejected.push_back(r.person);
        }
      }
      // During a re-run a new face waits unassigned: the people it could join
      // are still mostly old vectors. The settle pass at the end files it.
      std::int64_t person = rerun_ ? 0 : assign_locked(f.emb, f.quality, no);
      // A weak clip frame nobody took follows its track (refine_people's rule).
      if (!rerun_ && person == 0 && f.quality >= rules_.unusable_quality && f.quality < rules_.weak_quality) {
        person = track_locked(asset, pts_ms, f.emb, no);
        if (person != 0) join_locked(person, f.emb);
      }
      ins.reset();
      ins.bind(1, asset).bind(2, path).bind(3, pts_ms).bind_real(4, f.x).bind_real(5, f.y)
          .bind_real(6, f.w).bind_real(7, f.h).bind_real(8, f.score)
          .bind_blob(10, f.emb.data(), f.emb.size() * sizeof(float));
      if (person > 0) {
        ins.bind(9, person);
        in_frame.push_back(framed{person, f.emb});
      } else {
        ins.bind_null(9);
      }
      if (f.quality >= 0) {
        ins.bind_real(11, f.quality);
      } else {
        ins.bind_null(11);
      }
      ins.bind(12, std::int64_t{f.tta ? 1 : 0}).bind(13, spec_);
      ok = ins.run();
      const std::int64_t id = sqlite3_last_insert_rowid(db_);
      if (ok && person == 0) loose.push_back(id);
      for (std::int64_t r : rejected) {
        rej.reset();
        ok = ok && rej.bind(1, id).bind(2, r).run();
        used_kept = true;
      }
    }
    if (!ok) {
      exec("ROLLBACK");
      load_locked();
      return err(status::io);
    }
  }
  if (used_kept) {
    stmt d(db_, "DELETE FROM rejected_at WHERE asset_id = ?1 AND pts_ms = ?2");
    (void)d.bind(1, asset).bind(2, pts_ms).run();
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
  if (!s.bind(1, asset).bind(2, spec).run()) return err(status::io);
  if (spec != spec_) return {};
  // The asset is analysed with this embedder: a face of another one that the
  // new pass did not find again (its box matched nothing) has no vector here.
  std::set<std::int64_t> touched;
  {
    stmt q(db_, "SELECT DISTINCT person_id FROM faces WHERE asset_id = ?1 AND spec != ?2 AND person_id IS NOT NULL");
    q.bind(1, asset).bind(2, spec_);
    while (q.step_row()) touched.insert(q.i64(0));
  }
  stmt r(db_, "DELETE FROM rejected WHERE face_id IN (SELECT id FROM faces WHERE asset_id = ?1 AND spec != ?2)");
  stmt d(db_, "DELETE FROM faces WHERE asset_id = ?1 AND spec != ?2");
  // Rejections kept for faces of this asset that were not found again.
  stmt k(db_, "DELETE FROM rejected_at WHERE asset_id = ?1");
  if (!r.bind(1, asset).bind(2, spec_).run() || !d.bind(1, asset).bind(2, spec_).run() || !k.bind(1, asset).run()) {
    return err(status::io);
  }
  for (std::int64_t p : touched) {
    recompute_locked(p);
    touch_locked(p);
  }
  return {};
}

// The faces matching `where_faces` (?1 = asset) are about to go: their
// "not this person" moves to rejected_at by box, so the face found there
// again inherits it (add), and the rows naming the old ids go (ids are
// reused). mark_scanned drops what was not found again.
void faces_db::keep_rejections_locked(const char* where_faces, std::int64_t asset) {
  const std::string sel = std::string("SELECT id FROM faces WHERE ") + where_faces;
  stmt k(db_, ("INSERT INTO rejected_at(asset_id, pts_ms, x, y, w, h, person_id) SELECT f.asset_id, f.pts_ms,"
               " f.x, f.y, f.w, f.h, r.person_id FROM rejected r JOIN faces f ON f.id = r.face_id WHERE r.face_id IN (" +
               sel + ")").c_str());
  stmt d(db_, ("DELETE FROM rejected WHERE face_id IN (" + sel + ")").c_str());
  (void)k.bind(1, asset).run();
  (void)d.bind(1, asset).run();
}

expected faces_db::forget_moments(std::int64_t asset) {
  std::lock_guard lock(m_);
  std::set<std::int64_t> touched;
  {
    stmt q(db_, "SELECT DISTINCT person_id FROM faces WHERE asset_id = ?1 AND pts_ms >= 0 AND person_id IS NOT NULL");
    q.bind(1, asset);
    while (q.step_row()) touched.insert(q.i64(0));
  }
  keep_rejections_locked("asset_id = ?1 AND pts_ms >= 0", asset);
  stmt d(db_, "DELETE FROM faces WHERE asset_id = ?1 AND pts_ms >= 0");
  stmt s(db_, "DELETE FROM scanned WHERE asset_id = ?1");
  if (!d.bind(1, asset).run() || !s.bind(1, asset).run()) return err(status::io);
  for (std::int64_t p : touched) {
    recompute_locked(p);
    touch_locked(p);
  }
  return {};
}

expected faces_db::forget_asset(std::int64_t asset) {
  std::lock_guard lock(m_);
  std::set<std::int64_t> touched;
  {
    stmt q(db_, "SELECT DISTINCT person_id FROM faces WHERE asset_id = ?1 AND person_id IS NOT NULL");
    q.bind(1, asset);
    while (q.step_row()) touched.insert(q.i64(0));
  }
  keep_rejections_locked("asset_id = ?1", asset);
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
  // Counted in photos and clips, not faces: a clip sampled every few seconds
  // gave one stranger hundreds of faces, and "297 photos" of one video.
  stmt s(db_, "SELECT p.id, p.name, COUNT(DISTINCT f.asset_id) FROM people p JOIN faces f ON f.person_id = p.id"
              " GROUP BY p.id HAVING COUNT(DISTINCT f.asset_id) >= ?1 OR p.name != ''"
              " ORDER BY p.name = '' ASC, COUNT(DISTINCT f.asset_id) DESC");
  s.bind(1, std::int64_t{min_faces});
  while (s.step_row()) {
    person_row p;
    p.id = s.i64(0);
    p.name = s.text(1);
    p.faces = static_cast<std::uint32_t>(s.i64(2));
    out.push_back(std::move(p));
  }
  // The cover: the user's pinned face first, else the clearest (quality),
  // else the largest confident one. In a folder scope the same order, over
  // that folder's faces only, and the count (photos and clips) is theirs
  // too; nobody there, no card.
  // Both from faces_cover alone; only the cover's own row is read.
  stmt order(db_, (std::string("SELECT id, asset_id FROM faces WHERE person_id = ?1"
                               " ORDER BY pinned DESC, COALESCE(quality, 0) DESC, score * w * h DESC") +
                   (assets ? "" : " LIMIT 1")).c_str());
  stmt row(db_, (std::string("SELECT ") + kFaceCols + " FROM faces WHERE id = ?1").c_str());
  const auto cover_of = [&](std::int64_t id, person_row& p) {
    row.reset();
    if (row.bind(1, id).step_row()) p.cover = face_from(row);
  };
  for (person_row& p : out) {
    order.reset();
    order.bind(1, p.id);
    if (!assets) {
      if (order.step_row()) cover_of(order.i64(0), p);
      continue;
    }
    std::set<std::int64_t> here;
    while (order.step_row()) {
      if (assets->count(order.i64(1)) == 0) continue;
      if (here.empty()) cover_of(order.i64(0), p);
      here.insert(order.i64(1));
    }
    p.faces = static_cast<std::uint32_t>(here.size());
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

std::string faces_db::people_stamp() {
  std::lock_guard lock(m_);
  std::string out;
  stmt s(db_, "SELECT (SELECT COUNT(*) FROM people WHERE name != ''),"
              " (SELECT COALESCE(group_concat(id || ':' || name), '') FROM people WHERE name != ''),"
              " (SELECT COUNT(*) FROM faces WHERE person_id IN (SELECT id FROM people WHERE name != ''))");
  if (s.step_row()) out = std::to_string(s.i64(0)) + "/" + s.text(1) + "/" + std::to_string(s.i64(2));
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

expected faces_db::rename(std::int64_t person, const std::string& name, std::int64_t cover) {
  std::lock_guard lock(m_);
  stmt s(db_, "UPDATE people SET name = ?2 WHERE id = ?1");
  if (!s.bind(1, person).bind(2, name).run()) return err(status::io);
  // The cover is the face the user looked at when naming: an anchor.
  if (!folded(name).empty()) pin_cover_locked(person, cover);
  exemplars_.erase(person);
  // Touched either way: a name taken away lets the idle merge look at it again.
  touch_locked(person);
  return {};
}

result<std::int64_t> faces_db::merge(std::int64_t into, std::int64_t from, std::int64_t cover_into,
                                     std::int64_t cover_from) {
  std::lock_guard lock(m_);
  if (into == from) return std::int64_t{0};
  return merge_logged_locked(into, from, cover_into, cover_from, true);
}

// A merge with what undo_merge needs to put it back. `pin`: the user's merge
// pins both covers; Merge duplicates' pins nothing.
result<std::int64_t> faces_db::merge_logged_locked(std::int64_t into, std::int64_t from, std::int64_t cover_into,
                                                   std::int64_t cover_from, bool pin) {
  // What undo_merge needs to put it back, read before anything changes.
  std::string from_name, into_name;
  std::int64_t from_created = 0;
  {
    stmt q(db_, "SELECT name, created_at FROM people WHERE id = ?1");
    if (!q.bind(1, from).step_row()) return err(status::invalid_arg);
    from_name = q.text(0);
    from_created = q.i64(1);
    stmt r(db_, "SELECT name FROM people WHERE id = ?1");
    if (!r.bind(1, into).step_row()) return err(status::invalid_arg);
    into_name = r.text(0);
  }
  const auto ids = [&](const char* sql, std::int64_t a) {
    std::vector<std::int64_t> out;
    stmt q(db_, sql);
    q.bind(1, a);
    while (q.step_row()) out.push_back(q.i64(0));
    return out;
  };
  const std::vector<std::int64_t> faces = ids("SELECT id FROM faces WHERE person_id = ?1", from);
  const std::vector<std::int64_t> rejected = ids("SELECT face_id FROM rejected WHERE person_id = ?1", from);
  const std::vector<std::int64_t> apart =
      ids("SELECT CASE WHEN a = ?1 THEN b ELSE a END FROM no_merge WHERE a = ?1 OR b = ?1", from);
  const std::vector<std::int64_t> into_apart =
      ids("SELECT CASE WHEN a = ?1 THEN b ELSE a END FROM no_merge WHERE a = ?1 OR b = ?1", into);
  const std::vector<std::int64_t> was_pinned = ids(
      "SELECT id FROM faces WHERE pinned = 1 AND person_id = ?1", into);
  std::vector<std::int64_t> pinned_before = was_pinned;
  for (std::int64_t f : ids("SELECT id FROM faces WHERE pinned = 1 AND person_id = ?1", from)) {
    pinned_before.push_back(f);
  }
  // Both covers were on screen when the user said "same person": anchors.
  if (pin) {
    pin_cover_locked(into, cover_into);
    pin_cover_locked(from, cover_from);
  }
  std::vector<std::int64_t> pinned_now;
  {
    std::set<std::int64_t> before(pinned_before.begin(), pinned_before.end());
    for (std::int64_t p : {into, from}) {
      for (std::int64_t f : ids("SELECT id FROM faces WHERE pinned = 1 AND person_id = ?1", p)) {
        if (!before.count(f)) pinned_now.push_back(f);
      }
    }
  }
  const std::set<std::int64_t> loose_before = loose_;
  MV_TRY_VOID(merge_locked(into, from));
  std::vector<std::int64_t> evicted;
  for (std::int64_t f : loose_) {
    if (!loose_before.count(f)) evicted.push_back(f);
  }
  stmt log(db_, "INSERT INTO merge_log(into_id, from_id, from_name, from_created, into_name, faces, evicted, pinned,"
                " rejected, no_merge, into_no_merge) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
  log.bind(1, into).bind(2, from).bind(3, from_name).bind(4, from_created).bind(5, into_name);
  log.bind_ids(6, faces).bind_ids(7, evicted).bind_ids(8, pinned_now).bind_ids(9, rejected).bind_ids(10, apart);
  log.bind_ids(11, into_apart);
  if (!log.run()) return std::int64_t{0};
  return std::int64_t{sqlite3_last_insert_rowid(db_)};
}

expected faces_db::undo_merge(std::int64_t token) {
  std::lock_guard lock(m_);
  stmt q(db_, "SELECT into_id, from_id, from_name, from_created, into_name, faces, evicted, pinned, rejected,"
              " no_merge, into_no_merge FROM merge_log WHERE token = ?1");
  if (!q.bind(1, token).step_row()) return err(status::not_found);
  const std::int64_t into = q.i64(0), from = q.i64(1);
  const std::string from_name = q.text(2), into_name = q.text(4);
  const std::int64_t from_created = q.i64(3);
  const std::vector<std::int64_t> faces = q.ids(5), evicted = q.ids(6), pinned = q.ids(7), rejected = q.ids(8),
                                  apart = q.ids(9), into_apart = q.ids(10);
  {
    stmt live(db_, "SELECT 1 FROM people WHERE id = ?1");
    if (!live.bind(1, into).step_row()) return err(status::not_found);  // merged on, or gone
    stmt taken(db_, "SELECT 1 FROM people WHERE id = ?1");
    if (taken.bind(1, from).step_row()) return err(status::busy);  // its id is someone else's now
  }
  if (!exec("BEGIN")) return err(status::io);
  bool ok = true;
  stmt mk(db_, "INSERT INTO people(id, name, created_at) VALUES(?1, ?2, ?3)");
  ok = mk.bind(1, from).bind(2, from_name).bind(3, from_created).run();
  stmt nm(db_, "UPDATE people SET name = ?2 WHERE id = ?1");
  ok = ok && nm.bind(1, into).bind(2, into_name).run();
  const std::set<std::int64_t> was_evicted(evicted.begin(), evicted.end());
  stmt back(db_, "UPDATE faces SET person_id = ?2 WHERE id = ?1 AND (person_id = ?3 OR (person_id IS NULL AND ?4))");
  for (std::int64_t f : faces) {
    back.reset();
    ok = ok && back.bind(1, f).bind(2, from).bind(3, into).bind(4, std::int64_t{was_evicted.count(f) ? 1 : 0}).run();
  }
  stmt unpin(db_, "UPDATE faces SET pinned = 0 WHERE id = ?1");
  for (std::int64_t f : pinned) {
    unpin.reset();
    ok = ok && unpin.bind(1, f).run();
  }
  stmt rj(db_, "UPDATE OR IGNORE rejected SET person_id = ?2 WHERE face_id = ?1 AND person_id = ?3");
  for (std::int64_t f : rejected) {
    rj.reset();
    ok = ok && rj.bind(1, f).bind(2, from).bind(3, into).run();
  }
  const std::set<std::int64_t> into_had(into_apart.begin(), into_apart.end());
  stmt na(db_, "INSERT OR IGNORE INTO no_merge(a, b) VALUES(?1, ?2)");
  stmt nd(db_, "DELETE FROM no_merge WHERE a = ?1 AND b = ?2");
  for (std::int64_t o : apart) {
    na.reset();
    ok = ok && na.bind(1, std::min(o, from)).bind(2, std::max(o, from)).run();
    if (!into_had.count(o)) {
      nd.reset();
      ok = ok && nd.bind(1, std::min(o, into)).bind(2, std::max(o, into)).run();
    }
  }
  stmt d(db_, "DELETE FROM merge_log WHERE token = ?1");
  ok = ok && d.bind(1, token).run();
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return err(status::io);
  }
  for (std::int64_t f : evicted) loose_.erase(f);
  recompute_locked(into);
  recompute_locked(from);
  touch_locked(into);
  touch_locked(from);
  return {};
}

result<bool> faces_db::merge_auto(std::int64_t into, std::int64_t from, std::int64_t* undo) {
  std::lock_guard lock(m_);
  if (into == from || !clusters_.count(into) || !clusters_.count(from)) return false;
  // The user kept them apart (a split), or said a face of one is not the
  // other, or the two are in one photo together.
  stmt nm(db_, "SELECT 1 FROM no_merge WHERE a = ?1 AND b = ?2");
  if (nm.bind(1, std::min(into, from)).bind(2, std::max(into, from)).step_row()) return false;
  stmt rj(db_, "SELECT 1 FROM rejected r JOIN faces f ON f.id = r.face_id"
               " WHERE (f.person_id = ?1 AND r.person_id = ?2) OR (f.person_id = ?2 AND r.person_id = ?1)");
  if (rj.bind(1, into).bind(2, from).step_row()) return false;
  if (share_frame_locked(into, from)) return false;
  MV_TRY(std::int64_t token, merge_logged_locked(into, from, 0, 0, false));
  if (undo) *undo = token;
  return true;
}

std::set<std::pair<std::int64_t, std::int64_t>> faces_db::blocks_locked() {
  std::set<std::pair<std::int64_t, std::int64_t>> out;
  stmt nm(db_, "SELECT a, b FROM no_merge");
  while (nm.step_row()) out.insert({std::min(nm.i64(0), nm.i64(1)), std::max(nm.i64(0), nm.i64(1))});
  stmt rj(db_, "SELECT DISTINCT f.person_id, r.person_id FROM rejected r JOIN faces f ON f.id = r.face_id"
               " WHERE f.person_id IS NOT NULL AND f.person_id != r.person_id");
  while (rj.step_row()) out.insert({std::min(rj.i64(0), rj.i64(1)), std::max(rj.i64(0), rj.i64(1))});
  // Two people seen in one photo or frame (faces_frame: a few rows a frame),
  // unless the two faces are the same face twice (a mirror).
  stmt fr(db_, "SELECT a.person_id, b.person_id, a.emb, b.emb FROM faces a JOIN faces b ON b.asset_id = a.asset_id"
               " AND b.pts_ms = a.pts_ms AND b.person_id > a.person_id WHERE a.person_id IS NOT NULL"
               " AND a.spec = ?1 AND b.spec = ?1");
  fr.bind(1, spec_);
  while (fr.step_row()) {
    const std::pair<std::int64_t, std::int64_t> key{fr.i64(0), fr.i64(1)};
    if (out.count(key)) continue;
    const std::vector<float> x = fr.floats(2), y = fr.floats(3);
    if (x.size() == dim_ && y.size() == dim_ && mean_cosine(x, 1, y) < twice_locked()) out.insert(key);
  }
  return out;
}

bool faces_db::share_frame_locked(std::int64_t a, std::int64_t b) {
  stmt s(db_, "SELECT x.emb, y.emb FROM faces x JOIN faces y ON y.asset_id = x.asset_id AND y.pts_ms = x.pts_ms"
              " AND y.person_id = ?2 WHERE x.person_id = ?1 AND x.spec = ?3 AND y.spec = ?3");
  s.bind(1, a).bind(2, b).bind(3, spec_);
  while (s.step_row()) {
    const std::vector<float> x = s.floats(0), y = s.floats(1);
    if (x.size() == dim_ && y.size() == dim_ && mean_cosine(x, 1, y) < twice_locked()) return true;
  }
  return false;
}

std::vector<std::pair<std::int64_t, std::int64_t>> faces_db::merge_blocks() {
  std::lock_guard lock(m_);
  const auto out = blocks_locked();
  return {out.begin(), out.end()};
}

// `from` is becoming `into`: the user's corrections that name `from` name
// `into` now. A pair that would name one person twice goes.
void faces_db::remap_corrections_locked(std::int64_t into, std::int64_t from) {
  stmt r(db_, "UPDATE OR IGNORE rejected SET person_id = ?1 WHERE person_id = ?2");
  (void)r.bind(1, into).bind(2, from).run();
  stmt k(db_, "UPDATE rejected_at SET person_id = ?1 WHERE person_id = ?2");
  (void)k.bind(1, into).bind(2, from).run();
  std::vector<std::int64_t> apart;
  {
    stmt q(db_, "SELECT CASE WHEN a = ?1 THEN b ELSE a END FROM no_merge WHERE a = ?1 OR b = ?1");
    q.bind(1, from);
    while (q.step_row()) apart.push_back(q.i64(0));
  }
  stmt n(db_, "INSERT OR IGNORE INTO no_merge(a, b) VALUES(?1, ?2)");
  for (std::int64_t o : apart) {
    if (o == into) continue;
    n.reset();
    (void)n.bind(1, std::min(o, into)).bind(2, std::max(o, into)).run();
  }
  drop_person_locked(from);
}

// A person row is gone: nothing may name its id (it can be given out again).
void faces_db::drop_person_locked(std::int64_t person) {
  stmt r(db_, "DELETE FROM rejected WHERE person_id = ?1");
  stmt k(db_, "DELETE FROM rejected_at WHERE person_id = ?1");
  stmt n(db_, "DELETE FROM no_merge WHERE a = ?1 OR b = ?1");
  (void)r.bind(1, person).run();
  (void)k.bind(1, person).run();
  (void)n.bind(1, person).run();
}

expected faces_db::merge_locked(std::int64_t into, std::int64_t from) {
  if (!exec("BEGIN")) return err(status::io);
  stmt f(db_, "UPDATE faces SET person_id = ?1 WHERE person_id = ?2");
  stmt n(db_, "UPDATE people SET name = (SELECT CASE WHEN a.name = '' THEN b.name ELSE a.name END"
              " FROM people a, people b WHERE a.id = ?1 AND b.id = ?2) WHERE id = ?1");
  stmt d(db_, "DELETE FROM people WHERE id = ?2");
  const bool ok = f.bind(1, into).bind(2, from).run() && n.bind(1, into).bind(2, from).run() &&
                  d.bind(2, from).run();
  if (ok) remap_corrections_locked(into, from);
  // A face either side was rejected from is not this person, merged or not.
  std::vector<std::int64_t> out;
  if (ok) {
    stmt q(db_, "SELECT f.id FROM faces f JOIN rejected r ON r.face_id = f.id AND r.person_id = f.person_id"
                " WHERE f.person_id = ?1");
    q.bind(1, into);
    while (q.step_row()) out.push_back(q.i64(0));
  }
  stmt e(db_, "UPDATE faces SET person_id = NULL, pinned = 0 WHERE id = ?1");
  bool evicted = true;
  for (std::int64_t id : out) {
    e.reset();
    evicted = evicted && e.bind(1, id).run();
  }
  if (!ok || !evicted || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return err(status::io);
  }
  loose_.insert(out.begin(), out.end());
  recompute_locked(into);
  clusters_.erase(from);
  exemplars_.erase(from);
  protos_.erase(from);
  dirty_.erase(from);
  touch_locked(into);
  return {};
}

result<reject_result> faces_db::reject(std::span<const std::int64_t> faces) {
  std::lock_guard lock(m_);
  reject_result out;
  struct leaving {
    std::int64_t face, person;
    std::vector<float> emb;
    float quality;
    std::int64_t asset, pts_ms;
    bool pinned = false;
  };
  std::vector<leaving> go;
  std::set<std::int64_t> seen;
  {
    stmt q(db_, "SELECT COALESCE(person_id, 0), emb, spec, quality, asset_id, pts_ms, pinned FROM faces WHERE id = ?1");
    // A clip's other frames in the same person that are the same face (the
    // track: same_person to the rejected one) go with it.
    stmt t(db_, "SELECT id, emb, quality, pts_ms FROM faces WHERE asset_id = ?1 AND person_id = ?2 AND id != ?3"
                " AND pts_ms >= 0 AND pinned = 0 AND spec = ?4");
    for (std::int64_t face : faces) {
      q.reset();
      if (!q.bind(1, face).step_row()) return err(status::invalid_arg);
      const std::int64_t person = q.i64(0);
      if (person == 0 || !seen.insert(face).second) continue;
      std::vector<float> emb;
      if (q.text(2) == spec_) emb = q.floats(1);  // another embedder's: re-run files it
      go.push_back(leaving{face, person, emb, q.null(3) ? -1.0f : static_cast<float>(q.real(3)), q.i64(4), q.i64(5),
                           q.i64(6) != 0});
      if (q.i64(5) < 0 || emb.size() != dim_) continue;
      t.reset();
      t.bind(1, q.i64(4)).bind(2, person).bind(3, face).bind(4, spec_);
      while (t.step_row()) {
        const std::int64_t id = t.i64(0);
        if (seen.count(id)) continue;
        std::vector<float> v = t.floats(1);
        if (v.size() != dim_ || mean_cosine(emb, 1, v) < same_) continue;
        seen.insert(id);
        go.push_back(leaving{id, person, std::move(v), t.null(2) ? -1.0f : static_cast<float>(t.real(2)), q.i64(4),
                             t.i64(3)});
        ++out.spread;
      }
    }
  }
  if (go.empty()) return out;
  if (!exec("BEGIN")) return err(status::io);
  stmt r(db_, "INSERT OR IGNORE INTO rejected(face_id, person_id) VALUES(?1, ?2)");
  stmt clear(db_, "UPDATE faces SET person_id = NULL, pinned = 0 WHERE id = ?1");
  bool ok = true;
  std::set<std::int64_t> left;
  for (const leaving& l : go) {
    r.reset();
    clear.reset();
    ok = ok && r.bind(1, l.face).bind(2, l.person).run() && clear.bind(1, l.face).run();
    left.insert(l.person);
  }
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return err(status::io);
  }
  // Off the clusters first, then each is filed against everyone it was not
  // rejected from: an existing person it clearly matches, never a new one
  // (a person of one face the user did not ask for), else nobody.
  for (std::int64_t p : left) {
    recompute_locked(p);
    touch_locked(p);
  }
  stmt all(db_, "SELECT person_id FROM rejected WHERE face_id = ?1");
  stmt set(db_, "UPDATE faces SET person_id = ?2 WHERE id = ?1");
  // Never into a person already in the same photo or frame (one face twice excepted).
  stmt fr(db_, "SELECT person_id, emb FROM faces WHERE asset_id = ?1 AND pts_ms = ?2 AND id != ?3"
               " AND person_id IS NOT NULL AND spec = ?4");
  for (const leaving& l : go) {
    std::int64_t next = 0;
    if (l.emb.size() == dim_) {
      std::set<std::int64_t> no;
      all.reset();
      all.bind(1, l.face);
      while (all.step_row()) no.insert(all.i64(0));
      fr.reset();
      fr.bind(1, l.asset).bind(2, l.pts_ms).bind(3, l.face).bind(4, spec_);
      while (fr.step_row()) {
        const std::vector<float> o = fr.floats(1);
        if (o.size() != dim_ || mean_cosine(l.emb, 1, o) < twice_locked()) no.insert(fr.i64(0));
      }
      next = assign_locked(l.emb, l.quality, no, false);
    }
    if (next != 0) {
      set.reset();
      if (!set.bind(1, l.face).bind(2, next).run()) return err(status::io);
      ++out.to[next];
    } else {
      loose_.insert(l.face);
      ++out.unassigned;
    }
    out.faces.push_back(reject_result::moved{l.face, l.person, next, l.pinned});
  }
  return out;
}

expected faces_db::reject(std::int64_t face) {
  MV_TRY(reject_result r, reject(std::span<const std::int64_t>(&face, 1)));
  (void)r;
  return {};
}

expected faces_db::assign(std::span<const std::int64_t> faces, std::int64_t person, bool pin, bool undo) {
  std::lock_guard lock(m_);
  if (faces.empty() || person < 0 || (person == 0 && !undo)) return err(status::invalid_arg);
  if (person > 0) {
    stmt q(db_, "SELECT 1 FROM people WHERE id = ?1");
    if (!q.bind(1, person).step_row()) return err(status::invalid_arg);
  }
  std::set<std::int64_t> touched;
  if (person > 0) touched.insert(person);
  if (!exec("BEGIN")) return err(status::io);
  stmt q(db_, "SELECT COALESCE(person_id, 0) FROM faces WHERE id = ?1");
  stmt r(db_, "INSERT OR IGNORE INTO rejected(face_id, person_id) VALUES(?1, ?2)");
  stmt w(db_, "DELETE FROM rejected WHERE face_id = ?1 AND person_id = ?2");
  stmt u(db_, "UPDATE faces SET person_id = NULLIF(?2, 0), pinned = ?3 WHERE id = ?1");
  bool ok = true;
  for (std::int64_t f : faces) {
    q.reset();
    if (!q.bind(1, f).step_row()) {
      ok = false;
      break;
    }
    const std::int64_t was = q.i64(0);
    if (was != 0 && was != person) {
      if (!undo) {
        r.reset();
        ok = ok && r.bind(1, f).bind(2, was).run();
      }
      touched.insert(was);
    }
    w.reset();
    u.reset();
    ok = ok && w.bind(1, f).bind(2, person).run() &&
         u.bind(1, f).bind(2, person).bind(3, std::int64_t{pin && person > 0 ? 1 : 0}).run();
    if (person > 0) {
      loose_.erase(f);
    } else {
      loose_.insert(f);
    }
  }
  if (!ok || !exec("COMMIT")) {
    exec("ROLLBACK");
    load_locked();
    return err(status::invalid_arg);
  }
  for (std::int64_t p : touched) {
    recompute_locked(p);
    touch_locked(p);
  }
  return {};
}

expected faces_db::set_pinned(std::span<const std::int64_t> faces, bool pinned) {
  std::lock_guard lock(m_);
  std::set<std::int64_t> touched;
  stmt q(db_, "SELECT COALESCE(person_id, 0) FROM faces WHERE id = ?1");
  stmt u(db_, "UPDATE faces SET pinned = ?2 WHERE id = ?1 AND person_id IS NOT NULL");
  for (std::int64_t f : faces) {
    q.reset();
    if (!q.bind(1, f).step_row()) return err(status::invalid_arg);
    if (q.i64(0) == 0) continue;  // only a face filed under someone is theirs
    u.reset();
    if (!u.bind(1, f).bind(2, std::int64_t{pinned ? 1 : 0}).run()) return err(status::io);
    touched.insert(q.i64(0));
  }
  for (std::int64_t p : touched) {
    exemplars_.erase(p);
    touch_locked(p);
  }
  return {};
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
    loose_.erase(f);
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
  const match m = match_locked(emb, {}, true);
  return m.best_score >= same_ ? m.best : 0;
}

// ---- reviewing a person (docs/design/17 "Reviewing a person") ------------------------

review_snapshot faces_db::review_begin(std::int64_t person) {
  std::lock_guard lock(m_);
  review_snapshot snap;
  snap.dim = dim_;
  snap.rules = rules_;
  snap.same_person = same_;
  snap.person = person;
  {
    stmt s(db_, "SELECT 1 FROM people WHERE id = ?1 AND name != ''");
    snap.named = s.bind(1, person).step_row();
  }
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          ", quality, emb, spec FROM faces WHERE person_id = ?1 ORDER BY score DESC";
  stmt s(db_, sql.c_str());
  s.bind(1, person);
  while (s.step_row()) {
    snap.rows.push_back(face_from(s));
    snap.quality.push_back(s.null(11) ? -1.0f : static_cast<float>(s.real(11)));
    if (s.text(13) != spec_) continue;
    std::vector<float> v = s.floats(12);
    if (v.size() != dim_) continue;
    const face_row& r = snap.rows.back();
    refine_face f;
    f.id = r.id;
    f.person = person;
    f.pinned = r.pinned;
    f.quality = snap.quality.back() >= 0 ? snap.quality.back() : face_quality_proxy(r.score, r.w, r.h);
    f.asset = r.asset;
    f.pts_ms = r.pts_ms;
    f.row = static_cast<std::uint32_t>(snap.faces.size());
    snap.emb.insert(snap.emb.end(), v.begin(), v.end());
    snap.faces.push_back(std::move(f));
    snap.face_row_of.push_back(snap.rows.size() - 1);
  }
  {
    stmt f(db_, "SELECT b.person_id, a.emb, b.emb FROM faces a JOIN faces b ON b.asset_id = a.asset_id"
                " AND b.pts_ms = a.pts_ms AND b.person_id != a.person_id WHERE a.person_id = ?1 AND a.spec = ?2"
                " AND b.spec = ?2");
    f.bind(1, person).bind(2, spec_);
    while (f.step_row()) {
      const std::vector<float> x = f.floats(1), y = f.floats(2);
      if (x.size() == dim_ && y.size() == dim_ && mean_cosine(x, 1, y) < twice_locked()) {
        snap.frame_people.insert(f.i64(0));
      }
    }
  }
  std::map<std::int64_t, std::string> names;
  {
    stmt n(db_, "SELECT id, name FROM people WHERE name != ''");
    while (n.step_row()) names[n.i64(0)] = n.text(1);
  }
  for (const auto& [id, c] : clusters_) {
    if (id == person || c.n == 0) continue;
    review_snapshot::other o;
    o.id = id;
    if (auto it = names.find(id); it != names.end()) o.name = it->second;
    o.sum = c.sum;
    o.n = c.n;
    if (auto it = exemplars_.find(id); it != exemplars_.end() && it->second.proto.pinned) o.proto = it->second.proto;
    snap.others.push_back(std::move(o));
  }
  return snap;
}

std::vector<face_review> faces_db::review_finish(const review_snapshot& snap) {
  const refine_params& p = snap.rules;
  const person_proto own = build_person_proto(snap.dim, snap.emb, snap.faces, snap.person, snap.named, p);
  const std::set<std::int64_t> doubtful(own.doubtful_pins.begin(), own.doubtful_pins.end());
  std::vector<face_review> out(snap.rows.size());
  for (std::size_t i = 0; i < snap.rows.size(); ++i) {
    out[i].face = snap.rows[i];
    out[i].quality = snap.quality[i];
  }
  // Rivals are people seen more than once (a person of one face is no
  // evidence for anybody: refine_people's eligibility), never one seen in a
  // frame with this one. Their averages as rows (sum / n), dotted in float.
  std::vector<std::size_t> rivals;
  std::vector<float> means;
  for (std::size_t o = 0; o < snap.others.size(); ++o) {
    const review_snapshot::other& x = snap.others[o];
    if (x.n < 2 || x.sum.size() != snap.dim || snap.frame_people.count(x.id)) continue;
    rivals.push_back(o);
    for (float v : x.sum) means.push_back(v / static_cast<float>(x.n));
  }
  std::vector<std::pair<float, std::size_t>> best;
  for (std::size_t k = 0; k < snap.faces.size(); ++k) {
    const refine_face& f = snap.faces[k];
    face_review& r = out[snap.face_row_of[k]];
    const std::span<const float> v(snap.emb.data() + std::size_t{f.row} * snap.dim, snap.dim);
    r.support = exemplar_support(v, own, f.id, p.top_k, f.asset);
    r.doubtful_pin = f.pinned && doubtful.count(f.id);
    // Rivals: the best `shortlist` by average, re-scored by exemplars where
    // the person is confirmed, as faces_db's matcher does. Never a person seen
    // in a frame with this one: they are someone else for sure.
    best.clear();
    for (std::size_t j = 0; j < rivals.size(); ++j) {
      best.emplace_back(dot8(means.data() + j * snap.dim, v.data(), snap.dim), rivals[j]);
    }
    const std::size_t keep = std::min<std::size_t>(best.size(), std::max<std::uint32_t>(p.shortlist, 1));
    std::partial_sort(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(keep), best.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t j = 0; j < keep; ++j) {
      const review_snapshot::other& o = snap.others[best[j].second];
      float sc = best[j].first;
      if (!o.proto.exemplar_ids.empty()) {
        if (const float sup = exemplar_support(v, o.proto, 0, p.top_k, f.asset); sup > -1.0f) sc = sup;
      }
      if (sc > r.rival) {
        r.rival = sc;
        r.rival_person = o.id;
        r.rival_name = o.name;
      }
    }
    if (r.rival < snap.same_person - 2 * p.margin) {
      r.rival = -1;
      r.rival_person = 0;
      r.rival_name.clear();
    }
    const bool low = r.support != -1 && r.support < snap.same_person;
    const bool contested = r.rival != -1 && r.rival >= r.support - p.margin;
    r.check = (!f.pinned && (low || contested || f.quality < p.unusable_quality)) || r.doubtful_pin;
  }
  // Doubtful first: the checks, by how far the person's support is above the
  // rival's (least first); then the rest as the sheet showed them (score).
  std::stable_sort(out.begin(), out.end(), [](const face_review& a, const face_review& b) {
    if (a.check != b.check) return a.check;
    if (!a.check) return false;
    return a.support - a.rival < b.support - b.rival;
  });
  return out;
}

std::vector<loose_face> faces_db::loose_faces(std::uint32_t offset, std::uint32_t limit) {
  std::lock_guard lock(m_);
  std::vector<loose_face> out;
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          ", quality, emb FROM faces WHERE person_id IS NULL AND spec = ?1"
                          " AND COALESCE(quality, 1) >= ?2 ORDER BY COALESCE(quality, 1) DESC, id LIMIT ?3 OFFSET ?4";
  stmt s(db_, sql.c_str());
  s.bind(1, spec_).bind_real(2, rules_.weak_quality).bind(3, std::int64_t{limit}).bind(4, std::int64_t{offset});
  stmt rj(db_, "SELECT person_id FROM rejected WHERE face_id = ?1");
  stmt fr(db_, "SELECT person_id FROM faces WHERE asset_id = ?1 AND pts_ms = ?2 AND person_id IS NOT NULL");
  while (s.step_row()) {
    loose_face l;
    l.face = face_from(s);
    l.quality = s.null(11) ? -1.0f : static_cast<float>(s.real(11));
    const std::vector<float> v = s.floats(12);
    if (v.size() == dim_) {
      std::set<std::int64_t> no;
      rj.reset();
      rj.bind(1, l.face.id);
      while (rj.step_row()) no.insert(rj.i64(0));
      fr.reset();
      fr.bind(1, l.face.asset).bind(2, l.face.pts_ms);
      while (fr.step_row()) no.insert(fr.i64(0));
      const match m = match_locked(v, no, true);
      if (m.best != 0 && m.best_score >= same_ - 2 * rules_.margin) {
        l.suggest = m.best;
        l.score = m.best_score;
      }
    }
    out.push_back(std::move(l));
  }
  return out;
}

loose_snapshot faces_db::loose_begin() {
  std::lock_guard lock(m_);
  loose_snapshot snap;
  snap.dim = dim_;
  snap.rules = rules_;
  snap.same_person = same_;
  snap.generation = touch_gen_ ^ (static_cast<std::uint64_t>(loose_.size()) << 40);
  const std::string sql = std::string("SELECT ") + kFaceCols +
                          ", quality, emb FROM faces WHERE person_id IS NULL AND spec = ?1"
                          " AND COALESCE(quality, 1) >= ?2 ORDER BY COALESCE(quality, 1) DESC, id";
  stmt s(db_, sql.c_str());
  s.bind(1, spec_).bind_real(2, rules_.weak_quality);
  stmt rj(db_, "SELECT person_id FROM rejected WHERE face_id = ?1");
  stmt fr(db_, "SELECT person_id, emb FROM faces WHERE asset_id = ?1 AND pts_ms = ?2 AND person_id IS NOT NULL"
               " AND spec = ?3");
  while (s.step_row()) {
    loose_face l;
    l.face = face_from(s);
    l.quality = s.null(11) ? -1.0f : static_cast<float>(s.real(11));
    const std::vector<float> v = s.floats(12);
    if (v.size() != dim_) continue;
    std::vector<std::int64_t> no;
    rj.reset();
    rj.bind(1, l.face.id);
    while (rj.step_row()) no.push_back(rj.i64(0));
    fr.reset();
    fr.bind(1, l.face.asset).bind(2, l.face.pts_ms).bind(3, spec_);
    while (fr.step_row()) {
      const std::vector<float> o = fr.floats(1);
      if (o.size() != dim_ || mean_cosine(v, 1, o) < twice_locked()) no.push_back(fr.i64(0));
    }
    snap.rows.push_back(std::move(l));
    snap.emb.insert(snap.emb.end(), v.begin(), v.end());
    snap.no.push_back(std::move(no));
  }
  std::set<std::int64_t> named;
  {
    stmt n(db_, "SELECT id FROM people WHERE name != ''");
    while (n.step_row()) named.insert(n.i64(0));
  }
  for (const auto& [id, c] : clusters_) {
    if (c.n == 0 || c.sum.size() != dim_) continue;
    loose_snapshot::other o;
    o.id = id;
    o.named = named.count(id) != 0;
    o.mean.resize(dim_);
    for (std::uint32_t d = 0; d < dim_; ++d) o.mean[d] = c.sum[d] / static_cast<float>(c.n);
    if (auto it = exemplars_.find(id); it != exemplars_.end()) o.proto = it->second.proto;
    snap.others.push_back(std::move(o));
  }
  return snap;
}

std::vector<loose_face> faces_db::loose_finish(const loose_snapshot& snap) {
  const refine_params& p = snap.rules;
  std::vector<loose_face> out = snap.rows;
  std::vector<std::pair<float, std::size_t>> best;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::span<const float> v(snap.emb.data() + i * snap.dim, snap.dim);
    const std::vector<std::int64_t>& no = snap.no[i];
    // The online matcher's scorer: the best `shortlist` by average, a
    // confirmed person by its exemplars, none whose rejections it resembles.
    best.clear();
    for (std::size_t o = 0; o < snap.others.size(); ++o) {
      if (std::find(no.begin(), no.end(), snap.others[o].id) != no.end()) continue;
      best.emplace_back(dot8(snap.others[o].mean.data(), v.data(), snap.dim), o);
    }
    const std::size_t keep = std::min<std::size_t>(best.size(), std::max<std::uint32_t>(p.shortlist, 1));
    std::partial_sort(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(keep), best.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t j = 0; j < keep; ++j) {
      const loose_snapshot::other& o = snap.others[best[j].second];
      float sc = best[j].first;
      if (o.proto.pinned && !o.proto.exemplar_ids.empty()) {
        if (const float sup = exemplar_support(v, o.proto, 0, p.top_k); sup > -1.0f) sc = sup;
      }
      if (!o.proto.negatives.empty()) {
        const float ng = negative_support(v, o.proto, p.top_k);
        if (ng > -1.0f && sc - ng < p.margin) continue;
      }
      if (sc > out[i].score) {
        out[i].score = sc;
        out[i].suggest = o.id;
      }
    }
    if (out[i].score < snap.same_person - 2 * p.margin) {
      out[i].score = -1;
      out[i].suggest = 0;
    }
  }
  // A suggested named person first, surest first: one click, and on the
  // owner's copy every one of the first 600 such held-out faces was right
  // (clearest first: 397 right, 40 wrong). Then other suggestions, then the
  // clearest of the rest.
  std::set<std::int64_t> named;
  for (const auto& o : snap.others) {
    if (o.named) named.insert(o.id);
  }
  const auto rank = [&](const loose_face& l) { return l.suggest == 0 ? 2 : named.count(l.suggest) ? 0 : 1; };
  std::stable_sort(out.begin(), out.end(), [&](const loose_face& a, const loose_face& b) {
    const int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return ra < 2 && a.score > b.score;
  });
  return out;
}

std::uint64_t faces_db::generation() {
  std::lock_guard lock(m_);
  return touch_gen_ ^ (static_cast<std::uint64_t>(loose_.size()) << 40);
}

std::uint64_t faces_db::loose_count() {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM faces WHERE person_id IS NULL AND spec = ?1 AND COALESCE(quality, 1) >= ?2");
  s.bind(1, spec_).bind_real(2, rules_.weak_quality);
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0;
}

suggest_snapshot faces_db::suggest_begin(std::int64_t person) {
  std::lock_guard lock(m_);
  suggest_snapshot snap;
  snap.dim = dim_;
  snap.rules = rules_;
  snap.same_person = same_;
  snap.person = person;
  {
    // The person's references, as build_proto takes them (pinned, then the
    // clearest), so the suggestions are what a refinement would admit.
    stmt s(db_, "SELECT id, pinned, COALESCE(quality, 0.5), asset_id, pts_ms, emb FROM faces WHERE person_id = ?1"
                " AND spec = ?2 ORDER BY pinned DESC, COALESCE(quality, 0.5) DESC, id LIMIT ?3");
    s.bind(1, person).bind(2, spec_).bind(3, std::int64_t{rules_.max_refs});
    while (s.step_row()) {
      std::vector<float> v = s.floats(5);
      if (v.size() != dim_) continue;
      refine_face f;
      f.id = s.i64(0);
      f.person = person;
      f.pinned = s.i64(1) != 0;
      f.quality = static_cast<float>(s.real(2));
      f.asset = s.i64(3);
      f.pts_ms = s.i64(4);
      f.row = static_cast<std::uint32_t>(snap.own.size());
      snap.own_emb.insert(snap.own_emb.end(), v.begin(), v.end());
      snap.own.push_back(std::move(f));
    }
  }
  if (snap.own.empty()) return snap;
  {
    // Unassigned usable faces, not rejected from it and not in a frame with it.
    const std::string sql = std::string("SELECT ") + kFaceCols +
                            ", emb FROM faces f WHERE person_id IS NULL AND spec = ?1 AND COALESCE(quality, 1) >= ?2"
                            " AND NOT EXISTS (SELECT 1 FROM rejected r WHERE r.face_id = f.id AND r.person_id = ?3)"
                            " AND NOT EXISTS (SELECT 1 FROM faces g WHERE g.asset_id = f.asset_id"
                            " AND g.pts_ms = f.pts_ms AND g.person_id = ?3)"
                            " ORDER BY COALESCE(quality, 1) DESC LIMIT 20000";
    stmt s(db_, sql.c_str());
    s.bind(1, spec_).bind_real(2, rules_.weak_quality).bind(3, person);
    while (s.step_row()) {
      std::vector<float> v = s.floats(11);
      if (v.size() != dim_) continue;
      snap.loose.push_back(face_from(s));
      snap.loose_emb.insert(snap.loose_emb.end(), v.begin(), v.end());
    }
  }
  std::set<std::int64_t> confirmed;
  {
    stmt s(db_, "SELECT id FROM people WHERE name != '' UNION SELECT DISTINCT person_id FROM faces"
                " WHERE pinned = 1 AND person_id IS NOT NULL");
    while (s.step_row()) confirmed.insert(s.i64(0));
  }
  // Kept apart from this person: a split, a rejection either way, a frame together.
  std::set<std::int64_t> apart;
  {
    stmt nm(db_, "SELECT CASE WHEN a = ?1 THEN b ELSE a END FROM no_merge WHERE a = ?1 OR b = ?1");
    nm.bind(1, person);
    while (nm.step_row()) apart.insert(nm.i64(0));
    stmt rj(db_, "SELECT f.person_id FROM rejected r JOIN faces f ON f.id = r.face_id WHERE r.person_id = ?1"
                 " AND f.person_id IS NOT NULL UNION SELECT r.person_id FROM rejected r JOIN faces f"
                 " ON f.id = r.face_id WHERE f.person_id = ?1");
    rj.bind(1, person);
    while (rj.step_row()) apart.insert(rj.i64(0));
    stmt fr(db_, "SELECT b.person_id, a.emb, b.emb FROM faces a JOIN faces b ON b.asset_id = a.asset_id"
                 " AND b.pts_ms = a.pts_ms AND b.person_id != a.person_id WHERE a.person_id = ?1 AND a.spec = ?2"
                 " AND b.spec = ?2");
    fr.bind(1, person).bind(2, spec_);
    while (fr.step_row()) {
      const std::vector<float> x = fr.floats(1), y = fr.floats(2);
      if (x.size() == dim_ && y.size() == dim_ && mean_cosine(x, 1, y) < twice_locked()) apart.insert(fr.i64(0));
    }
  }
  for (const auto& [id, c] : clusters_) {
    if (id == person || c.n == 0 || confirmed.count(id) || apart.count(id)) continue;
    snap.people.push_back(suggest_snapshot::unnamed{id, c.sum, c.n});
  }
  return snap;
}

person_suggestions faces_db::suggest_finish(const suggest_snapshot& snap, std::uint32_t limit) {
  person_suggestions out;
  if (snap.own.empty()) return out;
  const refine_params& p = snap.rules;
  const person_proto own = build_person_proto(snap.dim, snap.own_emb, snap.own, snap.person, true, p);
  if (own.exemplar_ids.empty()) return out;
  const std::size_t ne = own.exemplar_ids.size();
  // A suggestion is a question the user answers, so it reaches a margin
  // below what files a face by itself.
  const float ask = snap.same_person - p.margin;
  for (std::size_t i = 0; i < snap.loose.size(); ++i) {
    const std::span<const float> v(snap.loose_emb.data() + i * snap.dim, snap.dim);
    const float sup = exemplar_support(v, own, 0, p.top_k, snap.loose[i].asset);
    if (sup >= ask) out.faces.emplace_back(snap.loose[i], sup);
  }
  // A person resembles this one when its average face is, on average, like
  // the exemplars (mean pairwise cosine across the two, as consolidate).
  for (const suggest_snapshot::unnamed& u : snap.people) {
    float sum = 0;
    for (std::size_t e = 0; e < ne; ++e) {
      sum += mean_cosine(u.sum, u.n, std::span<const float>(own.exemplars.data() + e * snap.dim, snap.dim));
    }
    const float score = sum / static_cast<float>(ne);
    if (score >= ask) out.people.emplace_back(u.id, score);
  }
  const auto by_score = [](const auto& a, const auto& b) { return a.second > b.second; };
  std::stable_sort(out.faces.begin(), out.faces.end(), by_score);
  std::stable_sort(out.people.begin(), out.people.end(), by_score);
  if (out.faces.size() > limit) out.faces.resize(limit);
  if (out.people.size() > limit) out.people.resize(limit);
  return out;
}

std::size_t faces_db::consolidate(float merge_at) {
  std::lock_guard lock(m_);
  // A person the user confirmed (a name or a pinned face) never merges with
  // another confirmed one here, not even through an unnamed cluster that
  // took the first one's faces (2026-11-02: Bob, then Alice, into one); and
  // an unconfirmed cluster joins a confirmed one only when the confirmed
  // person's exemplars do not call it a stranger (the shortlist's bound,
  // same_person - 2 margins). Not more: a varied person (front-on and
  // side-on pins) sees its own fragments at ~0.3 by its exemplars, while
  // the stranger a merge glued in sat at ~0.04 (owner's library, 2026-11-02).
  std::set<std::int64_t> confirmed;
  {
    stmt s(db_, "SELECT id FROM people WHERE name != '' UNION SELECT DISTINCT person_id FROM faces"
                " WHERE pinned = 1 AND person_id IS NOT NULL");
    while (s.step_row()) confirmed.insert(s.i64(0));
  }
  // Only pairs with a person changed since the last pass: two that were not
  // are as far apart as they were then (or a split or a name keeps them so).
  // Every pair, every minute, was 0.5-0.8 s with the engine's lock held at
  // 2 k people (owner's library, 2026-10-06).
  const bool full = consolidate_full_;
  const std::uint64_t since = consolidated_gen_;
  consolidate_full_ = false;
  consolidated_gen_ = touch_gen_;
  const auto changed = [&](std::int64_t id) {
    if (full) return true;
    const auto it = touched_at_.find(id);
    return it != touched_at_.end() && it->second > since;
  };
  // Pairs the user kept apart (a split, a rejection), read once; a merge
  // carries them over to the survivor (remap_corrections_locked) and this
  // set follows. Two people seen in one frame are checked per candidate.
  std::set<std::pair<std::int64_t, std::int64_t>> forbid;
  {
    stmt s(db_, "SELECT a, b FROM no_merge");
    while (s.step_row()) forbid.insert({std::min(s.i64(0), s.i64(1)), std::max(s.i64(0), s.i64(1))});
    stmt r(db_, "SELECT DISTINCT f.person_id, r.person_id FROM rejected r JOIN faces f ON f.id = r.face_id"
                " WHERE f.person_id IS NOT NULL AND f.person_id != r.person_id");
    while (r.step_row()) forbid.insert({std::min(r.i64(0), r.i64(1)), std::max(r.i64(0), r.i64(1))});
  }
  std::size_t merged = 0;
  // How the confirmed person `c`'s exemplars see cluster `u`: the mean
  // pairwise cosine across them (exemplars x members). No exemplars (named,
  // nothing pinned): the averages alone, as before.
  const auto vouched = [&](std::int64_t c, const cluster& u) {
    const person_proto* pr = exemplars_locked(c);
    if (!pr || pr->exemplar_ids.empty() || u.n == 0) return 1.0f;
    const std::size_t ne = pr->exemplar_ids.size();
    float sum = 0;
    for (std::size_t e = 0; e < ne; ++e) {
      sum += mean_cosine(u.sum, u.n, std::span<const float>(pr->exemplars.data() + e * dim_, dim_));
    }
    return sum / static_cast<float>(ne);
  };
  const float stranger = same_ - 2 * rules_.margin;
  for (auto a = clusters_.begin(); a != clusters_.end(); ++a) {
    bool a_changed = changed(a->first);
    for (auto b = std::next(a); b != clusters_.end();) {
      const std::int64_t ia = a->first, ib = b->first;
      const bool ca = confirmed.count(ia) != 0, cb = confirmed.count(ib) != 0;
      if ((ca && cb) || (!a_changed && !changed(ib))) {
        ++b;
        continue;
      }
      // Mean cosine across the two (average linkage): sum_a . sum_b / (n_a n_b).
      // It stays an average after a merge, so a chain of merges cannot walk.
      const float across = b->second.n == 0 ? 0.0f
                                             : mean_cosine(a->second.sum, a->second.n, b->second.sum) /
                                                   static_cast<float>(b->second.n);
      if (across < merge_at || forbid.count({std::min(ia, ib), std::max(ia, ib)}) || share_frame_locked(ia, ib) ||
          (ca && vouched(ia, b->second) < stranger) || (cb && vouched(ib, a->second) < stranger)) {
        ++b;
        continue;
      }
      if (cb) confirmed.insert(ia);  // the survivor carries the user's faces (and name)
      stmt f(db_, "UPDATE faces SET person_id = ?1 WHERE person_id = ?2");
      stmt nm(db_, "UPDATE people SET name = (SELECT name FROM people WHERE id = ?2) WHERE id = ?1 AND name = ''");
      stmt d(db_, "DELETE FROM people WHERE id = ?1");
      (void)f.bind(1, ia).bind(2, ib).run();
      (void)nm.bind(1, ia).bind(2, ib).run();
      (void)d.bind(1, ib).run();
      remap_corrections_locked(ia, ib);
      std::set<std::pair<std::int64_t, std::int64_t>> next;
      for (const auto& [x, y] : forbid) {
        const std::int64_t nx = x == ib ? ia : x, ny = y == ib ? ia : y;
        if (nx != ny) next.insert({std::min(nx, ny), std::max(nx, ny)});
      }
      forbid = std::move(next);
      for (std::size_t i = 0; i < a->second.sum.size() && i < b->second.sum.size(); ++i) {
        a->second.sum[i] += b->second.sum[i];
      }
      a->second.n += b->second.n;
      exemplars_.erase(ia);
      exemplars_.erase(ib);
      protos_.erase(ib);
      dirty_.erase(ib);
      touch_locked(ia);
      a_changed = true;  // its average moved: the rest of its pairs are new
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
              " GROUP BY p.id HAVING COUNT(DISTINCT f.asset_id) >= ?1 OR p.name != '')");
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
      f.asset = s.i64(8);
      f.pts_ms = s.i64(9);
      f.row = static_cast<std::uint32_t>(snap.emb.size() / dim_);
      if (auto it = rejected.find(f.id); it != rejected.end()) f.rejected = it->second;
      snap.emb.insert(snap.emb.end(), e.begin(), e.end());
      snap.faces.push_back(std::move(f));
    }
  };
  // This embedder's faces only: another's vectors are in another space.
  constexpr const char* kCols =
      "SELECT id, COALESCE(person_id, 0), pinned, quality, score, w, emb, h, asset_id, pts_ms FROM faces";
  if (snap.full) {
    stmt s(db_, (std::string(kCols) + " WHERE spec = ?1 ORDER BY id").c_str());
    s.bind(1, spec_);
    take(s);
  } else {
    stmt s(db_, (std::string(kCols) + " WHERE person_id = ?1 AND spec = ?2 ORDER BY id").c_str());
    for (std::int64_t p : snap.rebuilt) {
      s.reset();
      s.bind(1, p).bind(2, spec_);
      take(s);
    }
    stmt l(db_, (std::string(kCols) + " WHERE id = ?1 AND person_id IS NULL AND spec = ?2").c_str());
    for (std::int64_t f : loose_) {
      l.reset();
      l.bind(1, f).bind(2, spec_);
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
    st.applied.push_back(refine_move{m.face, m.from, to, m.why, m.own, m.best});
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
    // The online matcher's exemplars too: the same rules, just rebuilt.
    if (!pr.pinned) continue;
    exemplar_cache& cache = exemplars_[pr.person];
    std::vector<float> negatives = std::move(cache.proto.negatives);
    const bool had = cache.n != 0;
    cache.proto = pr;
    cache.proto.core_mean = {};
    cache.n = clusters_[pr.person].n;
    if (had) {
      cache.proto.negatives = std::move(negatives);
    } else {
      exemplars_.erase(pr.person);  // built lazily, with its negatives
    }
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

// ---- re-run (docs/design/17 "People model") -----------------------------------------------

expected faces_db::rerun_all() {
  std::lock_guard lock(m_);
  if (!exec("DELETE FROM scanned")) return err(status::io);
  set_rerun_locked(true);
  rechecked_.clear();
  return {};
}

bool faces_db::rerun_pending() {
  std::lock_guard lock(m_);
  return rerun_;
}

bool faces_db::settle_due() {
  std::lock_guard lock(m_);
  return rerun_ || resettle_;
}

std::uint64_t faces_db::stale_count() {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM faces WHERE spec != ?1");
  s.bind(1, spec_);
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0;
}

std::uint64_t faces_db::unassigned_count() {
  std::lock_guard lock(m_);
  stmt s(db_, "SELECT COUNT(*) FROM faces WHERE person_id IS NULL AND spec = ?1");
  s.bind(1, spec_);
  return s.step_row() ? static_cast<std::uint64_t>(s.i64(0)) : 0;
}

void faces_db::rerun_done() {
  std::lock_guard lock(m_);
  // Faces of another embedder whose asset was not read this time (an offline
  // folder, Photos access withdrawn) stay as they are: they show their person
  // and are compared with nothing until their asset is analysed again, when
  // add / mark_scanned replace them like any other.
  full_due_ = true;
  set_rerun_locked(false);
  if (resettle_) rules_current_locked();
  resettle_ = false;
}

}  // namespace mv::ai
