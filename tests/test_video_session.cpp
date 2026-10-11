// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #236: a pending clip whose generation went stale is retired, and a
// stale one is refused at publish. Headless, with a fake source; both hosts.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "abi/video_session.h"

using namespace mv::player;

namespace {
class fake_source final : public media_source {
 public:
  explicit fake_source(std::atomic<int>& closed) : closed_(closed) {}
  ~fake_source() override { closed_.fetch_add(1); }
  media_info info() const noexcept override { return {}; }
  play_state state() const noexcept override { return play_state::paused; }
  time_ns position_ns() const noexcept override { return 0; }
  void play() noexcept override {}
  void pause() noexcept override {}
  void seek(time_ns, bool) noexcept override {}
  void step(int) noexcept override {}
  void set_rate(double) noexcept override {}
  void set_volume(float) noexcept override {}
  void set_muted(bool) noexcept override {}
  void select_audio_track(std::uint32_t) noexcept override {}
  void set_loop(time_ns, time_ns) noexcept override {}
  video_frame* acquire_frame(std::uint32_t, time_ns) noexcept override { return nullptr; }
  void release_frame(video_frame*) noexcept override {}
  clock_stats stats() const noexcept override { return {}; }
 private:
  std::atomic<int>& closed_;
};

// The cleaner thread closes retired sources; give it a moment.
bool eventually(const std::atomic<int>& closed, int want) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (closed.load() != want && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return closed.load() == want;
}

void tick(mv::abi::video_session& session, std::uint32_t generation) {
  video_frame frame; bool active = false;
  (void)session.tick(generation, -1, frame, active);
}
}  // namespace

TEST_CASE("a pending clip whose generation went stale is retired on the next tick", "[video][session]") {
  std::atomic<int> closed{0};
  mv::abi::video_session session;
  tick(session, 3);
  REQUIRE(session.publish(new fake_source(closed), 3));
  REQUIRE(session.open());
  // mv_video_close (or a cancel after the loader's last check) bumps before adoption.
  tick(session, 4);
  REQUIRE(eventually(closed, 1));
  REQUIRE_FALSE(session.open());
}

TEST_CASE("publish refuses a source older than the generation the session has seen", "[video][session]") {
  std::atomic<int> closed{0};
  mv::abi::video_session session;
  tick(session, 7);
  REQUIRE_FALSE(session.publish(new fake_source(closed), 6));
  REQUIRE(closed.load() == 1);  // closed on the publishing thread, not queued
  REQUIRE_FALSE(session.open());
}

TEST_CASE("a clip published ahead of the tick's generation is kept and adopted", "[video][session]") {
  std::atomic<int> closed{0};
  mv::abi::video_session session;
  tick(session, 5);
  // The loader ran at a bump the render thread has not read yet.
  REQUIRE(session.publish(new fake_source(closed), 6));
  tick(session, 5);
  REQUIRE(session.open());
  tick(session, 6);
  REQUIRE(session.open());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(closed.load() == 0);
}

TEST_CASE("generation comparison survives wrap-around", "[video][session]") {
  std::atomic<int> closed{0};
  mv::abi::video_session session;
  tick(session, 0xFFFF'FFFFu);
  REQUIRE(session.publish(new fake_source(closed), 0xFFFF'FFFFu));
  tick(session, 0);  // one past the wrap: the pending clip is now stale
  REQUIRE(eventually(closed, 1));
  REQUIRE_FALSE(session.publish(new fake_source(closed), 0xFFFF'FFFEu));
  REQUIRE(closed.load() == 2);
}
