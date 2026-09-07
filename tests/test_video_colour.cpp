// SPDX-License-Identifier: GPL-2.0-or-later
// OWNER: mediaviewer-48 (5a).
//
// What this proves: the colour description that reaches the shader is the
// STREAM'S, end to end — container VUI -> FFmpeg AVCOL_* -> colour_from_stream
// -> resolve_unspecified -> video_frame::colour. plan/05 calls a wrong matrix
// or range "the classic 'why is my video washed out' bug", and the only way to
// catch it is to read back what a real decode actually produced.
//
// The corpus lives in tools/testmedia/ and is GITIGNORED (plan/09: the corpus
// does not go in git), so these tests SKIP rather than fail where it is absent.
// A skip is reported; it is not a pass.
#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "corpus.h"
#include "player/media_source.h"

namespace {

// Duplicated in test_video_ring.cpp on purpose: a shared tests/ header is
// another file in someone else's ownership row, and 25 lines is cheaper than
// the round trip. The CORPUS finder is no longer duplicated — it moved to
// tests/corpus.h, because three private copies meant three places that could
// skip silently.
struct test_device {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  test_device() {
    // The same flags src/gfx/device.cpp uses: VIDEO_SUPPORT is what makes
    // D3D11VA possible at all, and without it this test would silently measure
    // the software path.
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
};

// Plays until a frame comes out, or gives up. Returns null on timeout; the
// caller must release what it gets back.
mv::player::video_frame* first_frame(mv::player::media_source* source) {
  source->play();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    // One refresh interval at 60 Hz, the number the render thread would pass.
    if (auto* frame = source->acquire_frame(1, 16'666'667)) return frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return nullptr;
}

struct opened {
  mv::player::media_source* source = nullptr;
  ~opened() {
    if (source) mv::player::close_media(source);
  }
};

}  // namespace

TEST_CASE("HLG is carried through to the frame and marked for tone-mapping", "[video][colour]") {
  MV_REQUIRE_CLIP(path, "hlg_4k_10bit.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  auto* frame = first_frame(guard.source);
  REQUIRE(frame != nullptr);

  // The whole point of the clip: an untone-mapped HLG frame is the "washed out"
  // bug in the verify line, and it starts with the transfer surviving to here.
  CHECK(frame->colour.transfer == mv::gfx::colour_transfer::arib_std_b67);
  CHECK(frame->colour.matrix == mv::gfx::colour_matrix::bt2020_ncl);
  CHECK(frame->colour.primaries == mv::gfx::colour_primaries::bt2020);
  CHECK(mv::gfx::needs_tone_map(frame->colour));
  // 10-bit content must take the P010 path, not NV12.
  CHECK(frame->colour.bit_depth == 10);
  CHECK(frame->ten_bit);

  guard.source->release_frame(frame);
}

TEST_CASE("PQ is carried through and is a different frame description from HLG",
          "[video][colour]") {
  MV_REQUIRE_CLIP(path, "pq_4k_10bit.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  auto* frame = first_frame(guard.source);
  REQUIRE(frame != nullptr);
  CHECK(frame->colour.transfer == mv::gfx::colour_transfer::smpte2084);
  CHECK(mv::gfx::needs_tone_map(frame->colour));
  guard.source->release_frame(frame);
}

TEST_CASE("An SDR clip is NOT marked for tone-mapping", "[video][colour]") {
  MV_REQUIRE_CLIP(path, "hevc_4k_10bit_bt709.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  auto* frame = first_frame(guard.source);
  REQUIRE(frame != nullptr);
  // This clip is byte-identical to the HLG one except for its VUI, so a
  // difference here is unambiguously the tag path. D6: tone-mapping
  // display-referred content is a bug, not a safe default.
  CHECK(frame->colour.transfer == mv::gfx::colour_transfer::bt709);
  CHECK(frame->colour.matrix == mv::gfx::colour_matrix::bt709);
  CHECK_FALSE(mv::gfx::needs_tone_map(frame->colour));
  guard.source->release_frame(frame);
}

TEST_CASE("A full-range clip is not reported as limited", "[video][colour]") {
  MV_REQUIRE_CLIP(path, "hevc_4k_10bit_fullrange.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  auto* frame = first_frame(guard.source);
  REQUIRE(frame != nullptr);
  // plan/05: "Do not assume BT.709 limited range." An explicit full-range tag
  // has to beat the limited default, or PC-range video comes out crushed.
  CHECK(frame->colour.range == mv::gfx::colour_range::full);
  guard.source->release_frame(frame);
}

TEST_CASE("An untagged clip is resolved, and never guessed into HDR", "[video][colour]") {
  MV_REQUIRE_CLIP(path, "untagged_1080p_8bit.mp4");
  test_device dev;
  if (!dev.valid()) SKIP("no D3D11 hardware device on this machine");

  auto opened_source = mv::player::open_media(path.c_str(), dev.device);
  REQUIRE(opened_source);
  opened guard{opened_source.value()};

  auto* frame = first_frame(guard.source);
  REQUIRE(frame != nullptr);

  // Nothing may still be unspecified by the time it reaches the shader: the
  // shader is not allowed to guess.
  CHECK(frame->colour.matrix != mv::gfx::colour_matrix::unspecified);
  CHECK(frame->colour.primaries != mv::gfx::colour_primaries::unspecified);
  CHECK(frame->colour.transfer != mv::gfx::colour_transfer::unspecified);
  CHECK(frame->colour.range != mv::gfx::colour_range::unspecified);
  // 1080p resolves to BT.709 limited, and an untagged clip is never guessed
  // into HDR — that would tone-map ordinary video and darken it, which is worse
  // than the bug the resolution heuristic exists to prevent.
  CHECK(frame->colour.matrix == mv::gfx::colour_matrix::bt709);
  CHECK(frame->colour.range == mv::gfx::colour_range::limited);
  CHECK_FALSE(mv::gfx::needs_tone_map(frame->colour));
  CHECK(frame->colour.bit_depth == 8);
  CHECK_FALSE(frame->ten_bit);

  guard.source->release_frame(frame);
}
