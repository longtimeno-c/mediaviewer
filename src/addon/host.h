// SPDX-License-Identifier: GPL-2.0-or-later
// The host side of an add-on: the function table (mediaviewer_addon.h) built
// over the core's own io, pairing and volume ports, and the loader that maps
// a verified add-on's library and calls its one export.
//
// Metadata reads, thumbnails, the completion queue and "is the present loop
// busy" belong to each host (the Windows ABI session, the Mac app), so they
// are injected as host_services. Portable; the loader has a _win and a _mac
// half (D9).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/media.h"
#include "addon/store.h"
#include "core/result.h"
#include "io/volume.h"

namespace mv::addon {

struct host_services {
  std::function<bool(const std::string& path, mv_addon_capture& out)> capture;
  std::function<result<std::string>(const std::string& path)> thumbnail;
  std::function<void(const mv_addon_event& event)> post;
  std::function<bool()> should_yield;
  // v2 (Milestone H): pixels. Each host installs media::* (media.h); tests
  // inject fakes. Empty means MV_ERR_UNSUPPORTED_FORMAT at the thunk.
  std::function<result<rgb_image>(const std::string& path, std::uint32_t max_edge)> still_rgb;
  std::function<result<std::unique_ptr<video_sampler>>(const std::string& path,
                                                      const sampler_options& options)>
      open_sampler;
  std::function<result<rgb_image>(const std::string& path, std::int64_t pts_ms,
                                  std::uint32_t max_edge)>
      video_frame;
  std::function<result<std::string>(const std::string& path, std::int64_t pts_ms,
                                    const rgb_image* image)>
      moment_thumbnail;
  std::function<result<std::unique_ptr<audio_stream>>(const std::string& path,
                                                      std::uint32_t sample_rate,
                                                      std::int64_t start_ms)>
      open_audio;
  // The verified folder of an installed piece of the add-on's family.
  // loaded_addon::load fills it from the store when left empty.
  std::function<result<std::string>(const std::string& piece_id)> piece_dir;
  std::string data_dir;         // created by the table
  std::string default_library;  // Pictures\MediaViewer
  // Honour mv_addon_copy_request's fault injection (tests, the verify rig).
  bool test_hooks = false;
};

class host_table {
 public:
  explicit host_table(host_services services);
  ~host_table();
  host_table(const host_table&) = delete;
  host_table& operator=(const host_table&) = delete;

  [[nodiscard]] const mv_host_api* api() const noexcept { return &api_; }
  [[nodiscard]] const host_services& services() const noexcept { return svc_; }
  // The table version the add-on was offered (manifest.h negotiated_host_api).
  void set_negotiated(std::uint32_t version) noexcept;

  // Used by the C thunks.
  struct watch_state {
    std::mutex control;  // start / stop
    std::mutex m;        // cb and user
    io::volume_watcher watcher;
    void(MV_CALL* cb)(void*, std::uint32_t, const char*) = nullptr;
    void* user = nullptr;
    bool running = false;
  };
  watch_state watch;
  // Unregisters the add-on's volume callback and stops the watcher; returns
  // only when no callback is running. Idempotent.
  void stop_watch() noexcept;

 private:
  host_services svc_;
  mv_host_api api_{};
};

class shared_library {
 public:
  shared_library() = default;
  ~shared_library();
  shared_library(shared_library&& other) noexcept;
  shared_library& operator=(shared_library&& other) noexcept;
  shared_library(const shared_library&) = delete;
  shared_library& operator=(const shared_library&) = delete;

  [[nodiscard]] static result<shared_library> open(const std::string& utf8_path);
  [[nodiscard]] void* symbol(const char* name) const noexcept;
  void close() noexcept;

 private:
  void* handle_ = nullptr;
};

// A loaded, running add-on. Destruction calls its shutdown, then unloads.
class loaded_addon {
 public:
  ~loaded_addon();
  [[nodiscard]] const mv_addon_api& api() const noexcept { return api_; }
  [[nodiscard]] const void* query(const char* interface_id) const noexcept;
  [[nodiscard]] const installed& info() const noexcept { return info_; }

  // Re-verifies `addon` (signature and every file), then loads its native
  // library and calls mv_addon_get with a fresh host table. A file changed
  // since install is refused: status::corrupt. An add-on built for another
  // host API: status::unsupported_format ("needs an update").
  [[nodiscard]] static result<std::unique_ptr<loaded_addon>> load(const store& s,
                                                                   const std::string& id,
                                                                   host_services services);

 private:
  loaded_addon() = default;
  installed info_;
  std::unique_ptr<host_table> table_;
  shared_library lib_;
  mv_addon_api api_{};
};

}  // namespace mv::addon
