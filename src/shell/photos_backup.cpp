// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/photos_backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/json.h"
#include "core/trace.h"
#include "io/collision_name.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/verified_copy.h"

namespace mv::shell::backup {
namespace {

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string sanitize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '-' || c == '_' || c == '.';
    out.push_back(ok ? c : '_');
  }
  return out;
}

// The last `n` components of a path, '/'-joined.
std::string tail_components(std::string_view path, int n) {
  std::size_t end = path.size();
  while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
  std::size_t begin = end;
  for (int seen = 0; begin > 0; --begin) {
    const char c = path[begin - 1];
    if ((c == '/' || c == '\\') && ++seen == n) break;
  }
  std::string out(path.substr(begin, end - begin));
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
  return out;
}

bool is_absolute(std::string_view path) {
  if (path.empty()) return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  return path.size() >= 2 && path[1] == ':';  // a drive letter
}

// A manifest row's place under the destination. The manifest keeps a backed-up
// file's place there ("2024/2024-05-17/IMG_0001.HEIC", "2024/IMG_0001.HEIC",
// "IMG_0001.HEIC": as deep as the layout it was written in), not the absolute
// path, so the backup is still found when the share mounts somewhere else
// (/Volumes/photos-1 after a stale /Volumes/photos, a drive letter that moved).
// Rows written before 2026-10-05 held absolute paths, all in the year/day
// layout (the only one then), so their last three components are that place.
std::string stored_place(std::string_view row_path) {
  if (is_absolute(row_path)) return tail_components(row_path, 3);
  std::string out(row_path);
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
  return out;
}

// The files copied so far, keyed by (asset, kind). SQLite, one row per file;
// WAL off and synchronous FULL: it lives on the destination, often a share.
class manifest {
 public:
  ~manifest() { close(); }

