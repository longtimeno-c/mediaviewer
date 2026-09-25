// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Volumes: what a path lives on, card arrival, eject (plan/18-import.md "Ports").
//
// Portable header. io/volume_win.cpp: GetVolumePathNameW / serial number,
// IOCTL_STORAGE_GET_DEVICE_NUMBER for the physical device, WM_DEVICECHANGE on
// a hidden window thread for arrival, IOCTL_STORAGE_EJECT_MEDIA and then
// CM_Request_Device_Eject for eject. io/volume_mac.cpp: statfs plus
// DiskArbitration (volume UUID, whole-disk BSD name, the mount callbacks, and
// DADiskUnmount + DADiskEject); the Linux test build answers from statfs only.
//
// Nothing here formats, erases or deletes (plan/18 "Never offered"). Volume
// ids and labels stay on the machine (rule 6). Worker or watch threads only.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::io {

struct volume_info {
  // Stable across unplug / replug of the same card, so "new since last
  // import" survives it: the filesystem's UUID where it has one, else its
  // serial number with the capacity.
  std::string volume_id;
  // Where it is mounted: "E:\" / "/Volumes/EOS_DIGITAL".
  std::string root_utf8;
  std::string label_utf8;
  // One key per physical device, so two cards in one reader share a reader
  // thread and two readers do not (plan/18 "Throughput").
  std::string device_key;
  std::uint64_t total_bytes = 0;
  std::uint64_t free_bytes = 0;
  bool removable = false;
  bool network = false;
  bool read_only = false;
};

// The volume `utf8_path` lives on.
[[nodiscard]] result<volume_info> volume_of(std::string_view utf8_path);

// Mounted, user-visible volumes, removable first.
[[nodiscard]] result<std::vector<volume_info>> list_volumes();

// Unmounts and ejects the volume at `root_utf8`. status::io if it is busy or
// cannot be ejected (a fixed disk); nothing is deleted either way.
[[nodiscard]] expected eject_volume(std::string_view root_utf8);

enum class volume_event : std::uint8_t { arrived = 0, removed = 1 };

class volume_watcher {
 public:
  // `cb` runs on the watcher's own thread. It must not block; post to the
  // completion queue or submit a job.
  using callback = void (*)(void* user, volume_event event, const char* root_utf8);

  volume_watcher();
  ~volume_watcher();
  volume_watcher(const volume_watcher&) = delete;
  volume_watcher& operator=(const volume_watcher&) = delete;

  [[nodiscard]] expected start(callback cb, void* user);
  void stop() noexcept;

 private:
  struct impl;
  std::mutex mutex_;
  std::unique_ptr<impl> impl_;
};

}  // namespace mv::io
