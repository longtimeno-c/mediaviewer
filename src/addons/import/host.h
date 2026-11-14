// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A C++ face on the host function table (mediaviewer_addon.h). Everything the
// add-on does to a file goes through here: it links no part of the core.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addons/import/model.h"
#include "core/result.h"

namespace mv::import {

struct copy_callbacks {
  std::function<bool()> cancelled;             // may be empty
  std::function<void(std::uint64_t)> progress; // may be empty
  std::function<void()> yield;                 // may be empty
};

struct copy_fault_request {
  int target = -1;
  std::uint32_t times = 0;
  std::uint64_t offset = 0;
};

class host {
 public:
  explicit host(const mv_host_api* api) noexcept : api_(api) {}

  [[nodiscard]] const mv_host_api* api() const noexcept { return api_; }

  [[nodiscard]] expected walk(const std::string& root, int max_depth,
                              const std::function<bool(const mv_addon_file_entry&)>& visit) const;
  struct stat_result {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool is_directory = false;
  };
  [[nodiscard]] result<stat_result> stat(const std::string& path) const;
  [[nodiscard]] expected make_directories(const std::string& dir) const;
  [[nodiscard]] expected remove_file(const std::string& path) const;
  [[nodiscard]] result<digest> hash(const std::string& path, bool uncached,
                                    const copy_callbacks& cb) const;
  [[nodiscard]] expected copy(const std::string& src, const std::vector<std::string>& targets,
                              bool read_back, const copy_callbacks& cb,
                              const copy_fault_request& fault, mv_addon_copy_result& out) const;
  [[nodiscard]] expected write_new_file(const std::string& path, std::string_view bytes) const;
  [[nodiscard]] result<mv_addon_volume> volume_of(const std::string& path) const;
  [[nodiscard]] result<std::vector<mv_addon_volume>> list_volumes() const;
  [[nodiscard]] expected eject(const std::string& root) const;
  [[nodiscard]] expected watch_volumes(void(MV_CALL* cb)(void*, std::uint32_t, const char*),
                                       void* user) const;

  [[nodiscard]] bool capture(const std::string& path, mv_addon_capture& out) const;
  // partner[i] (UINT32_MAX = none) and kind[i] for one directory's names.
  [[nodiscard]] expected pair(const std::vector<std::string>& names,
                              std::vector<std::uint32_t>& partner,
                              std::vector<std::uint32_t>& kind) const;
  [[nodiscard]] result<std::string> thumbnail(const std::string& path) const;

  // The Recycle Bin / Trash (find duplicates): absent on an older host.
  [[nodiscard]] bool can_recycle() const noexcept;
  // true: in the bin; false: the location has no bin, nothing was removed.
  [[nodiscard]] result<bool> recycle(const std::string& path) const;

  [[nodiscard]] bool should_yield() const noexcept;
  void post(mv_addon_event_kind kind, mv_status status, std::uint64_t id,
            std::int64_t payload) const noexcept;
  [[nodiscard]] result<std::string> data_dir() const;
  [[nodiscard]] result<std::string> default_library_dir() const;
  void log(int level, const char* ascii) const noexcept;

 private:
  const mv_host_api* api_;
};

[[nodiscard]] inline status to_status(mv_status s) noexcept { return static_cast<status>(s); }

// ---- where a destination is (docs/design/18 "Destination identity") ----

// A stored destination is "vol:<identity><US><folder inside the volume>", or
// an absolute path when no stable identity could be found for it.
[[nodiscard]] inline bool is_volume_key(std::string_view stored) noexcept {
  return stored.rfind("vol:", 0) == 0;
}

// One name for a network share however it was found: SMB and UNC names fold
// case, and the host loses its Bonjour service ("dev://me@NAS._smb._tcp.local/Photos"
// and "dev://me@nas.local/photos" are one share). The user stays: "home" is a
// different share for each of them.
[[nodiscard]] std::string share_identity(std::string_view device_key);

// A snapshot of what is mounted, taken once per plan or job (a volume list is
// not a per-file call). Worker and control threads only: it enumerates volumes.
// One thread at a time: live_root remembers its answers.
class dest_locator {
 public:
  explicit dest_locator(const host& h);

  // What import.db stores for the destination folder `root`. The folder need
  // not exist yet. A volume key is only issued if it finds `root` again, so a
  // folder on a mount the volume list cannot see keeps its absolute path.
  [[nodiscard]] std::string key_of(const std::string& root) const;
  // Where a stored destination is now; nullopt when its volume is not mounted,
  // or two different disks answer to it. Remembered per key, nullopt included.
  [[nodiscard]] std::optional<std::string> live_root(const std::string& stored) const;
  // Where the volume with this volume_id is mounted now (a card's source).
  [[nodiscard]] std::optional<std::string> volume_root(const std::string& volume_id) const;

 private:
  [[nodiscard]] std::vector<std::string> roots_for(const std::string& identity) const;
  [[nodiscard]] std::optional<std::string> resolve(const std::string& key) const;

  const host& h_;
  std::vector<mv_addon_volume> mounted_;
  mutable std::map<std::string, std::optional<std::string>> resolved_;
};

}  // namespace mv::import
