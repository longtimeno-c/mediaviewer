// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The JPEG-512 cache's SQLite index (image/thumb.h), one TU for both hosts
// (it used to be copied into thumb.cpp and thumb_mac.cpp, which differed only
// in the path separator). Issue #168: statements are prepared once per
// connection, the lock covers one SQLite step and never a stat or a write, and
// a caller that reads the bytes treats a failed read as the miss.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include <sqlite3.h>

#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"

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
// #612: the path's rows of any other stamp or spec, which store() replaces.
// The primary key starts with `path`, so both are an index range, not a scan.
constexpr char kOthersSelect[] =
    "SELECT file FROM thumbs WHERE path=?1 AND NOT (mtime=?2 AND size=?3 AND spec=?4);";
constexpr char kOthersDelete[] =
    "DELETE FROM thumbs WHERE path=?1 AND NOT (mtime=?2 AND size=?3 AND spec=?4);";

// #612: the sweep reads rows in batches of this many, one lock per batch.
constexpr int kSweepBatch = 256;
// Deletes between two asks of the pacer.
constexpr std::size_t kDeletesPerPace = 32;
// An unnamed JPEG younger than this may be a store() between its file write
// and its row insert: left for the next sweep.
constexpr std::int64_t kOrphanGraceSeconds = 60;

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

std::int64_t now_unix() noexcept {
  return static_cast<std::int64_t>(std::time(nullptr));
}

// Deletes `ids` in one transaction. The caller holds the store's lock.
bool delete_rowids(sqlite3* db, const std::vector<std::int64_t>& ids) noexcept {
  if (ids.empty()) return true;
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM thumbs WHERE rowid=?;", -1, &del, nullptr) != SQLITE_OK) {
    return false;
  }
  bool ok = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK;
  for (std::size_t i = 0; ok && i < ids.size(); ++i) {
    sqlite3_bind_int64(del, 1, ids[i]);
    ok = sqlite3_step(del) == SQLITE_DONE;
    sqlite3_reset(del);
  }
  sqlite3_finalize(del);
  sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
  return ok;
}

}  // namespace

