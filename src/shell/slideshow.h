// SPDX-License-Identifier: GPL-2.0-or-later
// Slideshow as a mode (plan/16 "Slideshow"). No transition pass: "next" is the
// same navigation command as browse, on a timer, so prefetch and the
// generation counter stay in play. This is the pure part — order, interval,
// and when to advance — tested without a window or a clock.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace mv::shell {

class slideshow {
 public:
  // `+` shortens, `-` lengthens, one rung at a time, clamped at the ends.
  static constexpr std::array<std::uint32_t, 11> kIntervalsMs = {
      1000, 2000, 3000, 4000, 5000, 7000, 10000, 15000, 20000, 30000, 60000};
  static constexpr std::size_t kDefaultRung = 3;  // 4 s

  void start(std::uint32_t count, std::uint32_t current, std::uint64_t seed) {
    active_ = true;
    paused_ = false;
    blackout_ = false;
    count_ = count;
    seed_ = seed;
    if (shuffle_) build_order(current);
  }

  void stop() noexcept {
    active_ = false;
    paused_ = false;
    blackout_ = false;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] bool paused() const noexcept { return paused_; }
  [[nodiscard]] bool blackout() const noexcept { return blackout_; }
  [[nodiscard]] bool shuffled() const noexcept { return shuffle_; }
  [[nodiscard]] std::uint32_t interval_ms() const noexcept { return kIntervalsMs[rung_]; }

  void toggle_pause() noexcept { paused_ = !paused_; }
  void toggle_blackout() noexcept { blackout_ = !blackout_; }
  void faster() noexcept {
    if (rung_ > 0) --rung_;
  }
  void slower() noexcept {
    if (rung_ + 1 < kIntervalsMs.size()) ++rung_;
  }

  // R: a fresh no-repeat order starting from the current item, or back to
  // folder order. Allocates here, never per advance.
  void toggle_shuffle(std::uint32_t current, std::uint64_t seed) {
    shuffle_ = !shuffle_;
    seed_ = seed;
    if (shuffle_) build_order(current);
    else order_.clear();
  }

  // The listing changed under a running slideshow (watcher, delete).
  void set_count(std::uint32_t count, std::uint32_t current) {
    if (count == count_) return;
    count_ = count;
    if (shuffle_) build_order(current);
  }

  // The item after `current`, or nothing at the end when not wrapping (the
  // slideshow then stops on the last item). Shuffle visits every item once
  // before any repeats; with wrap it goes round the same order again.
  [[nodiscard]] std::optional<std::uint32_t> next(std::uint32_t current, bool wrap) {
    if (count_ == 0) return std::nullopt;
    if (!shuffle_ || order_.size() != count_) {
      if (current + 1 < count_) return current + 1;
      if (wrap) return 0u;
      return std::nullopt;
    }
    const auto it = std::find(order_.begin(), order_.end(), current);
    std::size_t pos = it == order_.end() ? position_ : static_cast<std::size_t>(it - order_.begin());
    if (pos + 1 < order_.size()) {
      position_ = pos + 1;
      return order_[position_];
    }
    if (!wrap) return std::nullopt;
    position_ = 0;
    return order_[0];
  }

  // Where the current item's media is, as far as the slideshow cares.
  enum class media : std::uint8_t {
    none,      // a still, or an animation that loops forever: interval only
    opening,   // a clip is open but has not started (async open, first frame)
    playing,   // a clip, or a finite animation, still going
    paused,    // plan/16: a paused clip goes on the interval
    finished,  // ended, or a finite animation reached its loop count
  };

  // Opening counts as not finished: a 4K clip can take longer to open than the
  // shortest interval, and must not be skipped before it has played.
  [[nodiscard]] static constexpr bool media_finished(media m) noexcept {
    return m != media::opening && m != media::playing;
  }

  // plan/16: a clip (or a finite animation) advances at whichever is later —
  // the interval, or the end of the media. A still, or an animation that
  // loops forever, advances on the interval alone. Paused never advances.
  [[nodiscard]] bool should_advance(std::uint64_t elapsed_ms, bool media_finished) const noexcept {
    return active_ && !paused_ && elapsed_ms >= interval_ms() && media_finished;
  }

 private:
  // Fisher-Yates with splitmix64: deterministic for a seed (testable), no
  // dependency on <random>'s per-platform distributions.
  void build_order(std::uint32_t current) {
    order_.resize(count_);
    for (std::uint32_t i = 0; i < count_; ++i) order_[i] = i;
    std::uint64_t state = seed_;
    const auto rnd = [&state]() noexcept {
      state += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = state;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      return z ^ (z >> 31);
    };
    for (std::size_t i = order_.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rnd() % i);
      std::swap(order_[i - 1], order_[j]);
    }
    // Start the round from where the user is, so R never jumps backwards to
    // an item already seen in this round.
    if (const auto it = std::find(order_.begin(), order_.end(), current); it != order_.end()) {
      std::swap(*it, order_.front());
    }
    position_ = 0;
  }

  bool active_ = false;
  bool paused_ = false;
  bool blackout_ = false;
  bool shuffle_ = false;
  std::size_t rung_ = kDefaultRung;
  std::uint32_t count_ = 0;
  std::uint64_t seed_ = 0;
  std::vector<std::uint32_t> order_;
  std::size_t position_ = 0;
};

}  // namespace mv::shell
