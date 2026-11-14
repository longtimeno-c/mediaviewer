// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup, release 1.1 (docs/plans/mobile.md §10b, M11): the iPhone's
// photos and videos copied to a backup folder on the NAS, while the app is open.
//
// The phone keeps a ledger, one row per file it has seen confirmed: which asset
// (its PHCloudIdentifier where iCloud Photos gives one, else its local id),
// which resource, name, size, BLAKE3, and where it went. A photo counts as
// backed up only when every resource has a row AND the file is still in the
// folder, checked with one listing per folder at the start of each run, so a
// file deleted from the NAS is copied again. A row is added only after the
// service's confirmation (it hashed what reached its disk), so a run killed at
// any point never counts an unfinished copy. A copy of the ledger goes to the
// folder's meta folder every 50 files and at the end, for a reinstall or a new
// phone, which finds it by library id; restoring it only saves re-copying.
//
// The person's Photos come in through `source` (PhotoKit, in the host): one
// resource at a time is exported to a scratch file, hashed, sent and removed, so
// the phone never holds more than the largest single file. Files go out through
// `target`: the NAS service over the link, or in the sample library the
// clearly labelled "Sample NAS on this iPhone" folder (local_target), which
// writes through the service's own commit step (serve_backup.h) and never
// sends anything anywhere.
//
// Worker threads only: everything here blocks. Nothing is logged (rule 6).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/serve_client.h"
#include "core/result.h"
#include "mobile/link.h"

namespace mv::mobile::backup {

// How files are filed under the folder (§10b), from the photo's own capture date.
// The values are the desktop Photos backup's (shell/photos_backup.h, backup::layout).
enum class layout : std::uint8_t {
  year = 1,            // 2026/IMG_0412.HEIC
  year_month_day = 2,  // 2026/09/21/IMG_0412.HEIC
  flat = 4,            // IMG_0412.HEIC
  year_month = 5,      // 2026/09/IMG_0412.HEIC (the default)
};
[[nodiscard]] bool valid_layout(int value) noexcept;
// "2026/09" for year_month; "" for flat.
[[nodiscard]] std::string subdir(int year, int month, int day, layout l);

// ---- where files go -------------------------------------------------------------------------

struct folder {
  std::string id;
  std::string label;
  bool writable = false;
};

class target {
 public:
  virtual ~target() = default;
  // Backup folders only. status::unsupported_format: a service from before 1.1.
  [[nodiscard]] virtual result<std::vector<folder>> folders() = 0;
  [[nodiscard]] virtual result<driftwire::serve::stat_info> space(const std::string& root) = 0;
  [[nodiscard]] virtual result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                               const std::string& rel) = 0;
  [[nodiscard]] virtual result<driftwire::serve::client::write_done> write(
      const std::string& root, const std::string& rel, std::uint64_t size, const std::string& hash_hex,
      std::uint64_t at, const driftwire::serve::client::read_fn& read, const std::function<void(std::uint64_t)>& progress,
      const cancel_flag* cancel, driftwire::serve::client::write_refused* why, bool meta) = 0;
  // The meta folder: each device's folder id, and a ledger copy's text.
  [[nodiscard]] virtual result<std::vector<std::string>> ledger_ids(const std::string& root) = 0;
  [[nodiscard]] virtual result<std::string> read_ledger(const std::string& root, const std::string& id) = 0;
  // This phone's folder id (serve::device_folder_id).
  [[nodiscard]] virtual std::string device_id() const = 0;
};

[[nodiscard]] std::unique_ptr<target> link_target(link_session& link);

// The sample library's "NAS": a folder on this phone, never sent anywhere.
inline constexpr const char* kSampleFolderId = "5a3b1e0f5a3b1e0f";
inline constexpr const char* kSampleFolderLabel = "Sample NAS on this iPhone";
inline constexpr std::uint64_t kSampleLimit = 250ull * 1024 * 1024;
[[nodiscard]] std::unique_ptr<target> local_target(const std::string& folder_path, const std::string& device_id,
                                                   std::uint64_t limit = kSampleLimit,
                                                   std::function<std::uint64_t(const std::string&)> free_bytes = {});

// ---- what to copy ---------------------------------------------------------------------------

// One file of a photo: the photo itself, a Live Photo's video, a RAW, a video.
struct resource {
  std::string kind;  // "photo", "video", "live_video", "raw", "other"
  std::string name;  // the original file name, as shot
};

struct item {
  std::string key;       // PHCloudIdentifier, or "local:" + the local identifier
  std::string local_id;  // for the source
  int year = 0, month = 0, day = 0;  // the capture date where it was taken
  std::vector<resource> resources;
};

