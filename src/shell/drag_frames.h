// SPDX-License-Identifier: GPL-3.0-or-later
// --pane-drag-soak (docs/plans/edit-refresh.md §7 S1, docs/DEVELOPMENT.md): the
// present-to-present intervals the render thread made while the right
// column's edge was dragged, and the presents in all. Written by the render
// thread only; the host reads after the drag. Each slot is written once and
// published by the count (release), so a reader never sees a torn entry and
// nothing waits. Header only, shared by both labs.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace mv::shell {

class drag_frame_log {
 public:
  static constexpr std::uint32_t kCap = 8192;  // ~68 s at 120 Hz

  // Before the render thread starts: only an armed log records intervals.
  void arm() noexcept {
    if (!ms_) ms_.reset(new (std::nothrow) float[kCap]);
  }

  // [render thread] One call per present. `live` is chrome_live_resize of the
  // snapshot it drew; an interval counts only when this present and the one
  // before both drew a live drag.
  void on_present(double now_s, bool live, double refresh_s) noexcept {
    presents_.fetch_add(1, std::memory_order_relaxed);
    if (!ms_) return;
    if (live && last_live_ && last_s_ >= 0.0) {
      const std::uint32_t n = count_.load(std::memory_order_relaxed);
      if (n < kCap) {
        ms_[n] = static_cast<float>((now_s - last_s_) * 1000.0);
        count_.store(n + 1, std::memory_order_release);
      }
    }
    if (live && refresh_s > 0.0) refresh_ms_.store(static_cast<float>(refresh_s * 1000.0), std::memory_order_relaxed);
    last_s_ = now_s;
    last_live_ = live;
  }

  // [any thread]
  [[nodiscard]] std::uint64_t presents() const noexcept { return presents_.load(std::memory_order_relaxed); }
  [[nodiscard]] float refresh_ms() const noexcept { return refresh_ms_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::vector<float> intervals() const {
    std::vector<float> out;
    if (!ms_) return out;
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    out.assign(ms_.get(), ms_.get() + n);
    return out;
  }

 private:
  std::unique_ptr<float[]> ms_;
  std::atomic<std::uint32_t> count_{0};
  std::atomic<std::uint64_t> presents_{0};
  std::atomic<float> refresh_ms_{0.0f};
  double last_s_ = -1.0;  // render thread only
  bool last_live_ = false;
};

// The summary --pane-drag-soak writes (and its pass rule, docs/plans/edit-refresh.md
// §7 S1): no frame over 2x the refresh interval. The p99 against a base run and
// the presents after settle are the host's (it has the base and the clock).
struct drag_frame_summary {
  std::uint32_t frames = 0;
  float refresh_ms = 0.0f;
  float p50_ms = 0.0f;
  float p99_ms = 0.0f;
  float max_ms = 0.0f;
  std::uint32_t over_2x = 0;
};

[[nodiscard]] inline float percentile(std::vector<float> v, double p) {
  if (v.empty()) return 0.0f;
  std::sort(v.begin(), v.end());
  const auto at = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1) + 0.5);
  return v[std::min(at, v.size() - 1)];
}

[[nodiscard]] inline drag_frame_summary summarise(const std::vector<float>& ms, float refresh_ms) {
  drag_frame_summary s;
  s.frames = static_cast<std::uint32_t>(ms.size());
  s.refresh_ms = refresh_ms;
  if (ms.empty()) return s;
  s.p50_ms = percentile(ms, 0.50);
  s.p99_ms = percentile(ms, 0.99);
  s.max_ms = *std::max_element(ms.begin(), ms.end());
  if (refresh_ms > 0.0f) {
    s.over_2x = static_cast<std::uint32_t>(
        std::count_if(ms.begin(), ms.end(), [&](float f) { return f > 2.0f * refresh_ms; }));
  }
  return s;
}

}  // namespace mv::shell
