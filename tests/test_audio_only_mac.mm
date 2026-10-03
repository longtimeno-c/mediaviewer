// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Audio-only playback on the real Metal player (docs/plans/audio-and-documents.md
// §2.2): an M4A opens as a clip whose picture is the music card, plays against
// the audio clock to its end, seeks, steps by five seconds, and a FairPlay M4P
// opens to the padlock and never plays.
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

struct source_deleter {
  void operator()(mv::player::media_source* s) const noexcept { mv::player::close_media(s); }
};
using source_ptr = std::unique_ptr<mv::player::media_source, source_deleter>;

source_ptr open(const std::string& path, id<MTLDevice> device) {
  auto opened = mv::player::open_media(path.c_str(), (__bridge void*)device);
  REQUIRE(opened);
  source_ptr source(opened.value());
  source->set_muted(true);
  return source;
}

// Ticks like the render thread; returns how many frames it was handed.
template <class Done>
int run(mv::player::media_source& source, double seconds, Done done) {
  int frames = 0;
  const auto deadline = clock_type::now() + std::chrono::duration<double>(seconds);
  while (clock_type::now() < deadline && !done()) {
    if (source.needs_present()) {
      if (auto* frame = source.acquire_frame(1, kVblankNs)) {
        ++frames;
        source.release_frame(frame);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  return frames;
}

}  // namespace

TEST_CASE("An M4A plays as an audio-only clip to its end", "[audio][mac]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("audio_play");
  const auto path = utf8(dir / "tone.m4a");
  REQUIRE(fx::make_audio(path, 1.5));

  auto source = open(path, device);
  const auto info = source->info();
  CHECK(info.audio_only);
  CHECK_FALSE(info.drm_protected);
  CHECK(info.has_audio);
  CHECK(info.video_tracks == 0);
  CHECK(info.video.width == 1280);
  CHECK(info.video.height == 720);
  REQUIRE(info.duration_ns > 1'400'000'000);

  // The still is up before play: a seek's preview, like a clip's first frame.
  CHECK(run(*source, 2.0, [&] { return !source->needs_present(); }) == 1);

  source->play();
  run(*source, 5.0, [&] { return source->state() == play_state::ended; });
  REQUIRE(source->state() == play_state::ended);
  CHECK(source->position_ns() >= info.duration_ns - 50'000'000);
  CHECK_FALSE(source->needs_present());  // idle at the end
}

TEST_CASE("An audio-only clip seeks, pauses in place and steps by five seconds", "[audio][mac]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("audio_seek");
  const auto path = utf8(dir / "tone.m4a");
  REQUIRE(fx::make_audio(path, 12.0));
  auto source = open(path, device);
  run(*source, 2.0, [&] { return !source->needs_present(); });

  source->seek(4'000'000'000, true);
  CHECK(run(*source, 2.0, [&] { return !source->needs_present(); }) == 1);  // the still again
  CHECK(source->position_ns() == 4'000'000'000);

  source->play();
  run(*source, 0.6, [] { return false; });
  source->pause();
  const auto paused = source->position_ns();
  CHECK(paused > 4'300'000'000);
  CHECK(paused < 5'000'000'000);

  source->step(1);
  run(*source, 2.0, [&] { return !source->needs_present(); });
  CHECK(source->position_ns() == paused + mv::player::kAudioStepNs);
  source->step(-1);
  run(*source, 2.0, [&] { return !source->needs_present(); });
  CHECK(source->position_ns() == paused);
}

TEST_CASE("A FairPlay M4P opens to the padlock and never plays", "[audio][mac]") {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) SKIP("no Metal device");
  scratch_dir dir("audio_fairplay");
  const auto path = utf8(dir / "song.m4p");
  REQUIRE(fx::make_audio(path, 2.0, true));

  auto source = open(path, device);
  const auto info = source->info();
  CHECK(info.audio_only);
  CHECK(info.drm_protected);
  CHECK_FALSE(info.has_audio);
  CHECK(info.duration_ns == 0);
  CHECK(run(*source, 2.0, [&] { return !source->needs_present(); }) == 1);
  source->play();
  run(*source, 1.0, [&] { return source->state() == play_state::ended; });
  CHECK(source->state() == play_state::ended);
}
