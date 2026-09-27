// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 18 (folded-in PR 4, plan/12 2026-09-17): the backend a SwiftUI
// filmstrip/gallery consumes. Folder listing + FSEvents watch (io/dir_mac.cpp)
// tied to the JPEG-512 thumbnail cache (image/thumb_mac.cpp). No UI, no key
// router, no marks/copy-to/Trash/drag-drop, no RAW+JPEG/Live Photo pairing —
// those are the SwiftUI-side follow-up's job.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
  folder_model();
  ~folder_model();

  folder_model(const folder_model&) = delete;
  folder_model& operator=(const folder_model&) = delete;

  // Lists `dir_utf8`, opens its thumbnail cache, and starts watching it.
  // Safe to call again with a different directory — tears down the old one
  // first. `jobs` must outlive this object; relist/thumb jobs run on it, but
  // — see the shared_state note below — a job in flight when this object is
  // destroyed does not touch a dangling `this`.
  [[nodiscard]] expected open(std::string_view dir_utf8, job_system& jobs) noexcept;
  void close() noexcept;

  // A snapshot of the current listing. Copies under the lock — callers are
  // expected to poll this at UI-refresh cadence, not per frame; a filmstrip
  // holding thousands of items should diff by name/mtime rather than take
  // this on every draw.
  [[nodiscard]] std::vector<io::dir_entry> items() const;
  [[nodiscard]] std::size_t item_count() const noexcept;

  // Child folders of the open directory (plan/10 PR 26), natural order,
  // refreshed by the same relist that refreshes items().
  [[nodiscard]] std::vector<io::subdir_entry> subfolders() const;
  [[nodiscard]] std::string directory() const;

  using summary_ready_fn = std::function<void(std::string dir_utf8, bool ok, io::folder_summary)>;

  // Counts and picks a cover for a folder tile on a pool thread (io::summarize_dir
  // is bounded, but it is still directory I/O). `on_ready` runs on that thread;
  // `ok` is false on failure or when the model moved to another folder first.
  void request_summary(std::string dir_utf8, summary_ready_fn on_ready);

  // True once the watcher has fired and the relist has completed since the
  // last call. Callers poll this (e.g. once per UI tick) to know a redraw
  // of the filmstrip/gallery is due.
  [[nodiscard]] bool consume_changed() noexcept;

  using thumb_ready_fn = std::function<void(std::string path_utf8, std::string thumb_path)>;

  // Looks up (or generates and caches) the thumbnail for `path_utf8`/
  // `mtime_unix`/`size`. `on_ready` runs on the pool thread that produced the
  // result (job_system's contract, plan/02) — never the calling thread — with
  // an empty `thumb_path` on failure. The caller marshals to its own thread.
  // Safe to call after this object is later destroyed while the job is still
  // in flight: `on_ready` still fires (job_system's contract), operating on
  // the shared cache state, which the job keeps alive.
  void request_thumb(std::string path_utf8, std::int64_t mtime_unix, std::uint64_t size,
                     thumb_ready_fn on_ready);

 private:
  // job_system's queue can outlive this object (jobs already submitted when
  // close()/~folder_model() runs are not retracted, only not-yet-started ones
  // are — job_system.h). A job lambda captures shared_state by shared_ptr,
  // not `this`, so it keeps the cache/listing alive for its own duration
  // instead of touching a folder_model that may already be gone.
  struct shared_state {
    std::mutex mutex;
    std::string dir;
    std::vector<io::dir_entry> items;
    std::vector<io::subdir_entry> subdirs;
    image::thumb_store thumbs;
    std::atomic<bool> changed{false};
    // Bumped by every open(): `thumbs` is one mutable object re-pointed at a
    // new directory's cache DB on each open(), so a request_thumb() job
    // queued for the old directory has no other way to tell, once it finally
    // runs, that `thumbs` no longer means what it did when the job was
    // requested (it would otherwise mis-associate an old-directory path
    // with the new directory's cache database).
    std::atomic<std::uint64_t> generation{0};
  };

  static void watch_callback(void* user) noexcept;
  void relist_async();

  std::shared_ptr<shared_state> state_;
  io::directory_watcher watcher_;
  job_system* jobs_ = nullptr;
};

}  // namespace mv::shell