bool is_thumb_file_name(std::string_view name) noexcept {
  if (name.size() != 20 || name.substr(16) != ".jpg") return false;
  for (std::size_t i = 0; i < 16; ++i) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

thumb_store::~thumb_store() { close(); }

bool thumb_store::is_open() const noexcept {
  std::lock_guard lock(mutex_);
  return db_ != nullptr;
}

void thumb_store::finalize_locked() noexcept {
  sqlite3_finalize(select_);
  sqlite3_finalize(insert_);
  sqlite3_finalize(delete_);
  sqlite3_finalize(others_select_);
  sqlite3_finalize(others_delete_);
  select_ = insert_ = delete_ = others_select_ = others_delete_ = nullptr;
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  ++epoch_;
  sweep_token_.fetch_add(1, std::memory_order_acq_rel);
  total_known_.store(false, std::memory_order_relaxed);
  total_bytes_.store(0, std::memory_order_relaxed);
}

expected thumb_store::open(std::string_view dir_utf8) {
  if (dir_utf8.empty()) return err(status::invalid_arg);

  std::lock_guard lock(mutex_);
  // Already serving this directory. Reopening would close a handle that
  // lookups on other pool threads are about to use.
  if (db_ != nullptr && dir_ == dir_utf8) return {};
  finalize_locked();
  dir_.assign(dir_utf8);
  sweeps_stopped_.store(false, std::memory_order_release);
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
  sqlite3_stmt* osel = nullptr;
  sqlite3_stmt* odel = nullptr;
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kSelect, -1, SQLITE_PREPARE_PERSISTENT, &sel, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kInsert, -1, SQLITE_PREPARE_PERSISTENT, &ins, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kDelete, -1, SQLITE_PREPARE_PERSISTENT, &del, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kOthersSelect, -1, SQLITE_PREPARE_PERSISTENT, &osel, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v3(db, kOthersDelete, -1, SQLITE_PREPARE_PERSISTENT, &odel, nullptr) != SQLITE_OK) {
    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sqlite3_finalize(del);
    sqlite3_finalize(osel);
    sqlite3_finalize(odel);
    sqlite3_close(db);
    return err(status::io);
  }
  db_ = db;
  select_ = sel;
  insert_ = ins;
  delete_ = del;
  others_select_ = osel;
  others_delete_ = odel;
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

  std::vector<std::string> stale;
  std::string dir;
  {
    std::lock_guard lock(mutex_);
    if (!db_) return err(status::internal);
    bind_key(insert_, key);
    sqlite3_bind_text(insert_, 5, file.c_str(), static_cast<int>(file.size()), SQLITE_STATIC);
    const int rc = sqlite3_step(insert_);
    done_with(insert_);
    if (rc != SQLITE_DONE) return err(status::io);
    // #612: the path's other stamps (an edit, a rotate, a replaced file) and
    // old specs can never be hit again. Their rows go here, their files below
    // with the lock released.
    bind_key(others_select_, key);
    while (sqlite3_step(others_select_) == SQLITE_ROW) {
      const unsigned char* t = sqlite3_column_text(others_select_, 0);
      if (t && file != reinterpret_cast<const char*>(t)) stale.emplace_back(reinterpret_cast<const char*>(t));
    }
    done_with(others_select_);
    bind_key(others_delete_, key);
    sqlite3_step(others_delete_);
    done_with(others_delete_);
    if (!stale.empty()) dir = dir_;
  }
  total_bytes_.fetch_add(jpeg.size(), std::memory_order_relaxed);
  // Best effort: a file the chrome still has open (Windows) stays, and the
  // next sweep deletes it as an orphan.
  for (const std::string& name : stale) (void)io::remove_file(join_dir(dir, name));
  return path;
}

std::uint64_t thumb_store::total_bytes(bool* known) const noexcept {
  if (known) *known = total_known_.load(std::memory_order_relaxed);
  return total_bytes_.load(std::memory_order_relaxed);
}

void thumb_store::stop_sweep() noexcept {
  sweeps_stopped_.store(true, std::memory_order_release);
  sweep_token_.fetch_add(1, std::memory_order_acq_rel);
}