  // `read_only`: Check library reads a manifest and changes nothing in the
  // destination, not even this file's schema (no journal on the share, no
  // write lock against another Mac backing up into the same folder). An older
  // manifest then simply has no `found` column or `left_deleted` table.
  [[nodiscard]] expected open(const std::string& path, bool read_only = false) {
    close();
    if (sqlite3_open_v2(path.c_str(), &db_,
                        (read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      close();
      return err(status::io);
    }
    // Another writer (a choice being saved, another Mac's backup into this
    // folder) waits its turn instead of failing at once.
    sqlite3_busy_timeout(db_, 10000);
    if (read_only) {
      has_found_ = prepares("SELECT found FROM files LIMIT 0");
      has_left_ = prepares("SELECT hash FROM left_deleted LIMIT 0");
      if (!prepares("SELECT path FROM files LIMIT 0")) {
        close();
        return err(status::io);
      }
      return {};
    }
    const char* ddl =
        "PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS files("
        "  asset TEXT NOT NULL, kind INTEGER NOT NULL, path TEXT NOT NULL,"
        "  bytes INTEGER NOT NULL, hash TEXT NOT NULL, backed_up INTEGER NOT NULL,"
        "  PRIMARY KEY(asset, kind));"
        "CREATE TABLE IF NOT EXISTS runs("
        "  started INTEGER NOT NULL, finished INTEGER NOT NULL, done INTEGER NOT NULL,"
        "  skipped INTEGER NOT NULL, failed INTEGER NOT NULL, bytes INTEGER NOT NULL);";
    if (sqlite3_exec(db_, ddl, nullptr, nullptr, nullptr) != SQLITE_OK) {
      close();
      return err(status::io);
    }
    // Check library (2026-11-14): a row may say the asset's bytes were found
    // already in the library rather than copied. `path` is then the place under
    // the destination when it is inside it, else the absolute path. An older
    // manifest gains the column, every row 0 (copied); "duplicate column" on a
    // newer one is expected.
    (void)sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN found INTEGER NOT NULL DEFAULT 0", nullptr, nullptr,
                       nullptr);
    // Deleted on purpose (owner, 2026-11-14): bytes someone chose to leave
    // deleted from this destination, by hash, whatever asset they come back as.
    if (sqlite3_exec(db_,
                     "CREATE TABLE IF NOT EXISTS left_deleted(hash TEXT PRIMARY KEY, bytes INTEGER NOT NULL,"
                     " decided INTEGER NOT NULL)",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
      close();
      return err(status::io);
    }
    has_found_ = true;
    has_left_ = true;
    return {};
  }

  [[nodiscard]] bool prepares(const char* sql) {
    sqlite3_stmt* st = nullptr;
    const bool ok = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK;
    sqlite3_finalize(st);
    return ok;
  }

  // hash -> bytes of every Leave deleted.
  [[nodiscard]] std::unordered_map<std::string, std::uint64_t> left_deleted() {
    std::unordered_map<std::string, std::uint64_t> out;
    sqlite3_stmt* st = nullptr;
    if (!has_left_ || sqlite3_prepare_v2(db_, "SELECT hash, bytes FROM left_deleted", -1, &st, nullptr) != SQLITE_OK) {
      return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* h = sqlite3_column_text(st, 0);
      if (h) out.emplace(reinterpret_cast<const char*>(h), static_cast<std::uint64_t>(sqlite3_column_int64(st, 1)));
    }
    sqlite3_finalize(st);
    return out;
  }

  [[nodiscard]] bool exec_bound(const char* sql, const std::function<void(sqlite3_stmt*)>& bind) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    bind(st);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
  }
  [[nodiscard]] bool begin() { return sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK; }
  [[nodiscard]] bool commit() { return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK; }
  void rollback() { (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }

  void close() noexcept {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
  }

  struct row {
    std::string path;
    std::uint64_t bytes = 0;
    std::string hash;
    bool found = false;
  };

  // The size of every row, found ones included.
  [[nodiscard]] std::vector<std::uint64_t> all_sizes() {
    std::vector<std::uint64_t> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT bytes FROM files", -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)));
    sqlite3_finalize(st);
    return out;
  }

  // Every copied row (not the found ones), for Check library's deleted-since list.
  [[nodiscard]] std::vector<row> copied_rows() {
    std::vector<row> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, has_found_ ? "SELECT path, bytes, hash FROM files WHERE found=0"
                                           : "SELECT path, bytes, hash FROM files",
                           -1, &st, nullptr) != SQLITE_OK) {
      return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      row r;
      const unsigned char* p = sqlite3_column_text(st, 0);
      r.path = p ? reinterpret_cast<const char*>(p) : "";
      r.bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
      const unsigned char* h = sqlite3_column_text(st, 2);
      r.hash = h ? reinterpret_cast<const char*>(h) : "";
      out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
  }

  [[nodiscard]] std::optional<row> lookup(const asset_file& f) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           has_found_ ? "SELECT path, bytes, hash, found FROM files WHERE asset=? AND kind=?"
                                      : "SELECT path, bytes, hash, 0 FROM files WHERE asset=? AND kind=?",
                           -1, &st, nullptr) != SQLITE_OK) {
      return std::nullopt;
    }
    sqlite3_bind_text(st, 1, f.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int>(f.kind));
    std::optional<row> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
      row r;
      const unsigned char* p = sqlite3_column_text(st, 0);
      r.path = p ? reinterpret_cast<const char*>(p) : "";
      r.bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
      const unsigned char* h = sqlite3_column_text(st, 2);
      r.hash = h ? reinterpret_cast<const char*>(h) : "";
      r.found = sqlite3_column_int(st, 3) != 0;
      out = std::move(r);
    }
    sqlite3_finalize(st);
    return out;
  }

  void record(const asset_file& f, const std::string& path, std::uint64_t bytes, const std::string& hash,
              bool found = false) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT OR REPLACE INTO files(asset, kind, path, bytes, hash, backed_up, found) "
                           "VALUES(?,?,?,?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) {
      return;
    }
    sqlite3_bind_text(st, 1, f.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int>(f.kind));
    sqlite3_bind_text(st, 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(bytes));
    sqlite3_bind_text(st, 5, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, static_cast<sqlite3_int64>(now_unix()));
    sqlite3_bind_int(st, 7, found ? 1 : 0);
    // A lost row means the next run copies that file again beside itself.
    if (sqlite3_step(st) != SQLITE_DONE) MV_LOG_WARN("photos backup: a manifest row could not be written");
    sqlite3_finalize(st);
  }

  void record_run(const progress& p) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO runs(started, finished, done, skipped, failed, bytes) VALUES(?,?,?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) {
      return;
    }
    sqlite3_bind_int64(st, 1, p.started_unix);
    sqlite3_bind_int64(st, 2, p.finished_unix);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(p.done));
    sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(p.skipped));
    sqlite3_bind_int64(st, 5, static_cast<sqlite3_int64>(p.failed));
    sqlite3_bind_int64(st, 6, static_cast<sqlite3_int64>(p.bytes));
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
  }

 private:
  sqlite3* db_ = nullptr;
  bool has_found_ = true;
  bool has_left_ = true;
};