class source {
 public:
  virtual ~source() = default;
  // Writes resource `index` of `it`, the original as shot (downloaded first if
  // only iCloud has it), to `path`. status::not_found: it is gone from Photos.
  // `ymd` comes in as the item's date; the source may set the date it reads from
  // the file itself (the capture date where it was taken, §10b), which then files
  // this resource and the rest of the item.
  [[nodiscard]] virtual expected export_resource(const item& it, std::size_t index, const std::string& path,
                                                 const cancel_flag* cancel, int ymd[3]) = 0;
};

// ---- the ledger -----------------------------------------------------------------------------

struct row {
  std::string key, kind, name;
  std::uint64_t size = 0;
  std::string hash;
  std::string root;  // the folder's id
  std::string rel;   // where it is in the folder, as the service named it
  std::int64_t at = 0;
};

class ledger {
 public:
  // Application Support (excluded from iCloud backup by the host, §8.3).
  [[nodiscard]] static result<std::unique_ptr<ledger>> open(const std::string& path);
  ~ledger();
  ledger(const ledger&) = delete;
  ledger& operator=(const ledger&) = delete;

  // Rows are per backup folder: the same photo backed up to two folders has a row in each.
  [[nodiscard]] expected add(const row& r);
  // Many at once, in one transaction (a restore).
  [[nodiscard]] expected add_all(const std::vector<row>& rows);
  [[nodiscard]] expected remove(const std::string& root, const std::string& key, const std::string& kind,
                                const std::string& name);
  [[nodiscard]] std::vector<row> all(const std::string& root) const;
  [[nodiscard]] std::optional<row> find(const std::string& root, const std::string& key, const std::string& kind,
                                        const std::string& name) const;
  // A file sent under another key (a local id that changed): the same name, size and hash.
  [[nodiscard]] std::optional<row> find_same(const std::string& root, const std::string& name, std::uint64_t size,
                                             const std::string& hash) const;
  [[nodiscard]] std::size_t size(const std::string& root) const;
  // The copy on the NAS, of one folder's rows: {"v":1,"library_id","rows":[...]}.
  [[nodiscard]] std::string to_json(std::string_view library_id, const std::string& root) const;
  // Rows of a copy for this library; nullopt when it is not one. A row that could not be one
  // of ours (a path that escapes, a name that does not match where it went, a bad hash) is left out.
  [[nodiscard]] static std::optional<std::vector<row>> parse_json(std::string_view text, std::string_view library_id);
  [[nodiscard]] expected clear();

 private:
  ledger() = default;
  struct impl;
  std::unique_ptr<impl> impl_;
};

// ---- the run --------------------------------------------------------------------------------

enum class state : std::uint8_t {
  idle = 0,
  checking = 1,      // listing the folder and the ledger
  running = 2,
  done = 3,          // everything is backed up
  paused_wifi = 4,   // off Wi-Fi (the host says so; never over mobile data unless asked)
  not_allowed = 5,   // the folder is not one this phone may write to (§11.1)
  limit = 6,         // the folder's storage limit is reached
  full = 7,          // the NAS is full
  unreachable = 8,
  too_old = 9,       // the service is from before 1.1
  no_folder = 10,    // no backup folder chosen, or none offered
  no_access = 11,    // Photos access refused
  done_failures = 12,  // every file it could copy is backed up; some could not be (failures says which)
};

struct failure {
  std::string key;
  std::string name;
  std::string why;  // "gone", "export", "hash", "error"
};

struct status_report {
  state now = state::idle;
  std::uint64_t total = 0;    // photos and videos to consider
  std::uint64_t backed_up = 0;
  std::uint64_t files_done = 0;   // files in the ledger and in the folder
  std::uint64_t copied = 0;       // files copied this run
  std::uint64_t bytes = 0;        // sent this run
  std::string current;            // the file being copied (this phone's own screen only)
  double current_fraction = 0;
  std::vector<failure> failures;  // this run's
};

struct options {
  std::string library_id;
  std::string root;              // the backup folder's id
  layout folders = layout::year_month;
  std::string scratch_dir;       // one resource at a time is exported here
  int ledger_copy_every = 50;
};

class engine {
 public:
  engine(options o, target& t, source& s, ledger& l);
  // Newest first (§10b: recent photos are safe soonest).
  void set_items(std::vector<item> items);
  // One run, to the end or the first state that stops it. `report` is called
  // from this thread as things change.
  state run(const cancel_flag* cancel, const std::function<void(const status_report&)>& report);
  // Keys of the photos not backed up yet, as of the last run's check.
  [[nodiscard]] std::vector<std::string> remaining() const;

 private:
  struct present_set;
  [[nodiscard]] bool restore_if_empty();
  [[nodiscard]] result<present_set> check_folder();
  [[nodiscard]] bool item_done(const item& it) const;
  void copy_ledger();

  options o_;
  target& t_;
  source& s_;
  ledger& l_;
  std::vector<item> items_;
  mutable std::mutex m_;
  std::vector<std::string> remaining_;  // under m_
  status_report st_;
  bool restored_ = false;  // rows came from the NAS's copy this run: send ours back
};

}  // namespace mv::mobile::backup
