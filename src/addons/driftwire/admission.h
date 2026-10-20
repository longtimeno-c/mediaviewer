// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #502: how often one source (an address, the radio) may start something
// here: a receive, a notification, the window brought forward. A sender that
// connects, idles and connects again all night is held to a few a minute,
// and real senders from elsewhere still get through. Portable, pure; the
// engine holds one per kind, under its lock.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace mv::driftwire {

class source_limit {
 public:
  using clock = std::chrono::steady_clock;
  // At most `per_window` in any `window` from one source; `max_sources`
  // remembered, the one heard from longest ago dropped for a new one.
  source_limit(int per_window, std::chrono::milliseconds window, std::size_t max_sources = 256)
      : per_window_(per_window), window_(window), max_sources_(max_sources) {}

  // True (and counted) while `source` has fewer than per_window in the last
  // window. An empty source is not limited.
  bool allow(const std::string& source, clock::time_point now) {
    if (source.empty()) return true;
    auto it = std::find_if(seen_.begin(), seen_.end(), [&](const entry& e) { return e.source == source; });
    if (it == seen_.end()) {
      if (seen_.size() >= max_sources_) {
        auto oldest = std::min_element(seen_.begin(), seen_.end(),
                                       [](const entry& a, const entry& b) { return a.last < b.last; });
        seen_.erase(oldest);
      }
      seen_.push_back({source, {}, now});
      it = std::prev(seen_.end());
    }
    auto& at = it->at;
    std::erase_if(at, [&](clock::time_point t) { return now - t >= window_; });
    it->last = now;
    if (static_cast<int>(at.size()) >= per_window_) return false;
    at.push_back(now);
    return true;
  }

 private:
  struct entry {
    std::string source;
    std::vector<clock::time_point> at;  // in the last window, at most per_window
    clock::time_point last;
  };
  int per_window_;
  std::chrono::milliseconds window_;
  std::size_t max_sources_;
  std::vector<entry> seen_;
};

}  // namespace mv::driftwire
