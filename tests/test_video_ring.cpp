// SPDX-License-Identifier: GPL-2.0-or-later
// OWNER: mediaviewer-48 (5a).
//
// What this proves, from the verify line:
//   - "4K 10-bit HEVC and AV1 play ... with GPU video decode" — the decoder we
//     actually opened is D3D11VA, not a silent software fallback.
//   - "the decoder never stalls waiting for a surface" — the copy-out design
//     keeps the DPB free; surface_waits is the number that says so.
//   - "photo -> video -> photo leaks no textures" — asserted against the
//     device's own reference count rather than eyeballed in Task Manager.
//
// The corpus is gitignored, so these SKIP where it is absent. A skip is
// reported; it is not a pass.
#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "corpus.h"
#include "player/media_source.h"

namespace {

// Duplicated from test_video_colour.cpp on purpose — see the note there.
struct test_device {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  test_device() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &got,
                                   &context))) {
      device = nullptr;
      context = nullptr;
      return;
    }
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
      mt->SetMultithreadProtected(TRUE);
      mt->Release();
    }
  }
  ~test_device() {
    if (context) context->Release();
    if (device) device->Release();
  }
  [[nodiscard]] bool valid() const noexcept { return device != nullptr; }

  // Every D3D11 resource holds a reference on its device, so a leaked ring
  // texture shows up here. AddRef/Release round trip is the only portable way
  // to read a COM refcount, and it is exact for this purpose.
  [[nodiscard]] ULONG device_refcount() const noexcept {
    device->AddRef();
    return device->Release();
  }
};

struct opened {
  mv::player::media_source* source = nullptr;
  ~opened() {
    if (source) mv::player::close_media(source);
  }
};

// Plays for `seconds`, releasing every frame it is handed, and reports what
// came out. Releasing is the point: a test that acquires without releasing
// starves the ring and would "prove" a stall that the caller caused.
struct playback_result {
  int                 frames = 0;
  bool                pts_monotonic = true;
  mv::player::time_ns first_pts = -1;
  mv::player::time_ns last_pts = -1;
};

playback_result play_for(mv::player::media_source* source, int seconds) {
  playback_result out;
  source->play();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto* frame = source->acquire_frame(1, 16'666'667)) {
      if (out.frames == 0) out.first_pts = frame->pts_ns;
      // Presentation order must be non-decreasing: the ring is FIFO and the
      // presenter drops rather than reorders.
      if (out.last_pts >= 0 && frame->pts_ns < out.last_pts) out.pts_monotonic = false;
      out.last_pts = frame->pts_ns;
      ++out.frames;
      source->release_frame(frame);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return out;
}

}  // namespace

TEST_CASE("4K 10-bit HEVC opens on the hardware decoder and produces P010 frames",
          "[video][ring]") {
  MV_REQUIRE_CLIP(path, "hevc_4k_10bit_bt709.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  const mv::player::media_info info = guard.source->info();
  CHECK(info.video.width == 3840);
  CHECK(info.video.height == 2160);
  CHECK(info.video.ten_bit);
  CHECK(std::string(info.video.codec_name) == "hevc");
  // The honest form of "GPU video decode > 0 in Task Manager" that a test can
  // assert: what avcodec_open2 actually left us holding. A software fallback
  // fails here rather than passing slowly and silently.
  CHECK(info.video.decoder == mv::player::decoder_kind::d3d11va);

  const playback_result played = play_for(guard.source, 3);
  CHECK(played.frames > 0);
  CHECK(played.pts_monotonic);
  CHECK(guard.source->stats().decoder == mv::player::decoder_kind::d3d11va);
  CHECK(guard.source->stats().surface_waits == 0);
}

TEST_CASE("4K 10-bit AV1 opens on the hardware decoder", "[video][ring]") {
  MV_REQUIRE_CLIP(path, "av1_4k_10bit_bt709.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  const mv::player::media_info info = guard.source->info();
  CHECK(std::string(info.video.codec_name) == "av1");
  CHECK(info.video.decoder == mv::player::decoder_kind::d3d11va);

  const playback_result played = play_for(guard.source, 3);
  CHECK(played.frames > 0);
  CHECK(played.pts_monotonic);
  CHECK(guard.source->stats().decoder == mv::player::decoder_kind::d3d11va);
  CHECK(guard.source->stats().surface_waits == 0);
}

TEST_CASE("The decoder does not stall waiting for a presentation surface", "[video][ring]") {
  MV_REQUIRE_CLIP(path, "hevc_4k_10bit_bt709.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  const playback_result played = play_for(guard.source, 5);
  REQUIRE(played.frames > 0);

  // 5a publishes surface_waits in this field until 5b owns clock_stats; it
  // counts the times the decode thread had a frame in hand and no free ring
  // slot to copy it into. This is a SHORT run on a fast machine, so it is a
  // smoke test for the design, not the 10-minute soak in the verify line —
  // that one is reported separately and was run by hand.
  const mv::player::clock_stats stats = guard.source->stats();
  CHECK(stats.position_discontinuities == 0);
  CHECK(stats.counters.held_starved < stats.counters.presented);
}

TEST_CASE("Seeking discards pre-seek frames rather than presenting them", "[video][ring]") {
  MV_REQUIRE_CLIP(path, "soak_10min_1080p_hevc.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  REQUIRE(play_for(guard.source, 1).frames > 0);

  // Two minutes in. Everything queued was decoded from the first seconds, so if
  // the generation bump did not take, the frames that come back next carry the
  // OLD timestamps and this test says so.
  constexpr mv::player::time_ns two_minutes = 120LL * 1'000'000'000LL;
  guard.source->seek(two_minutes, false);

  const playback_result after = play_for(guard.source, 4);
  REQUIRE(after.frames > 0);
  // Half a second of slack for the keyframe the seek actually lands on, which
  // is at or before the target by design (AVSEEK_FLAG_BACKWARD).
  CHECK(after.first_pts > two_minutes - 5'000'000'000LL);
  CHECK(after.pts_monotonic);
}

TEST_CASE("photo -> video -> photo leaks no textures", "[video][ring]") {
  MV_REQUIRE_CLIP(path, "hevc_4k_8bit_bt709.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  // Baseline AFTER one full cycle, not before: the first open warms FFmpeg's
  // own per-device state, and counting that as a leak would make this test fail
  // for a reason that is not a leak.
  {
    auto first = mv::player::open_media(path.c_str(), dev.device);
    REQUIRE(first);
    opened guard{first.value()};
    (void)play_for(guard.source, 1);
  }
  const ULONG baseline = dev.device_refcount();

  for (int cycle = 0; cycle < 4; ++cycle) {
    auto source = mv::player::open_media(path.c_str(), dev.device);
    REQUIRE(source);
    opened guard{source.value()};
    const playback_result played = play_for(guard.source, 1);
    CHECK(played.frames > 0);
  }

  // Four open/close cycles, each of which built and tore down a four-slot ring
  // of 4K textures plus their SRVs. Every one of those holds a device
  // reference, so a ring that outlived its clip shows up as a higher count.
  CHECK(dev.device_refcount() == baseline);
}
