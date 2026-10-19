// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Several connects at once, the first to finish wins (#472: the mover's race
// across bearers, and the race across the places in an internet code, "happy
// eyeballs"). The others' links close as they arrive, before a byte is sent
// on them. Each connect has its own timeout; their threads are joined before
// the race's owner goes (no thread of the add-on outlives the engine).
// Portable (D9).
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "addons/driftwire/link.h"

namespace mv::driftwire {

class link_race {
 public:
  using connect_fn = std::function<result<std::unique_ptr<link>>()>;
  // What each connect gave, in the order they were added (status::ok for one
  // that connected, the winner or not).
  struct outcome {
    status how = status::timeout;
    bool done = false;
  };
  ~link_race() { join(); }
  // `delay_ms`: start this one that much later (happy eyeballs), unless the
  // race is already won or stopped.
  void add(connect_fn fn, int delay_ms = 0) {
    std::size_t at = 0;
    {
      std::lock_guard lock(m_);
      ++pending_;
      at = outcomes_.size();
      outcomes_.emplace_back();
    }
    threads_.emplace_back([this, fn = std::move(fn), delay_ms, at] {
      result<std::unique_ptr<link>> l = err(status::cancelled);
      bool go = true;
      if (delay_ms > 0) {
        std::unique_lock lock(m_);
        go = !cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), [&] { return winner_ || taken_; });
      }
      if (go) {
        try {
          l = fn();
        } catch (...) {
          l = err(status::internal);
        }
      }
      std::lock_guard lock(m_);
      outcomes_[at] = {l ? status::ok : l.error(), true};
      if (l && !winner_ && !taken_) {
        winner_ = std::move(*l);
        winner_at_ = static_cast<int>(at);
      } else if (l) {
        (*l)->shutdown();  // a slower one: not needed now
      }
      --pending_;
      cv_.notify_all();
    });
  }
  // The winner, once one connects or every connect has failed; nullptr when
  // none did or `stopped()` turned true.
  template <typename Stopped>
  std::unique_ptr<link> wait(const Stopped& stopped) {
    std::unique_lock lock(m_);
    while (!winner_ && pending_ > 0 && !stopped()) cv_.wait_for(lock, std::chrono::milliseconds(100));
    taken_ = true;
    cv_.notify_all();  // a delayed connect that has not started does not
    return std::move(winner_);
  }
  // Every connect has returned (their threads end at once).
  bool settled() {
    std::lock_guard lock(m_);
    return pending_ == 0;
  }
  // Which add() won (in order), or -1.
  int winner_index() {
    std::lock_guard lock(m_);
    return winner_at_;
  }
  std::vector<outcome> outcomes() {
    std::lock_guard lock(m_);
    return outcomes_;
  }
  void join() {
    for (auto& t : threads_) {
      if (t.joinable()) t.join();
    }
    threads_.clear();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0;
  bool taken_ = false;
  std::unique_ptr<link> winner_;
  int winner_at_ = -1;
  std::vector<outcome> outcomes_;
  std::vector<std::thread> threads_;
};

}  // namespace mv::driftwire