result<bool> thumb_store::sweep(thumb_sweep& st, std::uint64_t cap_bytes, const job_context& ctx,
                                const sweep_pacer& pacer) {
  using phase = thumb_sweep::phase;
  if (st.at == phase::done) return true;
  const auto stopped = [&]() noexcept {
    return ctx.cancelled() || sweep_token_.load(std::memory_order_acquire) != st.token;
  };
  // 0 go on, 1 step aside, 2 stop. Waiting sleeps: it costs no core, and no
  // view job is waiting for this worker (that would be step_aside).
  const auto pace = [&]() -> int {
    for (;;) {
      if (stopped()) return 2;
      const sweep_pace p = pacer ? pacer() : sweep_pace::go;
      if (p == sweep_pace::go) return 0;
      if (p == sweep_pace::step_aside) return 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  };
  const auto paused = [&](int p) -> result<bool> {
    if (p == 1) return false;
    return err(status::cancelled);
  };

  std::string dir;
  if (st.at == phase::start) {
    {
      std::lock_guard lock(mutex_);
      if (!db_) return err(status::invalid_arg);
      if (sweeps_stopped_.load(std::memory_order_acquire)) return err(status::cancelled);
      st.token = sweep_token_.load(std::memory_order_acquire);
      dir = dir_;
      sqlite3_stmt* max = nullptr;
      if (sqlite3_prepare_v2(db_, "SELECT IFNULL(MAX(rowid),0) FROM thumbs;", -1, &max, nullptr) != SQLITE_OK) {
        return err(status::io);
      }
      if (sqlite3_step(max) == SQLITE_ROW) st.max_rowid = sqlite3_column_int64(max, 0);
      sqlite3_finalize(max);
    }
    st.started_unix = now_unix();
    // One directory read for every file's size and time: no stat per row.
    // Every row up to max_rowid had its file written before the snapshot
    // above (store() writes the file, then the row), so its file is listed
    // here unless it has gone.
    MV_TRY(std::vector<io::dir_child> listed, io::list_children(dir));
    st.files.reserve(listed.size());
    for (io::dir_child& f : listed) {
      if (!is_thumb_file_name(f.name)) continue;
      st.files.emplace(std::move(f.name), thumb_sweep::listed{f.size, f.mtime_unix, false});
    }
    st.at = phase::rows;
  }

  while (st.at == phase::rows) {
    if (const int p = pace(); p != 0) return paused(p);
    struct row {
      std::int64_t rowid = 0;
      bool current_spec = false;
      std::string file;
    };
    std::vector<row> batch;
    batch.reserve(kSweepBatch);
    {
      std::lock_guard lock(mutex_);
      if (!db_ || stopped()) return err(status::cancelled);
      dir = dir_;
      sqlite3_stmt* q = nullptr;
      if (sqlite3_prepare_v2(db_,
                             "SELECT rowid, spec, file FROM thumbs WHERE rowid>? AND rowid<=? "
                             "ORDER BY rowid LIMIT ?;",
                             -1, &q, nullptr) != SQLITE_OK) {
        return err(status::io);
      }
      sqlite3_bind_int64(q, 1, st.after_rowid);
      sqlite3_bind_int64(q, 2, st.max_rowid);
      sqlite3_bind_int(q, 3, kSweepBatch);
      while (sqlite3_step(q) == SQLITE_ROW) {
        row r;
        r.rowid = sqlite3_column_int64(q, 0);
        const unsigned char* spec = sqlite3_column_text(q, 1);
        r.current_spec = spec && std::strcmp(reinterpret_cast<const char*>(spec), kThumbSpec) == 0;
        if (const unsigned char* f = sqlite3_column_text(q, 2)) r.file.assign(reinterpret_cast<const char*>(f));
        batch.push_back(std::move(r));
      }
      sqlite3_finalize(q);
    }
    if (batch.empty()) {
      st.at = phase::orphans;
      break;
    }
    std::vector<std::int64_t> drop;
    std::vector<std::string> unlink;
    for (const row& r : batch) {
      st.after_rowid = r.rowid;
      const auto it = st.files.find(r.file);
      if (!r.current_spec) {
        // An old spec (a kThumbSpec bump): never looked up again.
        drop.push_back(r.rowid);
        if (it != st.files.end()) {
          st.stats.bytes_removed += it->second.size;
          ++st.stats.files_removed;
          unlink.push_back(r.file);
          st.files.erase(it);
        }
      } else if (it == st.files.end()) {
        drop.push_back(r.rowid);  // its JPEG has gone
      } else if (!it->second.kept) {
        it->second.kept = true;
        st.kept.push_back({r.rowid, r.file, it->second.size, it->second.mtime_unix});
      }
    }
    if (!drop.empty()) {
      std::lock_guard lock(mutex_);
      if (!db_ || stopped()) return err(status::cancelled);
      if (!delete_rowids(db_, drop)) return err(status::io);
      st.stats.rows_removed += drop.size();
    }
    for (std::size_t i = 0; i < unlink.size(); ++i) {
      if (i != 0 && i % kDeletesPerPace == 0) {
        // Deleting is idempotent: a sweep that steps aside here re-deletes
        // nothing it has done, since the rows are already gone.
        if (const int p = pace(); p == 2) return paused(p);
      }
      (void)io::remove_file(join_dir(dir, unlink[i]));
    }
  }

  if (st.at == phase::orphans) {
    if (st.cursor == 0 && st.orphans.empty()) {
      // Rows written since the sweep began (a re-store gets a new rowid) name
      // files the listing saw: those are not orphans.
      {
        std::lock_guard lock(mutex_);
        if (!db_ || stopped()) return err(status::cancelled);
        dir = dir_;
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT file FROM thumbs WHERE rowid>?;", -1, &q, nullptr) != SQLITE_OK) {
          return err(status::io);
        }
        sqlite3_bind_int64(q, 1, st.max_rowid);
        while (sqlite3_step(q) == SQLITE_ROW) {
          const unsigned char* f = sqlite3_column_text(q, 0);
          if (!f) continue;
          const auto it = st.files.find(reinterpret_cast<const char*>(f));
          if (it != st.files.end()) it->second.kept = true;
        }
        sqlite3_finalize(q);
      }
      for (const auto& [name, f] : st.files) {
        if (!f.kept && f.mtime_unix + kOrphanGraceSeconds <= st.started_unix) st.orphans.push_back(name);
      }
    }
    if (dir.empty()) {
      std::lock_guard lock(mutex_);
      dir = dir_;
    }
    while (st.cursor < st.orphans.size()) {
      if (st.cursor % kDeletesPerPace == 0) {
        if (const int p = pace(); p != 0) return paused(p);
      }
      const std::string& name = st.orphans[st.cursor++];
      const auto it = st.files.find(name);
      if (it == st.files.end()) continue;
      if (io::remove_file(join_dir(dir, name))) {
        st.stats.bytes_removed += it->second.size;
        ++st.stats.files_removed;
      }
      st.files.erase(it);
    }
    st.orphans.clear();
    st.orphans.shrink_to_fit();
    st.cursor = 0;
    for (const thumb_sweep::kept_row& k : st.kept) st.stats.bytes_kept += k.size;
    st.stats.files_kept = st.kept.size();
    st.at = phase::cap;
    // The oldest-written first, for the cap.
    std::sort(st.kept.begin(), st.kept.end(), [](const auto& a, const auto& b) {
      return a.mtime_unix != b.mtime_unix ? a.mtime_unix < b.mtime_unix : a.rowid < b.rowid;
    });
  }

  if (st.at == phase::cap) {
    if (dir.empty()) {
      std::lock_guard lock(mutex_);
      dir = dir_;
    }
    while (cap_bytes != 0 && st.stats.bytes_kept > cap_bytes && st.cursor < st.kept.size()) {
      if (const int p = pace(); p != 0) return paused(p);
      std::vector<std::int64_t> drop;
      std::vector<std::string> unlink;
      std::uint64_t freed = 0;
      while (st.cursor < st.kept.size() && drop.size() < kDeletesPerPace &&
             st.stats.bytes_kept - freed > cap_bytes) {
        const thumb_sweep::kept_row& k = st.kept[st.cursor++];
        drop.push_back(k.rowid);
        unlink.push_back(k.file);
        freed += k.size;
      }
      {
        std::lock_guard lock(mutex_);
        if (!db_ || stopped()) return err(status::cancelled);
        if (!delete_rowids(db_, drop)) return err(status::io);
      }
      for (const std::string& name : unlink) (void)io::remove_file(join_dir(dir, name));
      st.stats.rows_removed += drop.size();
      st.stats.files_removed += unlink.size();
      st.stats.bytes_removed += freed;
      st.stats.bytes_kept -= freed;
      st.stats.files_kept -= drop.size();
    }
    st.kept.clear();
    st.kept.shrink_to_fit();
    st.files.clear();
    st.at = phase::done;
  }

  if (stopped()) return err(status::cancelled);
  total_bytes_.store(st.stats.bytes_kept, std::memory_order_relaxed);
  total_known_.store(true, std::memory_order_relaxed);
  return true;
}

