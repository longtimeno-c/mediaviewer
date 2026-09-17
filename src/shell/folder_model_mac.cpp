// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/folder_model_mac.h"

#include <utility>

#include "io/file.h"

namespace mv::shell {

folder_model::~folder_model() { close(); }

expected folder_model::open(std::string_view dir_utf8, job_system& jobs) noexcept {
  if (dir_utf8.empty()) return err(status::invalid_arg);
  jobs_ = &jobs;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    dir_.assign(dir_utf8);
  }

  if (auto opened = thumbs_.open(dir_utf8); !opened) return opened;
  if (auto started = watcher_.start(dir_utf8, &folder_model::watch_callback, this); !started) {
    thumbs_.close();
    return started;
  }

  relist_async();
  return {};
}

void folder_model::close() noexcept {
  watcher_.stop();
  thumbs_.close();
  std::lock_guard<std::mutex> lock(mutex_);
  dir_.clear();
  items_.clear();
}

std::vector<io::dir_entry> folder_model::items() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_;
}

std::size_t folder_model::item_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_.size();
}

bool folder_model::consume_changed() noexcept {
  return changed_.exchange(false, std::memory_order_acq_rel);
}

void folder_model::watch_callback(void* user) noexcept {
  static_cast<folder_model*>(user)->relist_async();
}

void folder_model::relist_async() {
  if (!jobs_) return;
  std::string dir_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dir_copy = dir_;
  }
  if (dir_copy.empty()) return;

  // Background generation (core/job_system.h): a relist triggered by a
  // filesystem event is not tied to the current view intent and must not be
  // abandoned just because the user arrowed to the next photo mid-scan.
  jobs_->submit_at(background_generation, [this, dir_copy](const job_context&) -> status {
    auto listed = io::list_still_files(dir_copy);
    if (!listed) return listed.error();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // The directory may have changed again (or closed) while this job was
      // queued or running; only publish if we are still looking at it.
      if (dir_ != dir_copy) return status::cancelled;
      items_ = std::move(listed).value();
    }
    changed_.store(true, std::memory_order_release);
    return status::ok;
  });
}

void folder_model::request_thumb(std::string path_utf8, std::int64_t mtime_unix,
                                 std::uint64_t size, thumb_ready_fn on_ready) {
  if (!jobs_) {
    if (on_ready) on_ready(std::move(path_utf8), {});
    return;
  }

  jobs_->submit_at(background_generation,
                   [this, path = std::move(path_utf8), mtime_unix, size,
                    on_ready = std::move(on_ready)](const job_context& ctx) -> status {
                     const image::thumb_key key{path, mtime_unix, size};
                     if (auto hit = thumbs_.lookup(key); hit && !hit.value().empty()) {
                       if (on_ready) on_ready(path, hit.value());
                       return status::ok;
                     }

                     auto bytes = io::read_all(path);
                     if (!bytes) {
                       if (on_ready) on_ready(path, {});
                       return bytes.error();
                     }
                     auto jpeg = image::make_thumb_jpeg(bytes.value(), &ctx);
                     if (!jpeg) {
                       if (on_ready) on_ready(path, {});
                       return jpeg.error();
                     }
                     auto stored = thumbs_.store(key, jpeg.value());
                     if (!stored) {
                       if (on_ready) on_ready(path, {});
                       return stored.error();
                     }
                     if (on_ready) on_ready(path, stored.value());
                     return status::ok;
                   });
}

}  // namespace mv::shell
