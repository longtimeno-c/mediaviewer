// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Backing up a Photos library's originals to a folder (docs/design/26-photos-library.md
// "Backup"; owner, 2026-10-03: "backup all my icloud photos to a local
// directory"). Portable: the library is behind `source` (PhotoKit on the Mac,
// shell/photos_backup_mac.mm; a fake in the tests), the copies are
// io/verified_copy (hashed while read, read back, compared: the Import
// add-on's rule, docs/design/18), and the layout is the user's choice
// (`layout`, issue #414): Import's default <destination>/YYYY/YYYY-MM-DD/<original
// file name> unless they picked a year folder, YYYY/MM/DD, a day folder, or flat.
//
// What is backed up: every asset's ORIGINAL files -- the photo or video as
// shot, a Live Photo's paired video, a RAW+JPEG pair's RAW. Not Photos' edited
// renditions (an edit is non-destructive; the original is what a backup
// keeps) and not albums or metadata Photos holds outside the file.
//
// Resumable and idempotent: a manifest beside the files
// (<destination>/.mediaviewer-photos-backup/manifest.sqlite) records every
// file copied and verified, so a run after the first copies only what is new
// (a file someone deleted from the destination is held until they choose Copy
// again or Leave deleted: decide_deleted), and a cancelled run picks up where it stopped. Zero bytes are
// written for an asset already backed up.
//
// The originals of an iCloud-only asset are fetched from iCloud for this (the
// user asked for a copy of them: docs/design/12 2026-10-03), into a scratch folder,
// then copied verified to the destination and removed. The index and the
// viewer's rules are not changed by this.
//
// One worker thread of its own, started by start(); cancel() stops it
// between files or mid-copy (verified_copy leaves nothing behind). Nothing
// here runs on the UI thread (rule 1); no path or name is logged (rule 6).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/result.h"
#include "io/content_hash.h"

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
  // The file's size as the library reports it without reading it (PhotoKit's
  // resource size, iCloud-only files included); 0 when it does not say. Only
  // a hint for Check library: a match is always decided on the bytes.
  std::uint64_t size = 0;
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

// The folders a new file is written into under the destination, from the
// asset's creation date in local time. Files already verified stay where the
// manifest says they are: a layout change moves nothing. Persisted by the Mac
// host as a number, so the values do not change.
enum class layout : std::uint8_t {
  year_day = 0,        // YYYY/YYYY-MM-DD (the default; Import's default)
  year = 1,            // YYYY
  year_month_day = 2,  // YYYY/MM/DD
  day = 3,             // YYYY-MM-DD
  flat = 4,            // the destination itself
  year_month = 5,      // YYYY/MM
};
inline constexpr std::uint8_t kLayoutCount = 6;

// ---- Check library (docs/plans/check-library.md) ---------------------------
// What the destination library already holds, by content (size, then
// BLAKE3-256; never the name). The index is the Import add-on's (import.db,
// docs/design/18 "Duplicate test"), reached through mv.import.1 on the Mac
// (photos_backup_library.h); a fake in the tests.

// Where to look: the destination and its subfolders, or every library
// folder Import's index knows (each where it is mounted now). Persisted by
// the Mac host as a number.
enum class library_scope : std::uint8_t { destination = 0, library = 1 };
inline constexpr std::uint8_t kLibraryScopeCount = 2;

struct library_expected {
  std::string rel;  // the manifest's place under the destination, '/'-separated
  std::uint64_t size = 0;
  std::string hash_hex;
};
struct library_check_input {
  std::string destination;
  library_scope scope = library_scope::destination;
  std::vector<std::uint64_t> sizes;        // of the source files (0s left out)
  std::vector<library_expected> expected;  // the manifest: what should be there
};
struct library_gone {
  std::string rel;
  std::string moved_to;  // absolute; empty: the bytes are nowhere in scope
};
struct library_group {
  std::uint64_t size = 0;
  std::vector<std::string> paths;
};
struct library_check_output {
  bool reachable = false;
  bool walk_complete = false;
  std::uint64_t library_files = 0;
  std::uint32_t hashed = 0;       // library files read by this check
  std::uint32_t remembered = 0;   // already in the index
  std::uint64_t hashed_bytes = 0;
  std::vector<std::uint64_t> matched_sizes;  // sorted: sizes the library has in scope
  std::vector<library_gone> gone;            // of `expected`, after a complete walk only
  std::vector<library_group> duplicates;     // inside the destination
};
struct library_progress {
  std::uint64_t walked = 0;      // library files listed so far
  std::uint64_t bytes_done = 0;  // hashed, of
  std::uint64_t bytes_total = 0;
};

class library {
 public:
  virtual ~library() = default;
  // [worker] Indexes the destination for these sizes and answers the lists.
  // Blocks until done; `cancel` stops it. An error: no index (Import not
  // there or busy); the caller goes on without it.
  [[nodiscard]] virtual result<library_check_output> check(
      const library_check_input& in, const std::atomic<bool>* cancel,
      const std::function<void(const library_progress&)>& on_progress) = 0;
  // [worker] Every library file in scope with these bytes, confirmed on disk.
  [[nodiscard]] virtual result<std::vector<std::string>> find(std::string_view destination,
                                                              library_scope scope, std::uint64_t size,
                                                              const io::content_hash& hash) = 0;
  // [worker] A file this backup just wrote and verified joins the index.
  virtual void record(std::string_view destination, const std::string& path, std::uint64_t size,
                      std::int64_t mtime, const io::content_hash& hash) = 0;
};