// The user's own report, in the destination: which files did not make it.
void write_report(const std::string& dir, const progress& p, const std::vector<std::string>& failures) {
  const std::string path = io::join_path(dir, "last-run.txt");
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  char when[64] = {};
  const time_t t = static_cast<time_t>(p.finished_unix);
  struct tm tmv {};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
  std::fprintf(f, "MediaViewer Photos backup, %s\n", when);
  std::fprintf(f, "%s\n", p.state == run_state::done        ? "Finished."
                           : p.state == run_state::cancelled ? "Cancelled; run again to continue."
                                                             : "Stopped on an error; run again to continue.");
  std::fprintf(f,
               "copied and verified: %llu\nalready in the library (not copied): %llu\nalready backed up: %llu\n"
               "failed: %llu\nbytes copied: %llu\n",
               static_cast<unsigned long long>(p.done), static_cast<unsigned long long>(p.found),
               static_cast<unsigned long long>(p.skipped),
               static_cast<unsigned long long>(p.failed), static_cast<unsigned long long>(p.bytes));
  if (!failures.empty()) {
    std::fprintf(f, "\nNot backed up (run again, or check the file in Photos):\n");
    for (const std::string& name : failures) std::fprintf(f, "  %s\n", name.c_str());
  }
  std::fclose(f);
}

}  // namespace

std::string layout_subdir(std::int64_t created_unix, layout folders) {
  if (folders == layout::flat) return {};
  const time_t t = static_cast<time_t>(created_unix);
  struct tm tmv {};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  const char* format = "%Y/%Y-%m-%d";
  switch (folders) {
    case layout::year: format = "%Y"; break;
    case layout::year_month: format = "%Y/%m"; break;
    case layout::year_month_day: format = "%Y/%m/%d"; break;
    case layout::day: format = "%Y-%m-%d"; break;
    case layout::year_day:
    case layout::flat: break;
  }
  char out[32] = {};
  std::strftime(out, sizeof(out), format, &tmv);
  return out;
}

std::string layout_dir(std::string_view destination, std::int64_t created_unix, layout folders) {
  const std::string sub = layout_subdir(created_unix, folders);
  return sub.empty() ? std::string(destination) : io::join_path(destination, io::native_relative(sub));
}

std::string housekeeping_dir(std::string_view destination) {
  return io::join_path(destination, ".mediaviewer-photos-backup");
}

engine::engine() = default;

engine::~engine() {
  cancel();
  join();
}

expected engine::start(std::unique_ptr<source> src, options opts) {
  if (!src || opts.destination.empty() || opts.scratch.empty()) return err(status::invalid_arg);
  if (static_cast<std::uint8_t>(opts.folders) >= kLayoutCount) return err(status::invalid_arg);
  if (running_.load(std::memory_order_acquire)) return err(status::invalid_arg);
  join();
  source_ = std::move(src);
  options_ = std::move(opts);
  cancel_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    progress_ = progress{};
    progress_.state = run_state::listing;
    progress_.started_unix = now_unix();
  }
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { run(); });
  return {};
}

void engine::cancel() noexcept { cancel_.store(true, std::memory_order_release); }

void engine::join() noexcept {
  if (thread_.joinable()) thread_.join();
}

bool engine::running() const noexcept { return running_.load(std::memory_order_acquire); }

progress engine::snapshot() const {
  std::lock_guard lock(mutex_);
  return progress_;
}

check_report engine::last_check() const {
  std::lock_guard lock(mutex_);
  return check_;
}

namespace {

// `path` under `destination` as a manifest place ('/'-separated), or "" when
// it is somewhere else.
std::string place_under(std::string_view destination, std::string_view path) {
  std::string root(destination);
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
  if (path.size() <= root.size() + 1 || path.compare(0, root.size(), root) != 0) return {};
  const char sep = path[root.size()];
  if (sep != '/' && sep != '\\') return {};
  std::string out(path.substr(root.size() + 1));
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
  return out;
}

// The library file a found row points at: inside the destination when one is.
const std::string& pick_where(std::string_view destination, const std::vector<std::string>& where) {
  for (const std::string& w : where) {
    if (!place_under(destination, w).empty()) return w;
  }
  return where.front();
}

}  // namespace

