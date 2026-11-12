// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup's half of `driftwire serve` (docs/plans/mobile.md §10b, M11):
// what a write into a backup folder may do, and the files it makes. The wire
// is in serve_wire.h ("writes"); the session's stream handling in serve.cpp.
//
// The rules, all here so they are tested in one place:
//   - only a named folder marked backup, and only a device that may write;
//   - never past the folder's storage limit, never leaving the volume less
//     free than its reserve; both counted before a byte is taken;
//   - a file is written as `.<name>.<device id>.mvpart` beside where it goes
//     (hidden, so no index or person takes it for the photo), flushed, hashed
//     as it is on disk, and renamed into place without replacing anything;
//   - the only files ever removed or appended to are a device's own .mvpart;
//     the only file ever replaced is a device's own ledger copy (owner's
//     calls B3 and B4, 2026-11-12).
// Nothing here logs a path or a name (rule 6). Portable (D9).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "addons/driftwire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::serve::backup {

// The storage limit and the reserve, across every session of one service.
class book {
 public:
  // Whether this device may write into backup folders. Empty: nobody may.
  std::function<bool(const std::array<std::uint8_t, 32>&)> may_write;

  [[nodiscard]] bool allowed(const root_config& root, const std::array<std::uint8_t, 32>& device) const;

  // A backup folder's stat_info space fields (counts the folder the first time).
  void space(const root_config& root, stat_info& out);

  enum class verdict : std::uint8_t { ok, limit, full };
  // Holds `bytes` against the folder's limit and the volume's reserve.
  [[nodiscard]] verdict reserve(const root_config& root, std::uint64_t bytes);
  void wrote(const std::string& root_id, std::uint64_t bytes);     // held bytes, now on disk
  void release(const std::string& root_id, std::uint64_t bytes);  // held bytes never written
  void removed(const std::string& root_id, std::uint64_t bytes);   // bytes taken off the disk
  void added(const std::string& root_id, std::uint64_t bytes);     // bytes put on it without a hold

  // One writer per unfinished copy: a second stream for the same .mvpart is refused.
  [[nodiscard]] bool claim(const std::string& part);
  void drop(const std::string& part);

  // Tests: the volume's free bytes, instead of asking the OS.
  std::function<std::uint64_t(const std::string& path)> free_bytes;

 private:
  struct counted {
    bool known = false;
    std::uint64_t used = 0;
    std::uint64_t held = 0;
  };
  counted& count(const root_config& root, std::unique_lock<std::mutex>& lock);
  [[nodiscard]] std::uint64_t free_of(const std::string& path) const;
  std::mutex m_;
  std::map<std::string, counted> roots_;  // under m_
  std::set<std::string> parts_;           // under m_
};

// "IMG_0412.HEIC", 2 -> "IMG_0412 (2).HEIC"; 1 is the name itself.
[[nodiscard]] std::string numbered(std::string_view name, int n);
// The unfinished copy's name for `name`, from device `device_id`.
[[nodiscard]] std::string part_name(std::string_view name, std::string_view device_id);
// BLAKE3 of the file as it is on disk, 64 hex digits.
[[nodiscard]] result<std::string> hash_file(const std::string& path);
// Whether `path` is a regular file of `size` bytes whose hash is `hash`.
[[nodiscard]] bool same_file(const std::string& path, std::uint64_t size, std::string_view hash);
// Names tried for a file before the write is refused (" (2)" ... " (99)").
inline constexpr int kMaxNumbered = 99;

}  // namespace mv::driftwire::serve::backup