struct options {
  std::string destination;  // the folder the user chose
  std::string scratch;      // where fetched originals wait for their verified copy
  layout folders = layout::year_day;
  // With an index: an asset whose bytes are already in the library (at
  // `scope`) is not copied; the manifest records it as found. Without one
  // nothing is looked up in the library. Either way a file deleted from the
  // folder after it was backed up is held (owner, 2026-11-14).
  std::shared_ptr<library> index{};
  library_scope scope = library_scope::destination;
  // Check library: compare and report; nothing is copied or fetched.
  bool check_only = false;
};

enum class run_state : std::uint8_t {
  idle = 0, listing = 1, copying = 2, done = 3, cancelled = 4, failed = 5,
  checking = 6,  // the library side of a check (walking, hashing)
};

struct progress {
  run_state state = run_state::idle;
  std::uint64_t total = 0;    // files to consider (after listing)
  std::uint64_t done = 0;     // copied and verified this run
  std::uint64_t skipped = 0;  // already in the backup
  std::uint64_t failed = 0;
  std::uint64_t fetched = 0;  // of `done`, fetched from the cloud first
  std::uint64_t found = 0;    // already in the library (by content), not copied
  // Deleted on purpose (docs/plans/check-library.md §2.3, owner 2026-11-14):
  // backed up, then removed from the folder, not copied again until someone
  // chooses Copy again in Check library.
  std::uint64_t held = 0;
  std::uint64_t left_deleted = 0;  // someone chose Leave deleted for these bytes
  std::uint64_t bytes = 0;    // copied this run (checking: library bytes hashed)
  std::uint64_t bytes_total = 0;  // checking: library bytes to hash
  std::int64_t started_unix = 0;
  std::int64_t finished_unix = 0;
  std::string current;        // the file being copied (the user's own UI)
  std::string error;          // why state is `failed` (no path)
};

// Check library's answer for a Photos source (check_only runs).
struct check_report {
  bool index = false;        // Import's index was there and answered
  bool reachable = false;    // the destination answered
  std::uint64_t originals = 0;
  std::uint64_t in_backup = 0;       // in the manifest and where it says
  struct found_file {
    std::string name;
    std::vector<std::string> where;
  };
  std::vector<found_file> in_library;  // not in the manifest, bytes in the library
  std::uint64_t to_copy = 0;           // would be copied
  std::uint64_t to_copy_bytes = 0;     // of those with a known size
  std::uint64_t unconfirmed = 0;       // in iCloud only, a size matches: checked when backed up
  std::vector<library_gone> gone;      // the manifest's files no longer there (moved ones included)
  // Backed up, then removed from the folder: held until someone chooses.
  // `left`: someone chose Leave deleted (remembered by hash).
  struct deleted_file {
    std::string asset;
    file_kind kind = file_kind::original;
    std::string name;
    std::string place;  // where it was, under the destination (or absolute, a found file elsewhere)
    std::uint64_t bytes = 0;
    bool left = false;
  };
  std::vector<deleted_file> deleted;
  std::vector<library_group> duplicates;
  std::uint64_t library_files = 0;
  std::uint32_t hashed = 0;
  std::uint32_t remembered = 0;
};
// The report as JSON for Settings: counts and every list.
[[nodiscard]] std::string check_report_json(const check_report& r);

// Deleted on purpose (docs/plans/check-library.md §2.3): what someone chose
// for a file the folder no longer holds. Copy again forgets the manifest row
// (and any Leave deleted for its bytes), so the next Back Up Now copies it;
// Leave deleted remembers its hash, so those bytes are never copied to this
// destination again, under any asset or name, until Copy again.
struct deleted_choice {
  std::string asset;
  file_kind kind = file_kind::original;
  bool copy_again = false;
};
// [worker] Writes the destination's manifest (it may be on a share). Not
// while a run is going (the caller checks). invalid_arg for an unknown file.
[[nodiscard]] expected decide_deleted(std::string_view destination, const std::vector<deleted_choice>& choices);

// The folder under the destination a new file goes into, '/'-separated, ""
// for flat ("2024/2024-05-17" for year_day); the date in local time. Pure.
[[nodiscard]] std::string layout_subdir(std::int64_t created_unix, layout folders);
// <destination>/<layout_subdir>. Pure.
[[nodiscard]] std::string layout_dir(std::string_view destination, std::int64_t created_unix,
                                     layout folders = layout::year_day);
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
  // The last check_only run's report (empty until one finishes).
  [[nodiscard]] check_report last_check() const;

 private:
  void run();

  std::unique_ptr<source> source_;
  options options_;
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  progress progress_;
  check_report check_;
};

}  // namespace mv::shell::backup
