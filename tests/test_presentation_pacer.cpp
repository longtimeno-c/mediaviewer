// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux PR 1's pacer arithmetic. No compositor: the Linux lab feeds it
// wp_presentation feedback; this file checks the counters a 60 s soak is judged
// by, including what headless Weston's flags-0, seq-0 feedback must come to and
// what KWin's seq-0 feedback with both flags counts as.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>

#include "gfx/presentation_pacer.h"

using namespace std::chrono_literals;

namespace {

constexpr double k60 = 1.0 / 60.0;
constexpr std::uint32_t k60_ns = 16'666'667;
constexpr std::uint32_t k_hw = mv::gfx::k_presentation_vsync | mv::gfx::k_presentation_hw_clock |
                               mv::gfx::k_presentation_hw_completion;

mv::gfx::presentation_sample presented(std::uint64_t id, std::uint64_t msc,
                                       std::uint32_t flags = k_hw) {
  mv::gfx::presentation_sample s;
  s.frame_id = id;
  s.presented = true;
  s.msc = msc;
  s.flags = flags;
  s.refresh_ns = k60_ns;
  s.timestamp_ns = 1'000'000'000ull + msc * k60_ns;
  return s;
}

// KWin's answer (Plasma 6): VSYNC, HW_CLOCK and HW_COMPLETION, the refresh, a
// hardware timestamp at refresh `index`, and seq 0 on every one.
mv::gfx::presentation_sample kwin(std::uint64_t id, std::uint64_t index) {
  auto s = presented(id, index);
  s.msc = 0;
  return s;
}

mv::gfx::presentation_sample discarded(std::uint64_t id) {
  mv::gfx::presentation_sample s;
  s.frame_id = id;
  return s;
}

// Commits `count` frames starting at id `first`, a millisecond or two apart:
// well inside 1.5 refreshes, so the clock alone never infers a miss.
void commit(mv::gfx::presentation_pacer& pacer, std::uint64_t first, int count) {
  for (int i = 0; i < count; ++i) {
    std::this_thread::sleep_for(1ms);
    pacer.frame_begin();
    pacer.frame_end(first + static_cast<std::uint64_t>(i));
  }
}

}  // namespace

TEST_CASE("a fresh presentation pacer reports nothing rather than zero",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::none);
  REQUIRE_FALSE(stats.meets_linux_pr1_gate());
  REQUIRE(std::string(mv::gfx::presentation_drop_source_label(stats.source)) == "no data");
}

TEST_CASE("a clean msc step of one is a displayed present and no drop",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 5);
  REQUIRE(pacer.pending_feedback() == 5);
  for (std::uint64_t id = 1; id <= 5; ++id) pacer.on_feedback(presented(id, 100 + id));
  const auto stats = pacer.stats();
  REQUIRE(pacer.pending_feedback() == 0);
  REQUIRE(stats.frames == 4);              // the first present is a baseline
  REQUIRE(stats.displayed_presents == 4);  // and so is its feedback
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.missed_refreshes == 0);
  REQUIRE(stats.statistics_unavailable_frames == 0);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
  REQUIRE(std::string(mv::gfx::presentation_drop_source_label(stats.source)) ==
          "Wayland presentation feedback");
  REQUIRE(pacer.feedback_refresh_ns() == k60_ns);
}

TEST_CASE("an msc step of two is one missed refresh", "[gfx][presentation_pacer]") {
  mv::gfx::presentation_statistics tracker;
  REQUIRE(tracker.observe(presented(1, 10)).baseline);
  const auto sample = tracker.observe(presented(2, 12));
  REQUIRE(sample.valid);
  REQUIRE(sample.presents == 1);
  REQUIRE(sample.missed_refreshes == 1);
  REQUIRE(sample.presented_interval_ms > 33.0);

  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 3);
  pacer.on_feedback(presented(1, 10));
  pacer.on_feedback(presented(2, 11));
  pacer.on_feedback(presented(3, 13));  // the compositor held frame 3 one refresh
  const auto stats = pacer.stats();
  REQUIRE(stats.dropped_frames == 1);
  REQUIRE(stats.missed_refreshes == 1);
  // The display saw the miss: the commits were not late, and an authoritative
  // sample is judged by its msc step, never by the clock.
  REQUIRE(stats.displayed_presents == 2);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
}

TEST_CASE("a discarded commit is a dropped frame", "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 4);
  pacer.on_feedback(presented(1, 50));
  pacer.on_feedback(presented(2, 51));
  pacer.on_feedback(discarded(3));
  pacer.on_feedback(presented(4, 52));
  const auto stats = pacer.stats();
  REQUIRE(stats.discarded_frames == 1);
  REQUIRE(stats.dropped_frames == 1);
  REQUIRE(stats.missed_refreshes == 0);  // the msc did not skip; a frame did
  REQUIRE(stats.displayed_presents == 2);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
}

