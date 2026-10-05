// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/photos_backup.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <utility>

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

// The last `n` components of a path, '/'-joined: a backed-up file's place
// under the destination ("2024/2024-05-17/IMG_0001.HEIC"). The manifest keeps
// this, not the absolute path, so the backup is still found when the share
// mounts somewhere else (/Volumes/photos-1 after a stale /Volumes/photos, a
// drive letter that moved). Rows written before this held absolute paths;
// their tail is the same three components, so they rebase too.
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

// The files copied so far, keyed by (asset, kind). SQLite, one row per file;
// WAL off and synchronous FULL: it lives on the destination, often a share.
class manifest {
 public:
  ~manifest() { close(); }

  [[nodiscard]] expected open(const std::string& path) {
    close();
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      close();
      return err(status::io);
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
    return {};
  }

  void close() noexcept {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
  }

  struct row {
    std::string path;
    std::uint64_t bytes = 0;
  };

  [[nodiscard]] std::optional<row> lookup(const asset_file& f) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT path, bytes FROM files WHERE asset=? AND kind=?", -1, &st, nullptr) !=
        SQLITE_OK) {
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
      out = std::move(r);
    }
    sqlite3_finalize(st);
    return out;
  }

  void record(const asset_file& f, const std::string& path, std::uint64_t bytes, const std::string& hash) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT OR REPLACE INTO files(asset, kind, path, bytes, hash, backed_up) "
                           "VALUES(?,?,?,?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) {
      return;
    }
    sqlite3_bind_text(st, 1, f.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int>(f.kind));
    sqlite3_bind_text(st, 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(bytes));
    sqlite3_bind_text(st, 5, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, static_cast<sqlite3_int64>(now_unix()));
    (void)sqlite3_step(st);
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
  std::fprintf(f, "copied and verified: %llu\nalready backed up: %llu\nfailed: %llu\nbytes copied: %llu\n",
               static_cast<unsigned long long>(p.done), static_cast<unsigned long long>(p.skipped),
               static_cast<unsigned long long>(p.failed), static_cast<unsigned long long>(p.bytes));
  if (!failures.empty()) {
    std::fprintf(f, "\nNot backed up (run again, or check the file in Photos):\n");
    for (const std::string& name : failures) std::fprintf(f, "  %s\n", name.c_str());
  }
  std::fclose(f);
}

}  // namespace

std::string layout_dir(std::string_view destination, std::int64_t created_unix) {
  char year[8] = {};
  char day[16] = {};
  const time_t t = static_cast<time_t>(created_unix);
  struct tm tmv {};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  std::strftime(year, sizeof(year), "%Y", &tmv);
  std::strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
  return io::join_path(io::join_path(destination, year), day);
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
    p.state = run_state::copying;
  });

  const std::string keep = housekeeping_dir(options_.destination);
  if (!io::make_directories(options_.destination) || !io::make_directories(keep) ||
      !io::make_directories(options_.scratch)) {
    finish(run_state::failed, "The destination folder could not be written.");
    return;
  }
  manifest log;
  if (!log.open(io::join_path(keep, "manifest.sqlite"))) {
    finish(run_state::failed, "The backup's manifest could not be opened in the destination.");
    return;
  }

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
    if (const auto row = log.lookup(f)) {
      const std::string where =
          io::join_path(options_.destination, io::native_relative(tail_components(row->path, 3)));
      if (auto st = io::stat_path(where); st && !st->is_directory && st->size == row->bytes) {
        set([&](progress& p) { ++p.skipped; });
        continue;
      }
    }

    const std::string dir = layout_dir(options_.destination, f.created_unix);
    if (!io::make_directories(dir)) {
      if (fail_file(f)) break;
      continue;
    }
    // Never overwrite: another asset's file of the same name on the same day
    // gets "(2)" (io/collision_name.h, the F7/F8 rule).
    const std::string name = io::unique_name(
        f.filename, [&](std::string_view candidate) { return io::file_exists(io::join_path(dir, candidate)); });
    if (name.empty()) {
      if (fail_file(f)) break;
      continue;
    }
    const std::string final_path = io::join_path(dir, name);

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
    log.record(f, tail_components(final_path, 3), copied->bytes, copied->source_hash.hex());
    set([&](progress& p) {
      ++p.done;
      if (!tmp.empty()) ++p.fetched;
    });
  }

  (void)io::remove_tree(options_.scratch);
  finish(abort_why ? run_state::failed : stopped ? run_state::cancelled : run_state::done, abort_why);
  const progress final_state = snapshot();
  log.record_run(final_state);
  write_report(keep, final_state, failures);
  MV_LOG_INFO("photos backup: %s, %llu copied, %llu already there, %llu failed",
              abort_why ? "destination gone" : stopped ? "cancelled" : "done", static_cast<unsigned long long>(final_state.done),
              static_cast<unsigned long long>(final_state.skipped),
              static_cast<unsigned long long>(final_state.failed));
}

}  // namespace mv::shell::backup
