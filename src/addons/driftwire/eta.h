// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #529: how fast a transfer moves and how long it has left, for the line
// under its progress on both windows ("1.2 GB of 4.8 GB · 14 MB/s · about
// 4 min left"). The rate is the average since the meter started for its
// first seconds, then a moving average with a 4 s time constant, so the
// estimate settles instead of jumping with each chunk. A link that drops
// restarts the meter (engine.cpp): a rate from before a handover says
// nothing about the link after it. Portable, no clock of its own (tests).
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace mv::driftwire {

class transfer_meter {
 public:
  using clock = std::chrono::steady_clock;
  static constexpr double kTauS = 4.0;        // the moving average's time constant
  static constexpr double kWarmS = 5.0;       // the average since the start until then
  static constexpr double kReadyS = 3.0;      // no estimate before this long...
  static constexpr std::uint64_t kReadyBytes = 512 * 1024;  // ...or this much moved

  void restart() noexcept { *this = transfer_meter{}; }

  // The transfer's bytes so far, now. Fewer than last time: a new transfer.
  void add(clock::time_point now, std::uint64_t bytes) noexcept {
    if (!started_ || bytes < last_bytes_) {
      started_ = true;
      start_ = last_ = now;
      start_bytes_ = last_bytes_ = bytes;
      rate_ = 0;
      return;
    }
    const double dt = std::chrono::duration<double>(now - last_).count();
    if (dt < 0.2) return;  // chunks bunch up: a sample at most every 200 ms
    const double since = std::chrono::duration<double>(now - start_).count();
    if (since <= kWarmS) {
      rate_ = static_cast<double>(bytes - start_bytes_) / since;
    } else {
      const double inst = static_cast<double>(bytes - last_bytes_) / dt;
      rate_ += (1 - std::exp(-dt / kTauS)) * (inst - rate_);
    }
    last_ = now;
    last_bytes_ = bytes;
  }

  [[nodiscard]] bool ready() const noexcept {
    return started_ && rate_ > 0 && std::chrono::duration<double>(last_ - start_).count() >= kReadyS &&
           last_bytes_ - start_bytes_ >= kReadyBytes;
  }
  // Bytes a second; 0 before ready().
  [[nodiscard]] double rate() const noexcept { return ready() ? rate_ : 0; }
  // Seconds to `total`; < 0 before ready().
  [[nodiscard]] double seconds_left(std::uint64_t total) const noexcept {
    if (!ready()) return -1;
    return total > last_bytes_ ? static_cast<double>(total - last_bytes_) / rate_ : 0;
  }

 private:
  bool started_ = false;
  clock::time_point start_{}, last_{};
  std::uint64_t start_bytes_ = 0, last_bytes_ = 0;
  double rate_ = 0;
};

// "about 4 min left", rounded so it does not flicker; "" when unknown.
inline std::string time_left_text(double seconds) {
  if (seconds < 0) return "";
  if (seconds < 10) return "a few seconds left";
  if (seconds < 55) return "about " + std::to_string(static_cast<int>(std::lround(seconds / 5) * 5)) + " seconds left";
  if (seconds < 90) return "about a minute left";
  const long minutes = std::lround(seconds / 60);
  if (minutes < 60) return "about " + std::to_string(minutes) + " min left";
  const long rounded = std::lround(seconds / 300) * 5;  // to five minutes past the hour
  const long h = rounded / 60, m = rounded % 60;
  return "about " + std::to_string(h) + " h" + (m ? " " + std::to_string(m) + " min" : std::string()) + " left";
}

}  // namespace mv::driftwire