TEST_CASE("feedback without VSYNC, HW_CLOCK or a vblank counter is weaker and cannot pass",
          "[gfx][presentation_pacer]") {
  // Headless Weston: flags 0, seq 0, refresh 16.67 ms.
  const std::uint32_t weak_flags[] = {0u, mv::gfx::k_presentation_vsync,
                                      mv::gfx::k_presentation_hw_clock};
  for (const std::uint32_t flags : weak_flags) {
    mv::gfx::presentation_pacer pacer;
    pacer.begin_session(k60);
    commit(pacer, 1, 4);
    for (std::uint64_t id = 1; id <= 4; ++id) pacer.on_feedback(presented(id, 7 + id, flags));
    const auto stats = pacer.stats();
    REQUIRE(stats.displayed_presents == 0);
    REQUIRE(stats.statistics_unavailable_frames == 3);
    REQUIRE(stats.source == mv::gfx::presentation_drop_source::interval_heuristic);
    REQUIRE(std::string(mv::gfx::presentation_drop_source_label(stats.source)) ==
            "clock intervals (weaker)");
  }
  // Both flags and seq 0, but refresh 0 (a variable-rate output under v1):
  // neither a counter nor a constant refresh to count steps with.
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 3);
  for (std::uint64_t id = 1; id <= 3; ++id) {
    auto s = kwin(id, 7 + id);
    s.refresh_ns = 0;
    pacer.on_feedback(s);
  }
  REQUIRE(pacer.stats().source == mv::gfx::presentation_drop_source::interval_heuristic);
  REQUIRE(pacer.stats().displayed_presents == 0);
}

TEST_CASE("KWin's seq 0 feedback is counted by its hardware clock and can pass",
          "[gfx][presentation_pacer]") {
  // Every answer from KWin: both flags, the refresh, and seq_hi = seq_lo = 0.
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  for (std::uint64_t id = 1; id <= 120; ++id) {
    commit(pacer, id, 1);
    auto s = kwin(id, 500 + id);
    // A few microseconds of scanout jitter round away.
    s.timestamp_ns += (id % 3) * 4'000;
    pacer.on_feedback(s);
  }
  auto stats = pacer.stats();
  REQUIRE(stats.frames == 119);
  REQUIRE(stats.displayed_presents == 119);
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.missed_refreshes == 0);
  REQUIRE(stats.statistics_unavailable_frames == 0);
  REQUIRE(stats.statistics_discontinuities == 0);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);

  // Frame 3 was shown two refreshes after frame 2: one miss.
  mv::gfx::presentation_pacer gap;
  gap.begin_session(k60);
  commit(gap, 1, 5);
  const std::uint64_t index[] = {10, 11, 13, 14, 15};
  for (std::uint64_t id = 1; id <= 5; ++id) gap.on_feedback(kwin(id, index[id - 1]));
  stats = gap.stats();
  REQUIRE(stats.displayed_presents == 4);
  REQUIRE(stats.dropped_frames == 1);
  REQUIRE(stats.missed_refreshes == 1);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
}

TEST_CASE("with no msc, two presents in one refresh or a counter that comes and goes is a "
          "discontinuity",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_statistics tracker;
  REQUIRE(tracker.observe(kwin(1, 10)).baseline);
  REQUIRE(tracker.observe(kwin(2, 11)).valid);
  auto same = kwin(3, 11);
  same.timestamp_ns += 2'000'000;  // 2 ms on: under half a refresh
  REQUIRE(tracker.observe(same).discontinuity);
  REQUIRE(tracker.observe(kwin(4, 13)).valid);

  // An msc where there was none, then none again: the source changed.
  REQUIRE(tracker.observe(presented(5, 14)).discontinuity);
  REQUIRE(tracker.observe(presented(6, 15)).valid);
  REQUIRE(tracker.observe(kwin(7, 16)).discontinuity);
  REQUIRE(tracker.observe(kwin(8, 17)).valid);
}

TEST_CASE("a weaker source fails the gate whatever its numbers", "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pace_stats s;
  s.elapsed_seconds = 60.0;
  s.refresh_interval_ms = 16.666667;
  s.frames = 3600;
  s.displayed_presents = 3600;
  s.mean_ms = 16.666667;
  s.p50_ms = 16.666667;
  s.p99_ms = 16.7;
  s.max_ms = 16.7;
  s.source = mv::gfx::presentation_drop_source::presentation_feedback;
  REQUIRE(s.meets_linux_pr1_gate());
  s.source = mv::gfx::presentation_drop_source::interval_heuristic;
  REQUIRE_FALSE(s.meets_linux_pr1_gate());
  s.source = mv::gfx::presentation_drop_source::presentation_feedback;
  s.displayed_presents = 3400;  // frames drawn, a twentieth never reached the display
  REQUIRE_FALSE(s.meets_linux_pr1_gate());
  s.displayed_presents = 3600;
  s.statistics_unavailable_frames = 1;  // one frame the display never answered for
  REQUIRE_FALSE(s.meets_linux_pr1_gate());
  s.statistics_unavailable_frames = 0;
  s.elapsed_seconds = 59.9;
  REQUIRE_FALSE(s.meets_linux_pr1_gate());
}

