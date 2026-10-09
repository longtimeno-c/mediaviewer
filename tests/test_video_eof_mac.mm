// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #43: Space (or K, or the transport's Play) on a clip that has played to
// its natural end restarts it from the beginning, once, on the real Metal
// player. Issue #230: a paused or ended clip parks its player threads. Driven the way present_lab_mac drives it: acquire_frame only while
// needs_present(), at a display-link cadence, with the toggle applied as
// "playing ? pause : play". Synthetic clips (clip_fixture.h), so no corpus.
#include "catch_compat.h"

#import <Metal/Metal.h>

#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "player/media_source.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;
using mv::player::play_state;
using mv::player::time_ns;

namespace {

using clock_type = std::chrono::steady_clock;
constexpr time_ns kVblankNs = 16'666'667;

struct presented {
  int frames = 0;
  time_ns first_pts = -1;
  time_ns last_pts = -1;
};

// One render-thread tick per ~vblank until `done` holds or the deadline passes.
template <class Done>
presented run(mv::player::media_source& source, double seconds, Done done) {
  presented out;
  const auto deadline = clock_type::now() + std::chrono::duration<double>(seconds);
  while (clock_type::now() < deadline && !done(out)) {
    if (source.needs_present()) {
      if (auto* frame = source.acquire_frame(1, kVblankNs)) {
        if (out.first_pts < 0) out.first_pts = frame->pts_ns;
        out.last_pts = frame->pts_ns;
        ++out.frames;
        source.release_frame(frame);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  return out;
}

// present_lab_mac's anim_toggle_seq branch for a clip.
void toggle(mv::player::media_source& source) {
  if (source.state() == play_state::playing) source.pause();
  else source.play();
}

struct source_deleter {
  void operator()(mv::player::media_source* s) const noexcept { mv::player::close_media(s); }
};
using source_ptr = std::unique_ptr<mv::player::media_source, source_deleter>;

source_ptr open(const std::string& path, id<MTLDevice> device) {
  auto opened = mv::player::open_media(path.c_str(), (__bridge void*)device);
  REQUIRE(opened);
  source_ptr source(opened.value());
  source->set_muted(true);  // the Core Audio clock still runs at gain 0
  return source;
}

void play_to_end_then_restart(bool audio) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir(audio ? "eof_av" : "eof_v");
  const auto path = utf8(dir / "clip.mp4");
  fx::spec spec;
  spec.frames = 30;  // one second at 30 fps: short enough to reach EOF quickly
  spec.audio = audio;
  REQUIRE(fx::make(path, spec));

  auto source = open(path, device);
  const time_ns duration = source->info().duration_ns;
  REQUIRE(duration > 900'000'000);
  source->play();

  // Natural EOF: the state machine must say ended, not sit at "playing".
  const auto first = run(*source, 5.0, [&](const presented&) {
    return source->state() == play_state::ended;
  });
  REQUIRE(source->state() == play_state::ended);
  REQUIRE(first.last_pts > duration / 2);
  CHECK_FALSE(source->needs_present());  // PR 1's idle gate: no present at EOF

  for (int round = 0; round < 2; ++round) {
    CAPTURE(round);
    // Space at EOF. It must play again from zero, not stay parked on the end.
    toggle(*source);
    REQUIRE(source->state() == play_state::playing);
    REQUIRE(source->needs_present());
    const auto restarted = run(*source, 5.0, [&](const presented& p) {
      return p.last_pts >= duration / 2;
    });
    REQUIRE(restarted.frames > 0);
    CHECK(restarted.first_pts < 100'000'000);  // the first frame shown is the start
    REQUIRE(restarted.last_pts >= duration / 2);  // and the clock runs from there
    REQUIRE(source->state() == play_state::playing);

    // And it plays out once more, rather than looping or freezing.
    run(*source, 5.0, [&](const presented&) { return source->state() == play_state::ended; });
    REQUIRE(source->state() == play_state::ended);
  }
}

}  // namespace

TEST_CASE("Space at natural EOF restarts a clip with audio from the start", "[video][mac][eof]") {
  play_to_end_then_restart(true);
}

TEST_CASE("Space at natural EOF restarts a silent clip from the start", "[video][mac][eof]") {
  play_to_end_then_restart(false);
}

TEST_CASE("Space mid-clip pauses and resumes in place, not from the start", "[video][mac][eof]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("eof_mid");
  const auto path = utf8(dir / "clip.mp4");
  fx::spec spec;
  spec.frames = 90;  // three seconds
  REQUIRE(fx::make(path, spec));

  auto source = open(path, device);
  source->play();
  const auto before = run(*source, 5.0, [](const presented& p) { return p.last_pts >= 1'000'000'000; });
  REQUIRE(before.last_pts >= 1'000'000'000);

  toggle(*source);  // pause
  REQUIRE(source->state() == play_state::paused);
  const time_ns paused_at = source->position_ns();

  toggle(*source);  // resume
  REQUIRE(source->state() == play_state::playing);
  const auto after = run(*source, 5.0, [](const presented& p) { return p.frames >= 3; });
  REQUIRE(after.frames >= 3);
  CHECK(after.first_pts >= paused_at - 100'000'000);
}

namespace {

// Context switches the whole process makes per second over `seconds` with no
// render ticks: a sleep-polling loop shows up as its wake rate (issue #230).
// Process-wide on purpose -- Core Audio's own threads count too, which is
// what the threshold leaves room for. getrusage's ru_nvcsw is always zero on
// macOS, so this asks the kernel's task info instead.
struct idle_cost {
  double wakes_per_second = 0;
  double cpu_ms_per_second = 0;
};

idle_cost measure_idle(double seconds) {
  proc_taskinfo before{}, after{};
  const int size = static_cast<int>(sizeof(proc_taskinfo));
  REQUIRE(proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &before, size) == size);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  REQUIRE(proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &after, size) == size);
  idle_cost out;
  out.wakes_per_second = static_cast<double>(after.pti_csw - before.pti_csw) / seconds;
  // pti_total_* are mach absolute time units, not nanoseconds.
  mach_timebase_info_data_t timebase{};
  mach_timebase_info(&timebase);
  const auto cpu = (after.pti_total_user + after.pti_total_system) -
                   (before.pti_total_user + before.pti_total_system);
  out.cpu_ms_per_second =
      static_cast<double>(cpu) * timebase.numer / timebase.denom / 1e6 / seconds;
  return out;
}

// The sleep-polling player measured ~1,750 wakes/s paused and ~1,150 ended on
// an M-series Mac (issue #230); parked, it is single digits. The bound leaves
// room for Core Audio and a busy machine while still failing any poll loop.
constexpr double kParkedWakesPerSecond = 200.0;

}  // namespace

TEST_CASE("A paused clip parks its player threads", "[video][mac][idle]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("idle_paused");
  const auto path = utf8(dir / "clip.mp4");
  fx::spec spec;
  spec.frames = 90;
  REQUIRE(fx::make(path, spec));

  auto source = open(path, device);
  // Opened paused on its poster frame (issue #44): let the poster land and the
  // packet queues fill, which is the state arrowing through a folder leaves.
  const auto poster = run(*source, 1.0, [](const presented& p) { return p.frames >= 1; });
  REQUIRE(poster.frames >= 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(source->state() != play_state::playing);

  const auto cost = measure_idle(1.0);
  std::printf("idle: paused clip %.0f wakes/s, %.2f ms CPU/s\n", cost.wakes_per_second,
              cost.cpu_ms_per_second);
  CHECK(cost.wakes_per_second < kParkedWakesPerSecond);

  // Parked is not stuck: play still runs the clip from where it stood.
  source->play();
  const auto played = run(*source, 5.0, [](const presented& p) { return p.last_pts >= 500'000'000; });
  CHECK(played.last_pts >= 500'000'000);
}

TEST_CASE("An ended clip parks its player threads", "[video][mac][idle]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("idle_ended");
  const auto path = utf8(dir / "clip.mp4");
  fx::spec spec;
  spec.frames = 30;
  REQUIRE(fx::make(path, spec));

  auto source = open(path, device);
  source->play();
  run(*source, 5.0, [&](const presented&) { return source->state() == play_state::ended; });
  REQUIRE(source->state() == play_state::ended);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const auto cost = measure_idle(1.0);
  std::printf("idle: ended clip %.0f wakes/s, %.2f ms CPU/s\n", cost.wakes_per_second,
              cost.cpu_ms_per_second);
  CHECK(cost.wakes_per_second < kParkedWakesPerSecond);

  // And Space still restarts it (issue #43) from the parked state.
  toggle(*source);
  const auto restarted = run(*source, 5.0, [](const presented& p) { return p.frames >= 3; });
  CHECK(restarted.frames >= 3);
}
