// SPDX-License-Identifier: GPL-2.0-or-later
// Marks (plan/16 "Marks, copy, move"): a set separate from the selection, so
// arrow-key browsing never turns culling into accidental ranges.
//
// Keyed by path, not index: a re-sort or a watcher refresh must not move a mark
// onto a different file. UI thread only. Not a hot path — a toggle is a key
// press, not a key repeat's worth of work.
#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mv::shell {

class mark_set {
 public:
  // Returns whether `path` is marked afterwards.
  bool toggle(std::string_view path) {
    if (path.empty()) return false;
    if (const auto it = marks_.find(path); it != marks_.end()) {
      marks_.erase(it);
      return false;
    }
    marks_.emplace(path);
    return true;
  }

  [[nodiscard]] bool contains(std::string_view path) const {
    return marks_.find(path) != marks_.end();
  }

  template <class Paths>
  void mark_all(const Paths& paths) {
    for (const auto& p : paths) {
      if (!std::string_view{p}.empty()) marks_.emplace(p);
    }
  }

  void erase(std::string_view path) {
    if (const auto it = marks_.find(path); it != marks_.end()) marks_.erase(it);
  }

  void clear() noexcept { marks_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return marks_.size(); }
  [[nodiscard]] bool empty() const noexcept { return marks_.empty(); }

  // What F7 / F8 / Delete act on: the marks if there are any, else `current`.
  [[nodiscard]] std::vector<std::string> targets(std::string_view current) const {
    if (!marks_.empty()) return {marks_.begin(), marks_.end()};
    if (current.empty()) return {};
    return {std::string(current)};
  }

 private:
  std::set<std::string, std::less<>> marks_;
};

}  // namespace mv::shell