TEST_CASE("a rewind or a standing msc is a discontinuity, not a drop",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_statistics tracker;
  (void)tracker.observe(presented(1, 500));
  const auto back = tracker.observe(presented(2, 400));
  REQUIRE(back.discontinuity);
  REQUIRE_FALSE(back.valid);
  const auto same = tracker.observe(presented(3, 400));
  REQUIRE(same.discontinuity);
  const auto jump = tracker.observe(presented(4, 400 + 5000));
  REQUIRE(jump.discontinuity);
  // The counter picked up from the jump: the next step is clean again.
  REQUIRE(tracker.observe(presented(5, 400 + 5001)).valid);

  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 3);
  pacer.on_feedback(presented(1, 500));
  pacer.on_feedback(presented(2, 501));
  pacer.on_feedback(presented(3, 300));
  const auto stats = pacer.stats();
  REQUIRE(stats.statistics_discontinuities == 1);
  REQUIRE(stats.dropped_frames == 0);
  // Not judged by the display, so not authoritative either.
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::interval_heuristic);
}

TEST_CASE("an msc step of one the hardware clock says took two refreshes is a miss",
          "[gfx][presentation_pacer]") {
  // Mutter's fallback: with no vblank sequence from the driver it steps the
  // surface's counter by 1, whatever the time, and keeps both flags.
  mv::gfx::presentation_statistics tracker;
  REQUIRE(tracker.observe(presented(1, 10)).baseline);
  auto late = presented(2, 11);
  late.timestamp_ns += k60_ns;  // shown at refresh 12
  const auto sample = tracker.observe(late);
  REQUIRE(sample.valid);
  REQUIRE(sample.missed_refreshes == 1);

  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 9);
  for (std::uint64_t id = 1; id <= 9; ++id) {
    auto s = presented(id, 100 + id);
    if (id >= 5) s.timestamp_ns += k60_ns;  // frame 5 a refresh late; the counter says 1
    pacer.on_feedback(s);
  }
  const auto stats = pacer.stats();
  REQUIRE(stats.displayed_presents == 8);
  REQUIRE(stats.dropped_frames == 1);
  REQUIRE(stats.missed_refreshes == 1);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
  REQUIRE_FALSE(stats.meets_linux_pr1_gate());
}

TEST_CASE("msc steps count only when the presentation clock agrees",
          "[gfx][presentation_pacer]") {
  // A counter that ran on (or changed output) while the clock moved one refresh.
  REQUIRE(mv::gfx::reconcile_feedback_misses(57, 16.7, 16.67) == 0);
  // A real stall: the clock moved two refreshes too.
  REQUIRE(mv::gfx::reconcile_feedback_misses(1, 33.4, 16.67) == 1);
  REQUIRE(mv::gfx::reconcile_feedback_misses(0, 16.7, 16.67) == 0);
  // No refresh known: nothing to reconcile against.
  REQUIRE(mv::gfx::reconcile_feedback_misses(3, 16.7, 0.0) == 3);

  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 2);
  pacer.on_feedback(presented(1, 10));
  auto odd = presented(2, 70);
  odd.timestamp_ns = presented(1, 10).timestamp_ns + k60_ns;
  pacer.on_feedback(odd);
  REQUIRE(pacer.stats().dropped_frames == 0);
  REQUIRE(pacer.stats().displayed_presents == 1);
}

TEST_CASE("an idle gap is not a miss: the window after it starts a new baseline",
          "[gfx][presentation_pacer]") {
  // The lab resets the window on every idle -> live edge outside a soak (a
  // soak never idles). The compositor's counter ran on through the gap; the
  // first feedback after the reset only seeds the counter again.
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 2);
  pacer.on_feedback(presented(1, 100));
  pacer.on_feedback(presented(2, 101));
  REQUIRE(pacer.stats().displayed_presents == 1);

  pacer.reset_window();  // ... a minute of rest ...
  commit(pacer, 3, 3);
  pacer.on_feedback(presented(3, 3700));  // 3600 vblanks later
  pacer.on_feedback(presented(4, 3701));
  pacer.on_feedback(presented(5, 3702));
  const auto stats = pacer.stats();
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.missed_refreshes == 0);
  REQUIRE(stats.displayed_presents == 2);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::presentation_feedback);
}

