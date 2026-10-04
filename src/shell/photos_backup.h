// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Backing up a Photos library's originals to a folder (plan/26-photos-library.md
// "Backup"; owner, 2026-10-03: "backup all my icloud photos to a local
// directory"). Portable: the library is behind `source` (PhotoKit on the Mac,
// shell/photos_backup_mac.mm; a fake in the tests), the copies are
// io/verified_copy (hashed while read, read back, compared: the Import
// add-on's rule, plan/18), and the layout is Import's default,
// <destination>/YYYY/YYYY-MM-DD/<original file name>.
//
// What is backed up: every asset's ORIGINAL files -- the photo or video as
// shot, a Live Photo's paired video, a RAW+JPEG pair's RAW. Not Photos' edited
// renditions (an edit is non-destructive; the original is what a backup
// keeps) and not albums or metadata Photos holds outside the file.
//
// Resumable and idempotent: a manifest beside the files
// (<destination>/.mediaviewer-photos-backup/manifest.sqlite) records every
// file copied and verified, so a run after the first copies only what is new
// or missing from the destination (a file the user deleted there is copied
// again), and a cancelled run picks up where it stopped. Zero bytes are
// written for an asset already backed up.
//
// The originals of an iCloud-only asset are fetched from iCloud for this (the
// user asked for a copy of them: plan/12 2026-10-03), into a scratch folder,
// then copied verified to the destination and removed. The index and the
// viewer's rules are not changed by this.
//
// One worker thread of its own, started by start(); cancel() stops it
// between files or mid-copy (verified_copy leaves nothing behind). Nothing
// here runs on the UI thread (rule 1); no path or name is logged (rule 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/result.h"

namespace mv::shell::backup {

enum class file_kind : std::uint8_t {
  original = 0,      // the photo or video as shot (a RAW+JPEG pair's JPEG)
  paired_video = 1,  // a Live Photo's video
  alternate = 2,     // a RAW+JPEG pair's RAW
};

struct asset_file {
  std::string id;           // the asset's identifier, opaque here
  file_kind kind = file_kind::original;
  std::string filename;     // the original file name
  std::int64_t created_unix = 0;  // the asset's creation date: the layout's day
};

class source {
 public:
  virtual ~source() = default;
  // [worker] Every file to back up. `cancel` stops a long enumeration.
  [[nodiscard]] virtual result<std::vector<asset_file>> enumerate(const std::atomic<bool>* cancel) = 0;
  // [worker] A file on this machine to copy from, in place; "" when the bytes
  // must be fetched. Never written.
  [[nodiscard]] virtual result<std::string> local_file(const asset_file& file) = 0;
  // [worker] Fetches the file's bytes to `tmp_utf8` (the whole file; a cancel
  // or a failure leaves nothing there).
  [[nodiscard]] virtual expected fetch(const asset_file& file, const std::string& tmp_utf8,
                                       const std::atomic<bool>* cancel) = 0;
};

struct options {
  std::string destination;  // the folder the user chose
  std::string scratch;      // where fetched originals wait for their verified copy
};

enum class run_state : std::uint8_t { idle = 0, listing = 1, copying = 2, done = 3, cancelled = 4, failed = 5 };

struct progress {
  run_state state = run_state::idle;
  std::uint64_t total = 0;    // files to consider (after listing)
  std::uint64_t done = 0;     // copied and verified this run
  std::uint64_t skipped = 0;  // already in the backup
  std::uint64_t failed = 0;
  std::uint64_t fetched = 0;  // of `done`, fetched from the cloud first
  std::uint64_t bytes = 0;    // copied this run
  std::int64_t started_unix = 0;
  std::int64_t finished_unix = 0;
  std::string current;        // the file being copied (the user's own UI)
  std::string error;          // why state is `failed` (no path)
};

// Import's default layout: <destination>/YYYY/YYYY-MM-DD/<filename>, the day
// in local time. Pure.
[[nodiscard]] std::string layout_dir(std::string_view destination, std::int64_t created_unix);
// The manifest and report live here, beside the files.
[[nodiscard]] std::string housekeeping_dir(std::string_view destination);

class engine {
 public:
  engine();
  ~engine();
  engine(const engine&) = delete;
  engine& operator=(const engine&) = delete;

  // Starts the run on its own thread. invalid_arg when one is running.
  [[nodiscard]] expected start(std::unique_ptr<source> src, options opts);
  // Stops between files or mid-copy; the thread ends soon after.
  void cancel() noexcept;
  // Waits for the thread (quit; tests).
  void join() noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] progress snapshot() const;

 private:
  void run();

  std::unique_ptr<source> source_;
  options options_;
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  progress progress_;
};

}  // namespace mv::shell::backup
