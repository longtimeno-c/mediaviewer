// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #540 step 1: where a transfer's time goes, one side at a time, so every
// speed change names the stage it moves. The benches give a stage_times to
// hooks::stages (and the engine's options); the app gives none, and a stage
// with no stage_times costs one branch.
//
// Each stage adds the time a thread spent in it and the bytes it handled.
// Stages on several threads (the receiver's two verifiers) add each thread's
// time, so `bytes / time` is the rate one thread of that stage sustains: a
// stage whose rate is far above the transfer's has headroom, and one near it
// is the limit. `recv` and `wait_room` are waits (the link had nothing yet,
// the peer had not acknowledged), not work. Sizes and times only (rule 6).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace mv::driftwire {

enum class stage : std::uint8_t {
  read,       // sender: the file, from its disk
  hash,       // sender: BLAKE3 as read; receiver: BLAKE3 as written
  seal,       // the copy into the replay and the AEAD seal
  send,       // the sealed frame onto the link (blocks while the socket is full)
  wait_room,  // sender: the replay window full, waiting for the peer's acknowledgement
  recv,       // receiver: the frame off the link (a wait while nothing has arrived)
  open,       // receiver: the AEAD open and the frame's header taken off
  write,      // receiver: the bytes into the temporary
  flush,      // receiver: the temporary to stable storage
  read_back,  // receiver: the temporary read back uncached
  back_hash,  // receiver: BLAKE3 of the read-back
  rename,     // receiver: the no-replace rename into place
  verify_wait,  // receiver: a whole file waiting for room among the files being verified
  count,
};

[[nodiscard]] const char* stage_name(stage s) noexcept;

struct stage_times {
  static constexpr int kCount = static_cast<int>(stage::count);
  std::atomic<std::uint64_t> ns[kCount]{};
  std::atomic<std::uint64_t> bytes[kCount]{};
  std::atomic<std::uint64_t> calls[kCount]{};

  void add(stage s, std::chrono::steady_clock::duration d, std::uint64_t n) noexcept {
    const auto i = static_cast<int>(s);
    ns[i].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()),
                    std::memory_order_relaxed);
    bytes[i].fetch_add(n, std::memory_order_relaxed);
    calls[i].fetch_add(1, std::memory_order_relaxed);
  }
  void reset() noexcept;
  // {"read":{"ms":12.3,"mb":1073.7,"mb_s":8700.1,"calls":4096},...}: the
  // stages that ran, in order. `wall_s` > 0 adds each stage's "share" of it.
  [[nodiscard]] std::string json(double wall_s = 0) const;
};

// Times one stage from construction to destruction (or done()); nothing
// when `t` is null.
class stage_timer {
 public:
  stage_timer(stage_times* t, stage s, std::uint64_t bytes = 0) noexcept : t_(t), s_(s), bytes_(bytes) {
    if (t_) at_ = std::chrono::steady_clock::now();
  }
  ~stage_timer() { done(); }
  stage_timer(const stage_timer&) = delete;
  stage_timer& operator=(const stage_timer&) = delete;
  void set_bytes(std::uint64_t n) noexcept { bytes_ = n; }
  void done() noexcept {
    if (!t_) return;
    t_->add(s_, std::chrono::steady_clock::now() - at_, bytes_);
    t_ = nullptr;
  }

 private:
  stage_times* t_;
  stage s_;
  std::uint64_t bytes_;
  std::chrono::steady_clock::time_point at_{};
};

}  // namespace mv::driftwire
