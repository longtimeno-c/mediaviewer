// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/folder_model_mac.h"

#include <utility>

#include "io/file.h"
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

  relist_async();
  return {};
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
  }
  // Jobs already submitted to `jobs_` (relist/thumb) keep their own
  // std::shared_ptr<shared_state> and finish safely against it; this object
  // is free to be destroyed without waiting for them.
}

std::vector<io::dir_entry> folder_model::items() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->items;
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
                     auto listed = io::list_still_files(dir_copy);
                     if (!listed) return listed.error();
                     {
                       std::lock_guard<std::mutex> lock(state->mutex);
                       // The directory may have changed again (or closed)
                       // while this job was queued or running.
                       if (state->dir != dir_copy) return status::cancelled;
                       state->items = std::move(listed).value();
                     }
                     state->changed.store(true, std::memory_order_release);
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