struct thumb_store::sweep_pass {
  job_system* jobs = nullptr;
  std::uint64_t cap_bytes = 0;
  std::function<bool()> busy;
  sweep_done_fn done;
  std::shared_ptr<void> keep_alive;
  thumb_sweep state;
  std::atomic<bool> finished{false};
};

bool thumb_store::submit_sweep(job_system& jobs, std::uint64_t cap_bytes, std::function<bool()> busy,
                               sweep_done_fn done, std::shared_ptr<void> keep_alive) {
  if (sweep_running_.exchange(true, std::memory_order_acq_rel)) return false;
  auto pass = std::make_shared<sweep_pass>();
  pass->jobs = &jobs;
  pass->cap_bytes = cap_bytes;
  pass->busy = std::move(busy);
  pass->done = std::move(done);
  pass->keep_alive = std::move(keep_alive);
  queue_sweep_step(pass);
  return true;
}

void thumb_store::queue_sweep_step(const std::shared_ptr<sweep_pass>& pass) {
  const auto finish = [this](sweep_pass& p, result<thumb_sweep_stats> r) {
    if (p.finished.exchange(true, std::memory_order_acq_rel)) return;
    sweep_running_.store(false, std::memory_order_release);
    if (p.done) p.done(std::move(r));
  };
  // Set by this step when it queued the next one: then its on_done is not the
  // pass's end.
  auto continued = std::make_shared<std::atomic<bool>>(false);
  const job_id id = pass->jobs->submit_at(
      background_generation,
      [this, pass, finish, continued](const job_context& ctx) -> status {
        job_system& jobs = *pass->jobs;
        const auto& busy = pass->busy;
        const sweep_pacer pacer = [&jobs, &busy]() -> sweep_pace {
          // The view first (CLAUDE.md): a view job waiting for a worker gets
          // this one; one running, or the viewer panning or playing, is
          // waited out.
          if (jobs.foreground_queued() != 0) return sweep_pace::step_aside;
          if (jobs.foreground_running() != 0 || (busy && busy())) return sweep_pace::wait;
          return sweep_pace::go;
        };
        auto r = sweep(pass->state, pass->cap_bytes, ctx, pacer);
        if (r && !r.value()) {
          continued->store(true, std::memory_order_release);
          queue_sweep_step(pass);  // behind the view job that wanted this worker
          return status::ok;
        }
        if (r) {
          finish(*pass, pass->state.stats);
          return status::ok;
        }
        const status why = r.error();
        finish(*pass, err(why));
        return why;
      },
      [pass, finish, continued](job_id, generation, status s) {
        // A step the pool drained without running it (shutdown), or one that
        // threw, ends the pass here. A step that queued the next does not.
        if (continued->load(std::memory_order_acquire)) return;
        finish(*pass, err(s == status::ok ? status::cancelled : s));
      });
  if (id == invalid_job) finish(*pass, err(status::cancelled));  // the pool is down
}

