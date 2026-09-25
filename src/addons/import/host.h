// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A C++ face on the host function table (mediaviewer_addon.h). Everything the
// add-on does to a file goes through here: it links no part of the core.
#pragma once

#include <cstdint>
#include <functional>
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

}  // namespace mv::import
