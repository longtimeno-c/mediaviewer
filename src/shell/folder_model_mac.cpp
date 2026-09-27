// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/folder_model_mac.h"

#include <utility>

#include "io/file.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "player/poster.h"
#include "shell/media_kind.h"

namespace mv::shell {

folder_model::folder_model() : state_(std::make_shared<shared_state>()) {}

folder_model::~folder_model() { close(); }

expected folder_model::open(std::string_view dir_utf8, job_system& jobs) noexcept {
  if (dir_utf8.empty()) return err(status::invalid_arg);
  jobs_ = &jobs;

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->dir.assign(dir_utf8);
    // A result list's items are not this folder's: never let a relist that
    // has not landed yet be read as them.
    if (state_->is_list) state_->items.clear();
    state_->is_list = false;
    state_->list_title.clear();
    state_->moments.clear();
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
  }

  // The cache lives in ~/Library/Caches, not in dir_utf8: the browsed folder
  // is the user's, and thumbnails written there would appear in its own
  // listing (and be thumbnailed in turn).
  auto cache = io::thumb_cache_dir();
  if (!cache) return err(cache.error());
  if (auto opened = state_->thumbs.open(cache.value()); !opened) return opened;
  if (auto started = watcher_.start(dir_utf8, &folder_model::watch_callback, this); !started) {
    state_->thumbs.close();
    return started;
  }

  // List here, on the worker open() already runs on (the host never calls it
  // on the UI thread), rather than queueing a second job behind the thumbnail
  // sweep. The watcher is already running, so nothing added from here on is
  // missed.
  (void)relist_now(*state_, std::string(dir_utf8));
  return {};
}

expected folder_model::open_list(std::string title_utf8, std::vector<list_entry> entries,
                                 job_system& jobs) noexcept {
  jobs_ = &jobs;
  // Nothing to watch: a result list changes only when the user searches again.
  watcher_.stop();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->dir.clear();  // a queued relist of the old folder now drops itself
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
  }
  auto cache = io::thumb_cache_dir();
  if (!cache) return err(cache.error());
  if (!state_->thumbs.is_open()) {
    if (auto opened = state_->thumbs.open(cache.value()); !opened) return opened;
  }
  // One stat per result: size and mtime key the thumbnail cache. A file
  // deleted or moved since it was indexed simply drops out.
  std::vector<io::dir_entry> items;
  std::vector<std::int64_t> moments;
  items.reserve(entries.size());
  moments.reserve(entries.size());
  for (list_entry& e : entries) {
    auto st = io::stat_path(e.path_utf8);
    if (!st || st.value().is_directory) continue;
    io::dir_entry d;
    d.name_utf8 = std::string(io::file_name_of(e.path_utf8));
    d.size = st.value().size;
    d.mtime_unix = st.value().mtime_unix;
    d.path_utf8 = std::move(e.path_utf8);
    items.push_back(std::move(d));
    moments.push_back(e.moment_ms);
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->is_list = true;
    state_->list_title = std::move(title_utf8);
    state_->items = std::move(items);
    state_->moments = std::move(moments);
    state_->subdirs.clear();
  }
  state_->changed.store(true, std::memory_order_release);
  if (state_->notify) state_->notify(state_->notify_user);
  return {};
}

folder_model::listing folder_model::snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  listing out;
  out.items = state_->items;
  out.subdirs = state_->subdirs;
  out.is_list = state_->is_list;
  if (state_->is_list) {
    out.moments = state_->moments;
    out.title = state_->list_title;
  }
  out.dir = state_->dir;
  return out;
}

void folder_model::set_changed_notify(changed_fn fn, void* user) noexcept {
  state_->notify = fn;
  state_->notify_user = user;
}

status folder_model::relist_now(shared_state& state, const std::string& dir) {
  auto listed = io::list_still_files(dir);
  if (!listed) return listed.error();
  // A folder whose subfolders cannot be read still shows its files: the
  // tiles are additive.
  std::vector<io::subdir_entry> subdirs;
  if (auto subs = io::list_subfolders(dir)) subdirs = std::move(subs).value();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    // The directory may have changed again (or closed) while this ran.
    if (state.dir != dir) return status::cancelled;
    state.items = std::move(listed).value();
    state.subdirs = std::move(subdirs);
  }
  state.changed.store(true, std::memory_order_release);
  if (state.notify) state.notify(state.notify_user);
  return status::ok;
}

void folder_model::close() noexcept {
  // Stops and joins the FSEvents watch thread first, so watch_callback (which
  // captures `this`, not shared_state) cannot fire again after this point.
  watcher_.stop();
  state_->thumbs.close();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->dir.clear();
    state_->items.clear();
    state_->subdirs.clear();
    state_->moments.clear();
    state_->list_title.clear();
    state_->is_list = false;
  }
  // Jobs already submitted to `jobs_` (relist/thumb) keep their own
  // std::shared_ptr<shared_state> and finish safely against it; this object
  // is free to be destroyed without waiting for them.
}

std::vector<io::dir_entry> folder_model::items() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->items;
}

std::vector<io::subdir_entry> folder_model::subfolders() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->subdirs;
}

std::string folder_model::directory() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->dir;
}