void engine::run() {
  const auto set = [this](auto&& fn) {
    std::lock_guard lock(mutex_);
    fn(progress_);
  };
  const auto finish = [&](run_state state, const char* why) {
    set([&](progress& p) {
      p.state = state;
      p.error = why ? why : "";
      p.current.clear();
      p.finished_unix = now_unix();
    });
    running_.store(false, std::memory_order_release);
  };
  const bool check_only = options_.check_only;

  auto listed = source_->enumerate(&cancel_);
  if (!listed) {
    finish(cancel_.load() ? run_state::cancelled : run_state::failed,
           listed.error() == status::permission_denied ? "MediaViewer may not read your Photos library."
                                                        : "The Photos library could not be listed.");
    return;
  }
  std::vector<asset_file> files = std::move(listed).value();
  set([&](progress& p) {
    p.total = files.size();
    p.state = check_only ? run_state::checking : run_state::copying;
  });

  const std::string keep = housekeeping_dir(options_.destination);
  const std::string manifest_path = io::join_path(keep, "manifest.sqlite");
  manifest log;
  bool have_log = false;
  if (check_only) {
    // Report only: nothing is created in the destination. An existing manifest
    // is read (and gains the `found` column, its own bookkeeping).
    const auto root = io::stat_path(options_.destination);
    if (!root || !root->is_directory) {
      {
        std::lock_guard lock(mutex_);
        check_ = check_report{};
        check_.originals = files.size();
      }
      finish(run_state::failed, "The destination folder is not reachable.");
      return;
    }
    if (io::file_exists(manifest_path)) have_log = static_cast<bool>(log.open(manifest_path, true));
  } else {
    if (!io::make_directories(options_.destination) || !io::make_directories(keep) ||
        !io::make_directories(options_.scratch)) {
      finish(run_state::failed, "The destination folder could not be written.");
      return;
    }
    if (!log.open(manifest_path)) {
      finish(run_state::failed, "The backup's manifest could not be opened in the destination.");
      return;
    }
    have_log = true;
  }

  // The library side (docs/plans/check-library.md): walk the destination, hash
  // only the sizes the Photos library has, and learn which manifest files have
  // gone or moved. Without an index (no Import) the backup is as before.
  std::optional<library_check_output> lib;
  std::unordered_map<std::string, std::string> moved;  // manifest place -> where its bytes are now
  std::unordered_set<std::string> gone_places;
  if (options_.index && !cancel_.load(std::memory_order_acquire)) {
    set([&](progress& p) { p.state = run_state::checking; });
    library_check_input in;
    in.destination = options_.destination;
    in.scope = options_.scope;
    {
      std::unordered_set<std::uint64_t> sizes;
      for (const asset_file& f : files) {
        if (f.size > 0) sizes.insert(f.size);
      }
      in.sizes.assign(sizes.begin(), sizes.end());
      std::sort(in.sizes.begin(), in.sizes.end());
    }
    if (have_log) {
      for (manifest::row& r : log.copied_rows()) {
        in.expected.push_back({stored_place(r.path), r.bytes, std::move(r.hash)});
      }
      // The bytes every manifest row relies on are always indexed (a found
      // file moved inside the library is then found where it went, even when
      // PhotoKit gave no size for its asset).
      std::unordered_set<std::uint64_t> sizes(in.sizes.begin(), in.sizes.end());
      for (const std::uint64_t b : log.all_sizes()) {
        if (b > 0 && sizes.insert(b).second) in.sizes.push_back(b);
      }
      std::sort(in.sizes.begin(), in.sizes.end());
    }
    auto checked = options_.index->check(in, &cancel_, [&](const library_progress& lp) {
      set([&](progress& p) {
        p.bytes = lp.bytes_done;
        p.bytes_total = lp.bytes_total;
      });
    });
    if (checked) {
      lib = std::move(checked).value();
      for (const library_gone& g : lib->gone) {
        if (g.moved_to.empty()) {
          gone_places.insert(g.rel);
        } else {
          moved.emplace(g.rel, g.moved_to);
        }
      }
    } else if (checked.error() == status::cancelled || cancel_.load(std::memory_order_acquire)) {
      finish(run_state::cancelled, nullptr);
      return;
    } else {
      MV_LOG_WARN("photos backup: the library index did not answer; backing up without it");
    }
    set([&](progress& p) {
      p.bytes = 0;
      p.bytes_total = 0;
      p.state = check_only ? run_state::checking : run_state::copying;
    });
  }
  const auto size_in_library = [&](std::uint64_t size) {
    return lib && size > 0 && std::binary_search(lib->matched_sizes.begin(), lib->matched_sizes.end(), size);
  };
  // Every library file with these bytes, or nothing (no index, no match, an error).
  // nullopt: the index could not answer (an error is not "nowhere").
  const auto find_answer = [&](std::uint64_t size, const io::content_hash& h)
      -> std::optional<std::vector<std::string>> {
    if (!lib) return std::nullopt;
    auto r = options_.index->find(options_.destination, options_.scope, size, h);
    if (!r) return std::nullopt;
    return std::move(r).value();
  };
  const auto find_in_library = [&](std::uint64_t size, const io::content_hash& h) {
    return find_answer(size, h).value_or(std::vector<std::string>{});
  };
  // A found row's file: inside the destination, its place; else absolute.
  const auto found_path = [&](const std::string& where) {
    std::string place = place_under(options_.destination, where);
    return place.empty() ? where : place;
  };

  check_report report;
  // Deleted on purpose: bytes someone chose to leave deleted, and their sizes
  // (only a size among them costs a hash before a copy).
  const std::unordered_map<std::string, std::uint64_t> left = have_log ? log.left_deleted()
                                                                      : std::unordered_map<std::string, std::uint64_t>{};
  std::unordered_set<std::uint64_t> left_sizes;
  for (const auto& [hash, bytes] : left) left_sizes.insert(bytes);
  // A file backed up and then removed from the folder: held, or left deleted.
  const auto hold = [&](const asset_file& f, const manifest::row& row, std::string place) {
    const bool chosen = left.count(row.hash) > 0;
    if (check_only) {
      report.deleted.push_back({f.id, f.kind, f.filename, std::move(place), row.bytes, chosen});
    }
    set([&](progress& p) { ++(chosen ? p.left_deleted : p.held); });
  };

  report.index = lib.has_value();
  report.reachable = true;
  report.originals = files.size();

  std::vector<std::string> failures;
  bool stopped = false;
  const char* abort_why = nullptr;
  // One file did not make it. If the destination itself has gone (the NAS
  // dropped off the network, the drive was unplugged) every file after it
  // would fail too -- each one listed in the report, an iCloud original
  // possibly fetched for nothing -- so the run stops instead; run it again
  // when the folder is back. One stat, only on a failure.
  const auto fail_file = [&](const asset_file& f) {
    failures.push_back(f.filename);
    set([&](progress& p) { ++p.failed; });
    const auto root = io::stat_path(options_.destination);
    if (!root || !root->is_directory) {
      abort_why = "The destination folder is no longer reachable; run again when it is back.";
      return true;
    }
    return false;
  };
  for (const asset_file& f : files) {
    if (cancel_.load(std::memory_order_acquire)) {
      stopped = true;
      break;
    }
    set([&](progress& p) { p.current = f.filename; });

    // Already there: the manifest says so and the file is where it was.
    if (const auto row = have_log ? log.lookup(f) : std::nullopt) {
      if (row->found) {
        // Found in the library before: still there by content, wherever it
        // has been moved to since; without an index, at the place recorded.
        // An empty place: an asset whose bytes someone chose to leave deleted
        // (recorded so it is not fetched again each run; see below).
        if (row->path.empty()) {
          hold(f, *row, row->path);
          continue;
        }
        // Gone only when that is certain: nothing is at the place (not found,
        // not merely a stat that failed), its folder answers (2026-11-09: an
        // offline share is not a deletion), and the index, in a scope that can
        // see the place, answered "nowhere". An error, a scope that cannot see
        // it (found under "every library", checked now under "this folder"),
        // or an unreachable folder all leave it found.
        const bool outside = is_absolute(row->path);
        const std::string where = outside ? row->path
                                          : io::join_path(options_.destination, io::native_relative(row->path));
        const auto st = io::stat_path(where);
        io::content_hash h;
        const bool index_sees = lib && io::content_hash::from_hex(row->hash, h) &&
                                (!outside || options_.scope == library_scope::library);
        enum class verdict { still, gone, recopy } v = verdict::still;
        if (st && !st->is_directory && st->size == row->bytes) {
          v = verdict::still;
        } else if (st && !st->is_directory) {
          // Something else at its place now (edited, replaced): copied again,
          // as a copied file is, unless its bytes are elsewhere in the library.
          const auto answer = index_sees ? find_answer(row->bytes, h) : std::nullopt;
          v = answer && !answer->empty() ? verdict::still : answer || !index_sees ? verdict::recopy : verdict::still;
        } else if (!st && st.error() == status::not_found) {
          const std::string parent(io::parent_of(where));
          const auto folder = io::stat_path(parent);
          bool folder_answers = folder && folder->is_directory;
          // Outside the backup, an empty folder is not proof: an offline
          // share's mount point is often left behind empty. Its folder must
          // hold something else for this file to count as removed.
          if (folder_answers && outside) {
            const auto children = io::list_children(parent, true);
            folder_answers = children && !children->empty();
          }
          if (!folder_answers) {
            v = verdict::still;
          } else if (index_sees) {
            const auto answer = find_answer(row->bytes, h);
            v = answer && answer->empty() ? verdict::gone : verdict::still;
          } else {
            v = verdict::gone;
          }
        }
        if (v == verdict::still) {
          ++report.in_backup;
          set([&](progress& p) { ++p.skipped; });
          continue;
        }
        if (v == verdict::gone) {
          // Its bytes were in the library and someone removed them: deleted
          // since, held like a copied file (below).
          hold(f, *row, row->path);
          continue;
        }
      } else {
        // Wherever it was written, in whichever layout: a layout change moves nothing.
        const std::string place = stored_place(row->path);
        const std::string where = io::join_path(options_.destination, io::native_relative(place));
        const auto st = io::stat_path(where);
        if (st && !st->is_directory && st->size == row->bytes) {
          ++report.in_backup;
          set([&](progress& p) { ++p.skipped; });
          continue;
        }
        // Something else is at its place now (edited in place, replaced):
        // copied again beside it, as before (a new name, never an overwrite).
        // Absent means not found: a stat that failed otherwise says nothing.
        const bool absent = !st && st.error() == status::not_found;
        if (!st && !absent) {
          ++report.in_backup;
          set([&](progress& p) { ++p.skipped; });
          continue;
        }
        // Not where it was put, but its bytes are elsewhere in the library:
        // moved (reorganised by someone), not deleted. Found, not copied again.
        if (const auto it = moved.find(place); it != moved.end()) {
          if (check_only) {
            report.in_library.push_back({f.filename, {it->second}});
          } else {
            log.record(f, found_path(it->second), row->bytes, row->hash, true);
            set([&](progress& p) { ++p.found; });
          }
          continue;
        }
        // Deleted since it was backed up (owner, 2026-11-14): never copied
        // back by itself. The destination answers and holds this manifest, so
        // this is the real folder, not an offline share's empty mount point.
        // Check library offers Copy again or Leave deleted.
        if (absent) {
          hold(f, *row, place);
          continue;
        }
      }
    }

    if (check_only) {
      // Report only: never fetch from iCloud to answer a question.
      if (!size_in_library(f.size)) {
        ++report.to_copy;
        report.to_copy_bytes += f.size;
        continue;
      }
      auto local = source_->local_file(f);
      if (!local || local->empty()) {
        ++report.unconfirmed;
        continue;
      }
      const auto st = io::stat_path(*local);
      auto h = st ? io::hash_file(*local, io::read_mode::sequential, &cancel_) : result<io::content_hash>(err(status::io));
      if (!h) {
        if (cancel_.load(std::memory_order_acquire)) {
          stopped = true;
          break;
        }
        ++report.to_copy;
        report.to_copy_bytes += f.size;
        continue;
      }
      std::vector<std::string> where = find_in_library(st->size, *h);
      if (where.empty()) {
        ++report.to_copy;
        report.to_copy_bytes += f.size;
      } else {
        report.in_library.push_back({f.filename, std::move(where)});
      }
      continue;
    }

    std::string src;
    std::string tmp;
    if (auto local = source_->local_file(f); local && !local->empty()) {
      src = std::move(local).value();
    } else {
      tmp = io::join_path(options_.scratch, sanitize(f.id) + "-" + std::to_string(static_cast<int>(f.kind)) + "-" +
                                                 sanitize(f.filename));
      (void)io::remove_file(tmp);
      if (auto fetched = source_->fetch(f, tmp, &cancel_); !fetched) {
        (void)io::remove_file(tmp);
        if (cancel_.load(std::memory_order_acquire)) {
          stopped = true;
          break;
        }
        if (fail_file(f)) break;
        continue;
      }
      src = tmp;
    }

    // Already in the library by content (however it got there): recorded as
    // found, not copied. Bytes someone chose to leave deleted are not copied
    // either, whatever asset they came back as. Only a size the library (or
    // the left-deleted list) has costs this extra read.
    if (lib || !left_sizes.empty()) {
      if (const auto st = io::stat_path(src);
          st && (size_in_library(st->size) || left_sizes.count(st->size) > 0)) {
        if (auto h = io::hash_file(src, io::read_mode::sequential, &cancel_)) {
          if (left.count(h->hex()) > 0) {
            if (!tmp.empty()) (void)io::remove_file(tmp);
            // Remembered for this asset too (an empty place), so later runs
            // hold it without fetching it again; Copy again forgets the row.
            log.record(f, std::string(), st->size, h->hex(), true);
            set([&](progress& p) { ++p.left_deleted; });
            continue;
          }
          std::vector<std::string> where = find_in_library(st->size, *h);
          if (!where.empty()) {
            if (!tmp.empty()) (void)io::remove_file(tmp);
            log.record(f, found_path(pick_where(options_.destination, where)), st->size, h->hex(), true);
            set([&](progress& p) { ++p.found; });
            continue;
          }
        } else if (cancel_.load(std::memory_order_acquire)) {
          if (!tmp.empty()) (void)io::remove_file(tmp);
          stopped = true;
          break;
        }
      }
    }

    const std::string sub = layout_subdir(f.created_unix, options_.folders);
    const std::string dir = sub.empty() ? options_.destination
                                        : io::join_path(options_.destination, io::native_relative(sub));
    // The folder is made only now, after the bytes are here (a fetch can take
    // minutes): a destination that went away meanwhile is never made again on
    // this Mac's disk (a stale /Volumes mount point), the run stops instead.
    const auto root = io::stat_path(options_.destination);
    if (!root || !root->is_directory || !io::make_directories(dir)) {
      if (!tmp.empty()) (void)io::remove_file(tmp);
      if (fail_file(f)) break;
      continue;
    }
    // Never overwrite: another asset's file of the same name in the same
    // folder gets "(2)" (io/collision_name.h, the F7/F8 rule).
    const std::string name = io::unique_name(
        f.filename, [&](std::string_view candidate) { return io::file_exists(io::join_path(dir, candidate)); });
    if (name.empty()) {
      if (!tmp.empty()) (void)io::remove_file(tmp);
      if (fail_file(f)) break;
      continue;
    }
    const std::string final_path = io::join_path(dir, name);

    io::copy_options copy;
    copy.cancel = &cancel_;
    copy.on_progress = [&](std::uint64_t delta) {
      set([&](progress& p) { p.bytes += delta; });
    };
    const std::string targets[] = {final_path};
    auto copied = io::verified_copy(src, targets, copy);
    if (!tmp.empty()) (void)io::remove_file(tmp);
    if (!copied) {
      if (copied.error() == status::cancelled || cancel_.load(std::memory_order_acquire)) {
        stopped = true;
        break;
      }
      if (fail_file(f)) break;
      continue;
    }
    const io::copy_target_result& t = copied->targets[0];
    if (!io::copy_succeeded(t.outcome)) {
      if (t.outcome == io::copy_target_outcome::cancelled) {
        stopped = true;
        break;
      }
      if (fail_file(f)) break;
      continue;
    }
    log.record(f, sub.empty() ? name : sub + "/" + name, copied->bytes, copied->source_hash.hex());
    // The library index learns the copy, so Import (and the next check)
    // knows these bytes are here without reading them.
    if (lib) {
      if (const auto st = io::stat_path(final_path)) {
        options_.index->record(options_.destination, final_path, st->size, st->mtime_unix, copied->source_hash);
      }
    }
    set([&](progress& p) {
      ++p.done;
      if (!tmp.empty()) ++p.fetched;
    });
  }

  if (check_only) {
    if (lib) {
      report.gone = lib->gone;
      report.duplicates = lib->duplicates;
      report.library_files = lib->library_files;
      report.hashed = lib->hashed;
      report.remembered = lib->remembered;
    }
    {
      std::lock_guard lock(mutex_);
      check_ = std::move(report);
    }
    finish(stopped ? run_state::cancelled : run_state::done, nullptr);
    MV_LOG_INFO("photos check: %s", stopped ? "cancelled" : "done");
    return;
  }

  (void)io::remove_tree(options_.scratch);
  finish(abort_why ? run_state::failed : stopped ? run_state::cancelled : run_state::done, abort_why);
  const progress final_state = snapshot();
  log.record_run(final_state);
  write_report(keep, final_state, failures);
  MV_LOG_INFO("photos backup: %s, %llu copied, %llu found in the library, %llu already there, %llu failed",
              abort_why ? "destination gone" : stopped ? "cancelled" : "done", static_cast<unsigned long long>(final_state.done),
              static_cast<unsigned long long>(final_state.found),
              static_cast<unsigned long long>(final_state.skipped),
              static_cast<unsigned long long>(final_state.failed));
}