expected thumb_store::clear() {
  std::string dir;
  {
    std::lock_guard lock(mutex_);
    if (!db_) return err(status::invalid_arg);
    if (sqlite3_exec(db_, "DELETE FROM thumbs;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      return err(status::io);
    }
    // A lookup that found a row before this and its file missing after has
    // nothing left to drop; a sweep under way stops at its next step.
    ++epoch_;
    sweep_token_.fetch_add(1, std::memory_order_acq_rel);
    dir = dir_;
  }
  total_bytes_.store(0, std::memory_order_relaxed);
  total_known_.store(true, std::memory_order_relaxed);
  // The files with the lock released. Only names the cache writes:
  // meta_cache.db and thumbs.sqlite live here too. A file a store() wrote
  // just before the DELETE and rows just after is a row whose file is
  // missing, which the next verified lookup drops.
  MV_TRY(std::vector<io::dir_child> listed, io::list_children(dir));
  for (const io::dir_child& f : listed) {
    if (is_thumb_file_name(f.name)) (void)io::remove_file(join_dir(dir, f.name));
  }
  {
    // Give the emptied index's pages back. Best effort: another process
    // reading the cache (the Explorer handler, the AI pack) makes it busy.
    std::lock_guard lock(mutex_);
    if (db_) {
      sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);
      sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    }
  }
  return {};
}

}  // namespace mv::image