std::size_t folder_model::item_count() const noexcept {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->items.size();
}

bool folder_model::consume_changed() noexcept {
  return state_->changed.exchange(false, std::memory_order_acq_rel);
}

void folder_model::watch_callback(void* user) noexcept {
  static_cast<folder_model*>(user)->relist_async();
}

void folder_model::relist_async() {
  if (!jobs_) return;
  std::string dir_copy;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    dir_copy = state_->dir;
  }
  if (dir_copy.empty()) return;

  // Background generation (core/job_system.h): a relist triggered by a
  // filesystem event is not tied to the current view intent and must not be
  // abandoned just because the user arrowed to the next photo mid-scan.
  jobs_->submit_at(background_generation,
                   [state = state_, dir_copy](const job_context&) -> status {
                     return relist_now(*state, dir_copy);
                   });
}

void folder_model::request_summary(std::string dir_utf8, summary_ready_fn on_ready) {
  if (!jobs_) {
    if (on_ready) on_ready(std::move(dir_utf8), false, {});
    return;
  }
  // Same staleness rule as request_thumb: a tile asked for in one folder must
  // not report into the next one the user has already navigated to.
  const std::uint64_t requested_generation = state_->generation.load(std::memory_order_acquire);
  jobs_->submit_at(background_generation,
                   [state = state_, dir = std::move(dir_utf8), on_ready = std::move(on_ready),
                    requested_generation](const job_context&) -> status {
                     if (state->generation.load(std::memory_order_acquire) != requested_generation) {
                       if (on_ready) on_ready(dir, false, {});
                       return status::cancelled;
                     }
                     auto summary = io::summarize_dir(dir);
                     if (!summary) {
                       if (on_ready) on_ready(dir, false, {});
                       return summary.error();
                     }
                     if (on_ready) on_ready(dir, true, std::move(summary).value());
                     return status::ok;
                   });
}

void folder_model::request_thumb(std::string path_utf8, std::int64_t mtime_unix,
                                 std::uint64_t size, thumb_ready_fn on_ready) {
  if (!jobs_) {
    if (on_ready) on_ready(std::move(path_utf8), {});
    return;
  }

  // `thumbs` is one mutable object that open() re-points at a new directory's
  // cache DB on every call — a generation captured now and rechecked in the
  // job (see shared_state::generation) is what stops a job queued for the
  // directory open() when request_thumb() was called from mis-associating
  // its path with whatever directory's cache happens to be open by the time
  // the job actually runs.
  const std::uint64_t requested_generation = state_->generation.load(std::memory_order_acquire);

  // Captures `state_` (shared_ptr), never `this` — see the shared_state note
  // in folder_model_mac.h. The job outlives this folder_model if close()/the
  // destructor runs before it starts or finishes.
  jobs_->submit_at(background_generation,
                   [state = state_, path = std::move(path_utf8), mtime_unix, size,
                    on_ready = std::move(on_ready), requested_generation](
                       const job_context& ctx) -> status {
                     const auto stale = [&] {
                       return state->generation.load(std::memory_order_acquire) !=
                              requested_generation;
                     };
                     if (stale()) {
                       if (on_ready) on_ready(path, {});
                       return status::cancelled;
                     }

                     const image::thumb_key key{path, mtime_unix, size};
                     if (auto hit = state->thumbs.lookup(key); hit && !hit.value().empty()) {
                       if (on_ready) on_ready(path, hit.value());
                       return status::ok;
                     }

                     // A clip has no still to decode: take one software-decoded frame
                     // near its head (player/poster.h, a one-shot on this pool thread,
                     // deliberately not the playback pipeline) through the same
                     // downscale-and-encode every photo takes.
                     result<std::vector<std::uint8_t>> jpeg = err(status::internal);
                     if (is_video_name(path)) {
                       auto poster = player::poster_frame(path.c_str(), image::kThumbLongEdge, &ctx);
                       if (!poster) {
                         if (on_ready) on_ready(path, {});
                         return poster.error();
                       }
                       jpeg = image::encode_thumb_rgba(poster.value().rgba, poster.value().width,
                                                       poster.value().height);
                     } else {
                       auto bytes = io::read_all(path);
                       if (!bytes) {
                         if (on_ready) on_ready(path, {});
                         return bytes.error();
                       }
                       jpeg = image::make_thumb_jpeg(bytes.value(), &ctx);
                     }
                     if (!jpeg) {
                       if (on_ready) on_ready(path, {});
                       return jpeg.error();
                     }
                     // Recheck right before writing: the read+decode above is
                     // the job's slowest part and the likeliest place for a
                     // directory switch to land mid-flight. Skipping the
                     // store (rather than caching under the new directory's
                     // db anyway) is the one thing that actually matters —
                     // dropping this thumbnail just means the new directory
                     // regenerates it itself when it lists this path.
                     if (stale()) {
                       if (on_ready) on_ready(path, {});
                       return status::cancelled;
                     }
                     auto stored = state->thumbs.store(key, jpeg.value());
                     if (!stored) {
                       if (on_ready) on_ready(path, {});
                       return stored.error();
                     }
                     if (on_ready) on_ready(path, stored.value());
                     return status::ok;
                   });
}

}  // namespace mv::shell