expected decide_deleted(std::string_view destination, const std::vector<deleted_choice>& choices) {
  const std::string path = io::join_path(housekeeping_dir(destination), "manifest.sqlite");
  if (!io::file_exists(path)) return err(status::not_found);
  manifest log;
  MV_TRY_VOID(log.open(path));
  if (!log.begin()) return err(status::io);
  bool ok = true;
  for (const deleted_choice& c : choices) {
    asset_file f;
    f.id = c.asset;
    f.kind = c.kind;
    const auto row = log.lookup(f);
    if (!row) {
      log.rollback();
      return err(status::invalid_arg);
    }
    if (c.copy_again) {
      // Forget the row: the next run sees a new file (and still skips it if its
      // bytes are in the library). Forget a Leave deleted for those bytes too.
      ok = log.exec_bound("DELETE FROM files WHERE asset=? AND kind=?", [&](sqlite3_stmt* st) {
             sqlite3_bind_text(st, 1, c.asset.c_str(), -1, SQLITE_TRANSIENT);
             sqlite3_bind_int(st, 2, static_cast<int>(c.kind));
           }) &&
           log.exec_bound("DELETE FROM left_deleted WHERE hash=?", [&](sqlite3_stmt* st) {
             sqlite3_bind_text(st, 1, row->hash.c_str(), -1, SQLITE_TRANSIENT);
           }) &&
           // Other assets held only because these bytes were left deleted (an
           // empty place) are forgotten too: they are copied again with them.
           log.exec_bound("DELETE FROM files WHERE hash=? AND found=1 AND path=''", [&](sqlite3_stmt* st) {
             sqlite3_bind_text(st, 1, row->hash.c_str(), -1, SQLITE_TRANSIENT);
           });
    } else {
      ok = row->hash.size() == 64 &&
           log.exec_bound("INSERT OR REPLACE INTO left_deleted(hash, bytes, decided) VALUES(?,?,?)",
                          [&](sqlite3_stmt* st) {
                            sqlite3_bind_text(st, 1, row->hash.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(row->bytes));
                            sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(now_unix()));
                          });
    }
    if (!ok) break;
  }
  if (!ok || !log.commit()) {
    log.rollback();
    return err(status::io);
  }
  return {};
}

