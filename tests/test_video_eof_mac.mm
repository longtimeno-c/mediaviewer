// SPDX-License-Identifier: GPL-2.0-or-later
// Issue #43: Space (or K, or the transport's Play) on a clip that has played to
// its natural end restarts it from the beginning, once, on the real Metal
// player. Driven the way present_lab_mac drives it: acquire_frame only while
// needs_present(), at a display-link cadence, with the toggle applied as
// "playing ? pause : play". Synthetic clips (clip_fixture.h), so no corpus.
#include "catch_compat.h"

#import <Metal/Metal.h>

#include <chrono>
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
