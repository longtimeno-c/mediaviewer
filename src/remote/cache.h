// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The remote folder's disk cache (docs/plans/desktop-client.md §3 "Caches"):
// originals fetched from a paired service and the tiles made from them, each
// under the file's content identity (folder, path, size, mtime), so a changed
// file is a new entry and an unchanged one is never fetched twice. Bounded,
// least recently used first. Worker threads only: every call may touch disk.
// Portable (D9).
#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mv::remote {

struct identity {
  std::string server;
  std::string root;
  std::string rel;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
};

class disk_cache {
 public:
  // `dir`: created as needed. `cap_bytes`: originals kept at most (tiles are
  // not counted: a few tens of KiB each, cleared with the server).
  disk_cache(std::string dir, std::uint64_t cap_bytes);

  // Where the original lives once it is here: <dir>/<server>/<h>/<name>, the
  // name kept so kind-by-name (the D5 table) holds.
  [[nodiscard]] std::string original_path(const identity& id) const;
  [[nodiscard]] std::string tile_path(const identity& id) const;
  // The original when it is here whole (its size checked): marked used.
  [[nodiscard]] std::optional<std::string> find(const identity& id);
  // A finished part file becomes the original (size checked). Then trims to
  // the cap, never removing `keep`.
  [[nodiscard]] bool admit(const identity& id, const std::string& part_path);
  // Everything of one server (Forget).
  void remove_server(std::string_view server);
  [[nodiscard]] std::uint64_t bytes_used();

 private:
  void scan_locked();
  void trim_locked(const std::string& keep);
  [[nodiscard]] static std::string hash_of(const identity& id);

  std::string dir_;
  std::uint64_t cap_;
  std::mutex m_;
  bool scanned_ = false;
  std::list<std::pair<std::string, std::uint64_t>> lru_;  // newest first: path, bytes
  std::unordered_map<std::string, std::list<std::pair<std::string, std::uint64_t>>::iterator> index_;
  std::uint64_t used_ = 0;
};

}  // namespace mv::remote
