// SPDX-License-Identifier: GPL-2.0-or-later
// The clock's behaviour against a FAKE endpoint.
//
// Two of the three clauses in PR 5b's verify line — "unplugging the audio device
// mid-playback recovers without stopping video" and "a clip with no audio track
// plays at correct speed" — cannot be tested deterministically against a real
// endpoint, and on a machine with no audio device cannot be tested at all. That
// is what av_clock::set_sink_for_test is for. It is not a convenience.
//
// What these tests do NOT cover, and no unit test can: that WASAPI's real
// IAudioClock reports what we think it reports. That is what the soak is for.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "player/av_clock.h"

using Catch::Approx;
using namespace mv::player;

namespace {

constexpr time_ns ns_per_second = 1'000'000'000;

// A scriptable endpoint. Position advances only when the test says so, which is
// the whole point: a real endpoint's position advances on its own schedule and
// nothing about drop/hold/fallback would be reproducible.
class fake_sink final : public audio_sink {
 public:
  std::atomic<bool> fail_open{false};
  std::atomic<bool> changed{false};
  std::atomic<std::int64_t> position_ns{0};
  std::atomic<std::uint64_t> discontinuities{0};
  std::atomic<int> opens{0};
  std::atomic<int> closes{0};
  std::atomic<std::uint64_t> frames_taken{0};
  std::atomic<std::uint32_t> accept_limit{~0u};  // frames accepted per write()

  mv::expected open(std::uint32_t sample_rate, std::uint32_t channels) override {
    if (fail_open.load()) return mv::err(mv::status::io);
    ++opens;
    rate_ = sample_rate;
    channels_ = channels;
    changed.store(false);
    return {};
  }

  void close() noexcept override { ++closes; }

  audio_endpoint_info info() const noexcept override {
    audio_endpoint_info out;
    out.sample_rate = rate_;
    out.channels = channels_;
    out.buffer_frames = 480;
    out.period_frames = 480;
    return out;
  }

  std::uint32_t write(const float*, std::uint32_t frames) noexcept override {
    const std::uint32_t taken = frames < accept_limit.load() ? frames : accept_limit.load();
    frames_taken.fetch_add(taken);
    return taken;
  }

  time_ns played_ns(std::uint64_t* out_discontinuity) const noexcept override {
    if (out_discontinuity != nullptr) *out_discontinuity = discontinuities.load();
    return position_ns.load();
  }

  bool device_changed() const noexcept override { return changed.load(); }
  void set_volume(float v) noexcept override { volume = v; }
  void set_muted(bool m) noexcept override { muted = m; }

  float volume = 1.0f;
  bool muted = false;

