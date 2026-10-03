// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Several files at once for a batch copy to or from a network share
// (io/verified_copy.h batch_copy_profile, plan/12 2026-10-01).
//
// A share answers each create, flush, read-back and rename with a round trip;
// copying one file at a time leaves the link idle through all of them. This
// runs `fn(i)` for i in [0, count), up to `width` at once, on threads of its
// own. It is not core/parallel.h's band split: the work waits on the network,
// not a core. Items start in index order. Once `stop()` returns true no new
// item starts; running ones finish. `fn` must not throw. Worker threads only
// (rule 1): the call returns when every started item has.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace mv::io {

inline void for_each_in_flight(std::size_t count, unsigned width,
                               const std::function<void(std::size_t)>& fn,
                               const std::function<bool()>& stop = {}) {
  if (count == 0) return;
  std::atomic<std::size_t> next{0};
  const auto run = [&] {
    for (;;) {
      if (stop && stop()) return;
      const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= count) return;
      fn(i);
    }
  };
  const std::size_t threads = std::min<std::size_t>(std::max(width, 1u), count);
  std::vector<std::thread> extra;
  extra.reserve(threads - 1);
  for (std::size_t t = 1; t < threads; ++t) {
    try {
      extra.emplace_back(run);
    } catch (...) {
      break;  // no thread to be had: the ones running take the rest
    }
  }
  run();
  for (auto& t : extra) t.join();
}

}  // namespace mv::io