TEST_CASE("feedback for a frame from before reset_window is ignored",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 3);
  pacer.reset_window();
  REQUIRE(pacer.pending_feedback() == 0);
  // Frames 1-3 answer after the reset; frame 2's answer is a discard.
  pacer.on_feedback(presented(1, 10));
  pacer.on_feedback(discarded(2));
  pacer.on_feedback(presented(3, 14));
  auto stats = pacer.stats();
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.discarded_frames == 0);
  REQUIRE(stats.displayed_presents == 0);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::none);

  commit(pacer, 4, 2);
  pacer.on_feedback(presented(2, 12));  // late, and for the closed window
  pacer.on_feedback(presented(4, 20));
  pacer.on_feedback(presented(5, 21));
  stats = pacer.stats();
  REQUIRE(stats.displayed_presents == 1);
  REQUIRE(stats.dropped_frames == 0);
  // A duplicate answer for a frame already judged is not judged twice.
  pacer.on_feedback(presented(5, 23));
  REQUIRE(pacer.stats().displayed_presents == 1);
  REQUIRE(pacer.stats().dropped_frames == 0);
}

TEST_CASE("feedback still outstanding when the window closes is unavailable",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  commit(pacer, 1, 6);
  for (std::uint64_t id = 1; id <= 4; ++id) pacer.on_feedback(presented(id, 40 + id));
  REQUIRE(pacer.pending_feedback() == 2);
  REQUIRE(pacer.stats().source == mv::gfx::presentation_drop_source::presentation_feedback);
  pacer.expire_pending();
  const auto stats = pacer.stats();
  REQUIRE(pacer.pending_feedback() == 0);
  REQUIRE(stats.statistics_unavailable_frames == 2);
  REQUIRE(stats.displayed_presents == 3);
  REQUIRE(stats.source == mv::gfx::presentation_drop_source::interval_heuristic);
}

TEST_CASE("a compositor that stops answering cannot grow the pending ring",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(k60);
  const auto cap = static_cast<int>(mv::gfx::presentation_pacer::pending_capacity);
  for (int i = 0; i < cap + 5; ++i) {
    pacer.frame_begin();
    pacer.frame_end(static_cast<std::uint64_t>(i + 1));
  }
  REQUIRE(pacer.pending_feedback() == mv::gfx::presentation_pacer::pending_capacity);
  // Frame 1 was the baseline (no interval): pushing it out counts nothing.
  REQUIRE(pacer.stats().statistics_unavailable_frames == 4);
  REQUIRE(pacer.stats().source == mv::gfx::presentation_drop_source::interval_heuristic);
}

TEST_CASE("an unanswered frame the clock saw as late is inferred as missed",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(1.0 / 240.0);  // 4.17 ms
  pacer.frame_begin();
  pacer.frame_end(1);
  std::this_thread::sleep_for(20ms);
  pacer.frame_begin();
  pacer.frame_end(2);
  pacer.expire_pending();
  const auto stats = pacer.stats();
  REQUIRE(stats.statistics_unavailable_frames == 1);
  REQUIRE(stats.dropped_frames == 1);
  REQUIRE(stats.missed_refreshes >= 3);
}

TEST_CASE("reset_window clears drops but keeps the refresh interval",
          "[gfx][presentation_pacer]") {
  mv::gfx::presentation_pacer pacer;
  pacer.begin_session(1.0 / 120.0);
  commit(pacer, 1, 2);
  pacer.on_feedback(presented(1, 10));
  pacer.on_feedback(discarded(2));
  REQUIRE(pacer.stats().dropped_frames == 1);
  pacer.reset_window();
  const auto stats = pacer.stats();
  REQUIRE(stats.frames == 0);
  REQUIRE(stats.dropped_frames == 0);
  REQUIRE(stats.discarded_frames == 0);
  REQUIRE(stats.refresh_interval_ms > 8.0);
  REQUIRE(stats.refresh_interval_ms < 8.5);
}

TEST_CASE("the idle gate needs a minute, no presents and no input", "[gfx][presentation_pacer]") {
  mv::gfx::presentation_idle_stats idle;
  idle.elapsed_seconds = 60.0;
  idle.cpu_percent = 0.4;
  REQUIRE(idle.meets_linux_pr1_gate());
  idle.presents = 1;
  REQUIRE_FALSE(idle.meets_linux_pr1_gate());
  idle.presents = 0;
  idle.cpu_percent = 1.5;
  REQUIRE_FALSE(idle.meets_linux_pr1_gate());
  idle.cpu_percent = -1.0;  // not measured
  REQUIRE_FALSE(idle.meets_linux_pr1_gate());
}
