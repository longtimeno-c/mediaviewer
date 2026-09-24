// SPDX-License-Identifier: GPL-2.0-or-later
// The animation feed with a fake texture maker: bounds, order, loops, seek,
// retire. No GPU; the GIF fixture is encoded in the test.
#include <catch2/catch_test_macros.hpp>

#include <gif_lib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "abi/animation_session.h"
#include "codec/decode.h"

namespace {

struct fake_texture {
  std::uint32_t width{}, height{}, generation{}, mip_levels{};
};
using animation_frame = mv::abi::animation_frame<fake_texture>;
using animation_session = mv::abi::animation_session<fake_texture>;

struct gif_writer {
  std::vector<std::uint8_t> bytes;
};

int write_gif(GifFileType* gif, const GifByteType* data, int len) {
  auto* w = static_cast<gif_writer*>(gif->UserData);
  w->bytes.insert(w->bytes.end(), data, data + len);
  return len;
}

// `frames` frames of `size` x `size`, each a different palette index so frame
// order is visible in pixel 0; NETSCAPE loop count `loops`.
std::shared_ptr<const std::vector<std::uint8_t>> make_gif(int frames, int size, int loops) {
  gif_writer w;
  int error = 0;
  GifFileType* gif = EGifOpen(&w, write_gif, &error);
  REQUIRE(gif != nullptr);
  EGifSetGifVersion(gif, true);
  std::array<GifColorType, 256> colours{};
  for (int i = 0; i < 256; ++i) {
    colours[static_cast<std::size_t>(i)] = GifColorType{static_cast<GifByteType>(i), 0, 0};
  }
  ColorMapObject* map = GifMakeMapObject(256, colours.data());
  REQUIRE(EGifPutScreenDesc(gif, size, size, 8, 0, map) == GIF_OK);
  REQUIRE(EGifPutExtensionLeader(gif, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
  REQUIRE(EGifPutExtensionBlock(gif, 11, "NETSCAPE2.0") == GIF_OK);
  const std::array<std::uint8_t, 3> loop = {1, static_cast<std::uint8_t>(loops & 0xFF),
                                            static_cast<std::uint8_t>(loops >> 8)};
  REQUIRE(EGifPutExtensionBlock(gif, 3, loop.data()) == GIF_OK);
  REQUIRE(EGifPutExtensionTrailer(gif) == GIF_OK);
  std::vector<GifByteType> row(static_cast<std::size_t>(size));
  for (int f = 0; f < frames; ++f) {
    GraphicsControlBlock gcb{};
    gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
    gcb.DelayTime = 2;
    gcb.TransparentColor = NO_TRANSPARENT_COLOR;
    GifByteType ext[4];
    const auto n = EGifGCBToExtension(&gcb, ext);
    REQUIRE(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, static_cast<int>(n), ext) == GIF_OK);
    REQUIRE(EGifPutImageDesc(gif, 0, 0, size, size, false, nullptr) == GIF_OK);
    std::fill(row.begin(), row.end(), static_cast<GifByteType>(f % 256));
    for (int y = 0; y < size; ++y) REQUIRE(EGifPutLine(gif, row.data(), size) == GIF_OK);
  }
  REQUIRE(EGifCloseFile(gif, &error) == GIF_OK);
  GifFreeMapObject(map);
  return std::make_shared<const std::vector<std::uint8_t>>(std::move(w.bytes));
}

struct fake_textures {
  std::atomic<int> made{0};
  animation_session::make_texture_fn maker() {
    return [this](const mv::codec::canvas_frame& frame, const mv::codec::animation_info& info,
                  std::uint32_t generation) {
      auto t = std::make_unique<fake_texture>();
      t->width = info.width;
      t->height = info.height;
      t->generation = generation;
      t->mip_levels = frame.rgba.empty() ? 0 : frame.rgba[0];  // frame order, from pixel 0's red
      made.fetch_add(1);
      return t;
    };
  }
};

bool take_within(animation_session& s, std::uint32_t gen, animation_frame& out,
                 std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (s.take(gen, out)) return true;
    if (s.finished(gen)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

}  // namespace

TEST_CASE("ring depth scales down with the canvas", "[abi][animation]") {
  REQUIRE(mv::abi::animation_ring_depth(64, 64) == 6);
  REQUIRE(mv::abi::animation_ring_depth(3840, 2160) == 2);   // 8.3 MP
  REQUIRE(mv::abi::animation_ring_depth(4096, 4096) == 2);
  REQUIRE(mv::abi::animation_ring_depth(0, 0) == 2);
  REQUIRE(mv::abi::animation_ring_depth(2560, 1440) >= 2);
  REQUIRE(mv::abi::animation_ring_depth(2560, 1440) <= 6);
}

TEST_CASE("a 200-frame animation plays in order within the ring bound, twice, then ends",
          "[abi][animation]") {
  fake_textures textures;
  animation_session session(textures.maker());
  auto opened = mv::codec::open_animation(make_gif(200, 16, 2));
  REQUIRE(opened);
  const std::uint32_t gen = 7;
  session.retire(gen);
  session.publish(std::move(opened).value(), gen);
  REQUIRE(session.open(gen));
  REQUIRE_FALSE(session.open(8));

  int taken = 0;
  int max_outstanding = 0;
  for (int play = 0; play < 2; ++play) {
    for (int i = 0; i < 200; ++i) {
      // Let the decoder fill up before each take: it must stop at the depth.
      std::this_thread::sleep_for(std::chrono::microseconds(i < 5 ? 20000 : 0));
      animation_frame f;
      REQUIRE(take_within(session, gen, f));
      ++taken;
      max_outstanding = std::max(max_outstanding, textures.made.load() - taken);
      REQUIRE(f.index == static_cast<std::uint32_t>(i));
      REQUIRE(f.texture->mip_levels == static_cast<std::uint32_t>(i % 256));
      REQUIRE(f.delay_ms == 20);
      delete f.texture;
    }
  }
  // Nothing decoded beyond what the ring may hold (+1 for a frame mid-push).
  REQUIRE(max_outstanding <= static_cast<int>(session.depth()) + 1);
  REQUIRE(session.depth() == 6);

  // Two plays asked for, two played: the feed ends.
  animation_frame after;
  REQUIRE_FALSE(take_within(session, gen, after, std::chrono::milliseconds(2000)));
  REQUIRE(session.finished(gen));
}

TEST_CASE("seek restarts the feed at a frame; retire stops it", "[abi][animation]") {
  fake_textures textures;
  animation_session session(textures.maker());
  const std::uint32_t gen = 3;
  session.retire(gen);
  session.publish(mv::codec::open_animation(make_gif(20, 8, 0)).value(), gen);

  animation_frame f;
  REQUIRE(take_within(session, gen, f));
  REQUIRE(f.index == 0);
  delete f.texture;

  session.seek(12);
  REQUIRE(take_within(session, gen, f));
  REQUIRE(f.index == 12);
  delete f.texture;
  REQUIRE(take_within(session, gen, f));
  REQUIRE(f.index == 13);
  delete f.texture;

  // Navigation: the old generation's frames are gone and nothing more comes.
  session.retire(gen + 1);
  const int made_at_retire = textures.made.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE_FALSE(session.take(gen, f));
  REQUIRE_FALSE(session.take(gen + 1, f));
  REQUIRE(textures.made.load() <= made_at_retire + 1);
}

TEST_CASE("a one-frame GIF that asks to loop is not played as an animation",
          "[abi][animation]") {
  fake_textures textures;
  animation_session session(textures.maker());
  const std::uint32_t gen = 5;
  session.retire(gen);
  session.publish(mv::codec::open_animation(make_gif(1, 4, 0)).value(), gen);
  animation_frame f;
  if (take_within(session, gen, f, std::chrono::milliseconds(2000))) {
    REQUIRE(f.index == 0);
    delete f.texture;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!session.finished(gen) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(session.finished(gen));
  REQUIRE(textures.made.load() <= 1);
}
