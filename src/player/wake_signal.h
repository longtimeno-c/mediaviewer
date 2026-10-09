// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #230 - where a player worker parks when it has nothing to do.
//
// A paused or ended clip is the normal state while arrowing through a camera
// dump, and the workers used to sleep-poll through it: about 2,500 wakes a
// second for a clip nobody is watching. They now park here until something
// that could unblock them happens -- a packet queued or taken, a ring slot
// released, a block taken by the audio pump, a seek, a generation bump, stop.
//
// One futex word (std::atomic wait/notify: WaitOnAddress on Windows, ulock on
// macOS), not a mutex and condition variable, because one of the notifiers is
// the render thread releasing a frame: notify() takes no lock, so the render
// thread never waits on one a worker holds (CLAUDE.md rule 1). With nobody
// parked, notify() is an atomic increment and a check.
//
// The epoch is read BEFORE the condition is checked, so a notify that lands
// between the check and the wait changes the word and the wait returns at
// once: no wake is lost, which is why no wait here needs a timeout.
#pragma once

#include <atomic>
#include <cstdint>

namespace mv::player {

class wake_signal {
 public:
  // [worker] Read first, then check the condition, then wait(seen).
  [[nodiscard]] std::uint32_t epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
  }
  // [worker] Returns once the epoch is no longer `seen` (spurious returns are
  // allowed; callers re-check their condition).
  void wait(std::uint32_t seen) const noexcept { epoch_.wait(seen, std::memory_order_acquire); }

  // [worker] Parks until `ready()` holds.
  template <class Ready>
  void wait_until(Ready ready) const noexcept {
    for (;;) {
      const std::uint32_t seen = epoch();
      if (ready()) return;
      wait(seen);
    }
  }

  // [any-thread][no-block] After the state change a waiter is waiting for.
  void notify() noexcept {
    epoch_.fetch_add(1, std::memory_order_release);
    epoch_.notify_all();
  }

 private:
  std::atomic<std::uint32_t> epoch_{0};
};

}  // namespace mv::player
