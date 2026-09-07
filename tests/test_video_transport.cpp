// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "corpus.h"
#include "player/media_source.h"
#include "abi/native.h"
#include "gfx/device.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
namespace {
using source_ptr = std::unique_ptr<mv::player::media_source, decltype(&mv::player::close_media)>;
mv::player::time_ns next_frame(mv::player::media_source& source) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto* frame = source.acquire_frame(1, 16'666'667)) {
      const auto pts = frame->pts_ns; source.release_frame(frame); return pts;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return -1;
}
}
TEST_CASE("real clip seeks while paused and steps to adjacent frames", "[video][integration]") {
  MV_REQUIRE_CLIP(path, "av_transport.mp4");
  mv::gfx::device device; REQUIRE(device.create(nullptr));
  auto opened = mv::player::open_media(path.c_str(), device.d3d()); REQUIRE(opened);
  source_ptr source(opened.value(), mv::player::close_media); source->set_muted(true);
  source->seek(2'000'000'000, true);
  const auto first = next_frame(*source);
  REQUIRE(first >= 2'000'000'000); REQUIRE(first < 2'034'000'000);
  REQUIRE(source->state() == mv::player::play_state::paused);
  source->step(1); const auto forward = next_frame(*source);
  REQUIRE(forward > first); REQUIRE(forward - first <= 33'333'334);
  source->step(-1); const auto backward = next_frame(*source);
  REQUIRE(backward == first);
  source->seek(20'000'000'000, true); REQUIRE(next_frame(*source) >= 20'000'000'000);
  source.reset();
  opened = mv::player::open_media(path.c_str(), device.d3d()); REQUIRE(opened);
  source.reset(opened.value()); source->set_muted(true);
  REQUIRE(next_frame(*source) >= 20'000'000'000);
  source->seek(0, true); REQUIRE(next_frame(*source) == 0);
}
TEST_CASE("video ABI opens asynchronously and retires on navigation", "[abi][video][integration]") {
  MV_REQUIRE_CLIP(path, "av_transport.mp4");
  mv::gfx::device device; REQUIRE(device.create(nullptr));
  mv_session_t session = nullptr; REQUIRE(mv_session_create(nullptr, &session) == MV_OK);
  struct release_session { mv_session_t value; ~release_session() { (void)mv_session_release(value); } } cleanup{session};
  REQUIRE(mv::abi::attach_device(session, device.d3d()) == mv::status::ok);
  uint64_t job = 0; REQUIRE(mv_video_open(session, path.c_str(), &job) == MV_OK); REQUIRE(job != 0);
  mv::player::video_frame frame; bool active = false, shown = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !shown) {
    shown = mv::abi::poll_video(session, 16'666'667, frame, active);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(shown); REQUIRE(frame.width == 640); REQUIRE(active);
  mv_video_info info{}; REQUIRE(mv_video_get_info(session, &info) == MV_OK);
  REQUIRE(info.audio_tracks == 1); REQUIRE(info.video_tracks == 1);
  REQUIRE(mv_video_close(session) == MV_OK);
  (void)mv::abi::poll_video(session, 16'666'667, frame, active); REQUIRE_FALSE(active);
  uint32_t state = 99; REQUIRE(mv_video_state(session, &state) == MV_OK); REQUIRE(state == MV_PLAY_STOPPED);
  mv::abi::detach_device(session);
}
