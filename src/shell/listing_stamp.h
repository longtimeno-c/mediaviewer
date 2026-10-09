// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The selected file's stamp (size + mtime, the key the metadata store and the
// edit stack file it under) without a stat on the UI thread (issue #231).
//
// The folder listing already stamped every item when it was read. The only
// time it is behind is right after a write this app made itself (a lossless
// rotate, a metadata write into a JPEG): the stamp moves before the watcher
// relists the folder. The write's job stats the file on its worker and the
// host notes the move here; a listing still showing a stamp the write
// replaced has not seen it, any other listing stamp is newer and wins. UI
// thread only; a map lookup per select, no I/O.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mv::shell {

struct file_stamp {
  std::uint64_t size = 0;
  std::int64_t mtime_unix = 0;
  friend bool operator==(const file_stamp&, const file_stamp&) = default;
};

class listing_stamps {
 public:
  // A write moved `path` from `before` to `after` (both stat'ed on the worker).
  void rewritten(std::string_view path, file_stamp before, file_stamp after) {
    if (path.empty() || before == after) return;  // a sidecar write: the file did not move
    auto it = by_path_.find(path);
    if (it == by_path_.end()) it = by_path_.emplace(std::string(path), entry{}).first;
    auto& replaced = it->second.replaced;
    if (replaced.size() >= kMaxReplaced) replaced.erase(replaced.begin());
    replaced.push_back(before);
    it->second.now = after;
  }

  // The stamp to key `path` by, given what the listing says. Forgets a write
  // the listing has caught up with (or moved past).
  file_stamp resolve(std::string_view path, file_stamp listed) {
    const auto it = by_path_.find(path);
    if (it == by_path_.end()) return listed;
    const auto& replaced = it->second.replaced;
    if (std::find(replaced.begin(), replaced.end(), listed) != replaced.end()) return it->second.now;
    by_path_.erase(it);
    return listed;
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_path_.size(); }

 private:
  // Several writes can land before one relist (a rating, then a comment).
  static constexpr std::size_t kMaxReplaced = 8;
  struct entry {
    file_stamp now;
    std::vector<file_stamp> replaced;
  };
  std::map<std::string, entry, std::less<>> by_path_;
};

}  // namespace mv::shell