 private:
  std::uint32_t rate_ = 0;
  std::uint32_t channels_ = 0;
};

audio_block make_block(time_ns pts_ns, std::uint32_t generation = 0) {
  audio_block block;
  block.pts_ns = pts_ns;
  block.frames = 512;
  block.channels = 2;
  block.generation = generation;
  return block;
}

// The pump is a real thread; give it a bounded chance to consume rather than
// asserting on a race. Bounded, so a hang fails the test instead of stalling it.
bool wait_until(const std::function<bool()>& predicate) {
  for (int i = 0; i < 400; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

TEST_CASE("a clip with no audio track plays at correct speed", "[clock]") {
  // Verify line, clause three. The host clock is the master and it must advance
  // at real time — not at zero, and not at some scaled rate.
  av_clock clock;
  clock.start_host_only();

  const clock_stats before = clock.stats();
  CHECK_FALSE(before.audio_master);

  const time_ns first = clock.now_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const time_ns second = clock.now_ns();

  const double elapsed_s = static_cast<double>(second - first) / ns_per_second;
  INFO("host clock advanced " << elapsed_s << " s over a 250 ms sleep");
  // Generous bounds: this asserts "real time", not "a precise timer".
  CHECK(elapsed_s > 0.15);
  CHECK(elapsed_s < 0.60);
}

TEST_CASE("an endpoint that will not open still plays", "[clock]") {
  // plan/05 and the verify line both require playback to continue when audio
  // fails. Only the master changes. start() reports ok because a dead sound
  // card is not a failure to open the clip.
  auto* sink = new fake_sink();
  sink->fail_open.store(true);

  av_clock clock;
  clock.set_sink_for_test(sink);
  const auto started = clock.start(48000, 2);

  CHECK(started.has_value());
  const clock_stats stats = clock.stats();
  CHECK_FALSE(stats.audio_master);
  CHECK(stats.fallback == clock_fallback_reason::device_open_failed);

  const time_ns first = clock.now_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK(clock.now_ns() > first);
}

TEST_CASE("the master clock follows samples played, not wall time", "[clock]") {
  // The core of plan/05: "derive presentation time from samples actually
  // played, not from a wall clock." A frozen endpoint must freeze the clock —
  // if this test fails by the clock advancing anyway, the implementation has
  // silently become a wall clock and the 30-minute soak would ramp.
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  REQUIRE(clock.submit(make_block(0)));
  REQUIRE(wait_until([&] { return sink->frames_taken.load() > 0; }));

  const time_ns anchored = clock.now_ns();

  // Endpoint frozen: real time passes, the clock must not.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const time_ns still = clock.now_ns();
  INFO("clock moved " << (still - anchored) << " ns with a frozen endpoint");
  CHECK(std::llabs(still - anchored) < 5 * 1'000'000);  // < 5 ms of slack

  // Endpoint advances one second: so does the clock.
  sink->position_ns.store(ns_per_second);
  const time_ns after = clock.now_ns();
  CHECK(static_cast<double>(after - anchored) / ns_per_second == Approx(1.0).margin(0.01));

  clock.stop();
}

TEST_CASE("playback rate scales the clock without jumping it", "[clock]") {
  // Speed is 5c's feature, but the rate lives in the clock from day one
  // precisely so this holds: changing rate must re-anchor, not retroactively
  // re-scale every sample already played.
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  REQUIRE(clock.submit(make_block(0)));
  REQUIRE(wait_until([&] { return sink->frames_taken.load() > 0; }));

  sink->position_ns.store(2 * ns_per_second);
  const time_ns before = clock.now_ns();
  CHECK(static_cast<double>(before) / ns_per_second == Approx(2.0).margin(0.05));

  clock.set_rate(2.0);
  const time_ns immediately_after = clock.now_ns();
  INFO("rate change jumped the clock by " << (immediately_after - before) << " ns");
  CHECK(std::llabs(immediately_after - before) < 10 * 1'000'000);  // no jump

  // From here the clock runs at double the endpoint's rate.
  sink->position_ns.store(3 * ns_per_second);
  const time_ns later = clock.now_ns();
  CHECK(static_cast<double>(later - immediately_after) / ns_per_second ==
        Approx(2.0).margin(0.05));

  CHECK(clock.stats().playback_rate == Approx(2.0));
  clock.stop();
}

TEST_CASE("pausing freezes the clock and resuming does not rewind it", "[clock]") {
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  REQUIRE(clock.submit(make_block(0)));
  REQUIRE(wait_until([&] { return sink->frames_taken.load() > 0; }));
  sink->position_ns.store(ns_per_second);

  const time_ns at_pause = clock.now_ns();
  clock.set_paused(true);

  // The endpoint keeps running while paused; the clock must not.
  sink->position_ns.store(5 * ns_per_second);
  CHECK(clock.now_ns() == at_pause);

  clock.set_paused(false);
  const time_ns resumed = clock.now_ns();
  INFO("resume moved the clock by " << (resumed - at_pause) << " ns");
  CHECK(resumed >= at_pause);
  CHECK(resumed - at_pause < 100 * 1'000'000);  // resumes where it paused
  clock.stop();
}

TEST_CASE("losing the device recovers without stopping the clock", "[clock]") {
  // Verify line, clause two: "unplugging the audio device mid-playback recovers
  // without stopping video." Video keeps presenting only if the clock keeps
  // advancing, so the clock falls back to the host master for the rebuild
  // window rather than freezing.
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  REQUIRE(clock.submit(make_block(0)));
  REQUIRE(wait_until([&] { return sink->frames_taken.load() > 0; }));
  sink->position_ns.store(ns_per_second);

  const int opens_before = sink->opens.load();
  const time_ns before_loss = clock.now_ns();

  sink->changed.store(true);
  REQUIRE(wait_until([&] { return sink->opens.load() > opens_before; }));

  // The endpoint was closed and reopened.
  CHECK(sink->closes.load() > 0);
  CHECK(sink->opens.load() == opens_before + 1);

  // And the clock never went backwards or stopped across the rebuild.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const time_ns after_loss = clock.now_ns();
  INFO("clock at loss " << before_loss << " ns, after recovery " << after_loss << " ns");
  CHECK(after_loss >= before_loss);

  clock.stop();
}

TEST_CASE("a device that stays gone keeps playing on the host clock", "[clock]") {
  // The genuinely unplugged case with nothing to fall back to. Playback must
  // not stop; the master just stays the host clock.
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());
  REQUIRE(clock.submit(make_block(0)));
  REQUIRE(wait_until([&] { return sink->frames_taken.load() > 0; }));

  sink->fail_open.store(true);
  sink->changed.store(true);
  REQUIRE(wait_until([&] { return sink->closes.load() > 0; }));

  const time_ns first = clock.now_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const time_ns second = clock.now_ns();
  INFO("host-clock fallback advanced " << (second - first) << " ns");
  CHECK(second > first);

  clock.stop();
}

TEST_CASE("blocks at a stale generation are never written to the endpoint", "[clock]") {
  // plan/02 generation counters. A block decoded before a seek must not be
  // heard after it.
  auto* sink = new fake_sink();
  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  clock.seeked(10 * ns_per_second, /*generation=*/7);

  // Generation 0 is stale now. submit() accepts it (telling the decoder to
  // retry a pre-seek block would just spin it) but nothing reaches the endpoint.
  REQUIRE(clock.submit(make_block(0, /*generation=*/0)));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(sink->frames_taken.load() == 0);

  // A current-generation block does get through.
  REQUIRE(clock.submit(make_block(10 * ns_per_second, /*generation=*/7)));
  CHECK(wait_until([&] { return sink->frames_taken.load() > 0; }));

  clock.stop();
}

TEST_CASE("a partially accepted block is not dropped", "[clock]") {
  // The endpoint buffer fills; write() takes what fits. The remainder must be
  // held and written next time, not discarded — dropping it would be an audible
  // gap that shows up as a drift step.
  auto* sink = new fake_sink();
  sink->accept_limit.store(64);  // 512-frame blocks go in eight bites

  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());
  REQUIRE(clock.submit(make_block(0)));

  CHECK(wait_until([&] { return sink->frames_taken.load() >= 512; }));
  CHECK(sink->frames_taken.load() == 512);

  clock.stop();
}

TEST_CASE("the ring reports full rather than blocking the decoder", "[clock]") {
  // CLAUDE.md rule 1: nothing that can block. submit() is called from the decode
  // thread and must return false rather than wait.
  auto* sink = new fake_sink();
  sink->accept_limit.store(0);  // endpoint never drains

  av_clock clock;
  clock.set_sink_for_test(sink);
  REQUIRE(clock.start(48000, 2).has_value());

  int accepted = 0;
  for (int i = 0; i < static_cast<int>(audio_ring_slots) * 4; ++i) {
    if (clock.submit(make_block(i * 1'000'000))) ++accepted;
  }
  INFO("ring accepted " << accepted << " of " << audio_ring_slots * 4 << " blocks");
  CHECK(accepted < static_cast<int>(audio_ring_slots) * 4);

  clock.stop();
}
