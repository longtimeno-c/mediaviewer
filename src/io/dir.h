// SPDX-License-Identifier: GPL-2.0-or-later
// Portable directory listing and watch. Windows impl is dir_win.cpp (D9).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

struct dir_entry {
  std::string name_utf8;
  std::string path_utf8;
  std::uint64_t size = 0;
  std::int64_t mtime_unix = 0;
};

// The decodable set: JPEG / PNG (incl. APNG) / BMP, GIF and WebP (PR 6), the
// PR 7 camera-dump stills (TIFF, ICO, HEIC/HEIF, AVIF, RAW), plus the PR 5
// video containers. The listing filters by extension; decode still probes
// magic bytes. Sorted by name, case-insensitive. Pairing (RAW+JPEG, Live
// Photo) and companion hiding happen after the scan (plan/04).
[[nodiscard]] result<std::vector<dir_entry>> list_still_files(std::string_view utf8_dir);

// A direct child directory (plan/10 PR 26, multi-folder browsing).
struct subdir_entry {
  std::string name_utf8;
  std::string path_utf8;
  std::int64_t mtime_unix = 0;
};

// Platform primitive: the direct child directories of `utf8_dir`, hidden and
// system ones already dropped, in no particular order. Use list_subfolders().
[[nodiscard]] result<std::vector<subdir_entry>> scan_subdirs(std::string_view utf8_dir);

// Child directories worth showing as folder tiles, in natural order ("Trip 2"
// before "Trip 10", "2019" before "2020"). Drops NAS / OS housekeeping folders
// (Synology "@eaDir", "#recycle", "$RECYCLE.BIN", ...). Portable (dir_tree.cpp).
[[nodiscard]] result<std::vector<subdir_entry>> list_subfolders(std::string_view utf8_dir);

// Case-insensitive ASCII compare where digit runs compare by value.
[[nodiscard]] bool less_natural(std::string_view a, std::string_view b) noexcept;

// True for folder names that are housekeeping, not media ("@eaDir", "#recycle").
[[nodiscard]] bool is_housekeeping_dir(std::string_view name) noexcept;

// What a folder tile shows. Bounded work: it lists the folder itself, and only
// if that holds no media walks down at most `max_depth` levels / `max_visits`
// directories for a cover, so a folder tile over a NAS never turns into a
// crawl. Runs on a worker (rule 1); never call it from the UI or render thread.
struct folder_summary {
  std::uint32_t media_count = 0;    // media files directly in the folder
  std::uint32_t subdir_count = 0;   // direct child folders
  bool has_cover = false;
  // True when `cover` came from a descendant: this folder's own listing has no
  // media, but a photo was found further down. A tile uses it to say "photos
  // inside" without claiming a total.
  bool photos_inside = false;
  // The bounded walk stopped before it could decide. No cover in that case
  // means "not fully looked", not "folders only".
  bool search_incomplete = false;
  dir_entry cover;                  // first media file, here or in a descendant
};
[[nodiscard]] result<folder_summary> summarize_dir(std::string_view utf8_dir, int max_depth = 3,
                                                   int max_visits = 48);

[[nodiscard]] result<bool> is_directory(std::string_view utf8_path);

struct subdir {
  std::string name_utf8;
  std::string path_utf8;
};

// The immediate subdirectories of `utf8_dir` for the folder tree (PR 9): hidden
// and package directories skipped, sorted case-insensitively by name. One
// directory read, no recursion, so a tree node expands in one worker job.
// Worker threads only. `status::io` if the directory cannot be opened.
[[nodiscard]] result<std::vector<subdir>> list_subdirectories(std::string_view utf8_dir);

// If `utf8_path` is a directory, returns it. If it is a file, returns the parent.
[[nodiscard]] result<std::string> containing_dir(std::string_view utf8_path);

class directory_watcher {
 public:
  using callback = void (*)(void* user);

  directory_watcher();
  ~directory_watcher();

  directory_watcher(const directory_watcher&) = delete;
  directory_watcher& operator=(const directory_watcher&) = delete;

  // `cb` runs on the watch thread. It must not block and must not re-enter
  // the watcher. Submit a job from it.
  //
  // start() and stop() are safe to call concurrently from different threads:
  // the ABI stops a watcher on the UI thread while a pool thread may still be
  // starting one for a folder the user has already navigated away from.
  [[nodiscard]] expected start(std::string_view utf8_dir, callback cb, void* user);
  void stop() noexcept;

 private:
  struct impl;

  // Precondition: `mutex_` is held.
  void stop_locked() noexcept;

  std::mutex mutex_;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::io