std::string check_report_json(const check_report& r) {
  json::writer w;
  w.begin_object();
  w.key("index").boolean(r.index);
  w.key("reachable").boolean(r.reachable);
  w.key("originals").integer(static_cast<std::int64_t>(r.originals));
  w.key("in_backup").integer(static_cast<std::int64_t>(r.in_backup));
  w.key("in_library_count").integer(static_cast<std::int64_t>(r.in_library.size()));
  w.key("to_copy").integer(static_cast<std::int64_t>(r.to_copy));
  w.key("to_copy_bytes").integer(static_cast<std::int64_t>(r.to_copy_bytes));
  w.key("unconfirmed").integer(static_cast<std::int64_t>(r.unconfirmed));
  w.key("library_files").integer(static_cast<std::int64_t>(r.library_files));
  w.key("hashed").integer(r.hashed);
  w.key("remembered").integer(r.remembered);
  w.key("in_library").begin_array();
  for (const check_report::found_file& f : r.in_library) {
    w.begin_object();
    w.key("name").string(f.name);
    w.key("where").begin_array();
    for (const std::string& p : f.where) w.string(p);
    w.end_array();
    w.end_object();
  }
  w.end_array();
  w.key("gone").begin_array();
  for (const library_gone& g : r.gone) {
    w.begin_object();
    w.key("rel").string(g.rel);
    w.key("moved_to").string(g.moved_to);
    w.end_object();
  }
  w.end_array();
  w.key("deleted").begin_array();
  for (const check_report::deleted_file& d : r.deleted) {
    w.begin_object();
    w.key("asset").string(d.asset);
    w.key("kind").integer(static_cast<int>(d.kind));
    w.key("name").string(d.name);
    w.key("place").string(d.place);
    w.key("bytes").integer(static_cast<std::int64_t>(d.bytes));
    w.key("left").boolean(d.left);
    w.end_object();
  }
  w.end_array();
  w.key("duplicates").begin_array();
  for (const library_group& g : r.duplicates) {
    w.begin_object();
    w.key("size").integer(static_cast<std::int64_t>(g.size));
    w.key("paths").begin_array();
    for (const std::string& p : g.paths) w.string(p);
    w.end_array();
    w.end_object();
  }
  w.end_array();
  w.end_object();
  return w.take();
}

}  // namespace mv::shell::backup
