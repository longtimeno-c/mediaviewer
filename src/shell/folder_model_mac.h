// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18 (folded-in PR 4, plan/12 2026-09-17): the backend a SwiftUI
// filmstrip/gallery consumes. Folder listing + FSEvents watch (io/dir_mac.cpp)
// tied to the JPEG-512 thumbnail cache (image/thumb_mac.cpp). No UI, no key
// router, no marks/copy-to/Trash/drag-drop, no RAW+JPEG/Live Photo pairing —
// those are the SwiftUI-side follow-up's job.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "image/thumb.h"
#include "io/dir.h"

namespace mv::shell {

class folder_model {
 public:
  folder_model() = default;
  ~folder_model();

  folder_model(const folder_model&) = delete;
  folder_model& operator=(const folder_model&) = delete;

  // Lists `dir_utf8`, opens its thumbnail cache, and starts watching it.
  // Safe to call again with a different directory — tears down the old one
  // first. `jobs` must outlive this object; thumb and relist work runs on it.
  [[nodiscard]] expected open(std::string_view dir_utf8, job_system& jobs) noexcept;
  void close() noexcept;

  // A snapshot of the current listing. Copies under the lock — callers are
  // expected to poll this at UI-refresh cadence, not per frame; a filmstrip
  // holding thousands of items should diff by name/mtime rather than take
  // this on every draw.
  [[nodiscard]] std::vector<io::dir_entry> items() const;
  [[nodiscard]] std::size_t item_count() const noexcept;

  // True once the watcher has fired and the relist has completed since the
  // last call. Callers poll this (e.g. once per UI tick) to know a redraw
  // of the filmstrip/gallery is due.
  [[nodiscard]] bool consume_changed() noexcept;

  using thumb_ready_fn = std::function<void(std::string path_utf8, std::string thumb_path)>;

  // Looks up (or generates and caches) the thumbnail for `path_utf8`/
  // `mtime_unix`/`size`. `on_ready` runs on the pool thread that produced the
  // result (job_system's contract, plan/02) — never the calling thread — with
  // an empty `thumb_path` on failure. The caller marshals to its own thread.
  void request_thumb(std::string path_utf8, std::int64_t mtime_unix, std::uint64_t size,
                     thumb_ready_fn on_ready);

 private:
  static void watch_callback(void* user) noexcept;
  void relist_async();

  mutable std::mutex mutex_;
  std::string dir_;
  std::vector<io::dir_entry> items_;
  image::thumb_store thumbs_;
  io::directory_watcher watcher_;
  job_system* jobs_ = nullptr;
  std::atomic<bool> changed_{false};
};

}  // namespace mv::shell
