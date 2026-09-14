// SPDX-License-Identifier: GPL-2.0-or-later
// Animation timing, compositing and APNG parsing. Fixtures are built in the
// test (stored zlib blocks, no compressor), so no binary corpus lives in git.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include "codec/anim.h"
#include "codec/apng.h"
#include "codec/decode.h"

using namespace mv::codec;

TEST_CASE("frame delays follow the browser clamp", "[codec][anim]") {
  REQUIRE(browser_frame_delay_ms(0) == 100);
  REQUIRE(browser_frame_delay_ms(10) == 100);
  REQUIRE(browser_frame_delay_ms(11) == 11);
  REQUIRE(browser_frame_delay_ms(19) == 19);
  REQUIRE(browser_frame_delay_ms(20) == 20);
  REQUIRE(gif_delay_ms(0) == 100);
  REQUIRE(gif_delay_ms(1) == 100);  // 10 ms
  REQUIRE(gif_delay_ms(2) == 20);
  REQUIRE(gif_delay_ms(7) == 70);
  REQUIRE(apng_delay_ms(1, 0) == 100);     // 1/100 s is 10 ms: clamped
  REQUIRE(apng_delay_ms(1, 30) == 33);     // 33.3 ms rounds
  REQUIRE(apng_delay_ms(11, 1000) == 11);
  REQUIRE(apng_delay_ms(0, 1000) == 100);
}

TEST_CASE("the frame due at a time honours delays and loop counts", "[codec][anim]") {
  const std::array<std::uint32_t, 3> delays = {100, 100, 50};
  REQUIRE(frame_at(delays, 0, 0).index == 0);
  REQUIRE(frame_at(delays, 0, 99).index == 0);
  REQUIRE(frame_at(delays, 0, 100).index == 1);
  REQUIRE(frame_at(delays, 0, 100).next_change_ms == 200);
  REQUIRE(frame_at(delays, 0, 249).index == 2);
  // Forever: the second cycle starts again at frame 0.
  const auto second = frame_at(delays, 0, 250);
  REQUIRE(second.index == 0);
  REQUIRE_FALSE(second.finished);
  REQUIRE(second.next_change_ms == 350);
  // Two plays, then the last frame holds and the loop can stop presenting.
  REQUIRE(frame_at(delays, 2, 499).index == 2);
  const auto done = frame_at(delays, 2, 500);
  REQUIRE(done.finished);
  REQUIRE(done.index == 2);
  // A single frame, or no frames, is not animated.
  const std::array<std::uint32_t, 1> one = {100};
  REQUIRE(frame_at(one, 0, 5000).finished);
  REQUIRE(frame_at({}, 0, 0).finished);

  // Presented schedule: stepping at 60 Hz, every change lands within one
  // refresh after its due time.
  const double refresh_ms = 1000.0 / 60.0;
  std::uint32_t shown = 0;
  std::uint64_t due = frame_at(delays, 0, 0).next_change_ms;
  for (int vblank = 1; vblank < 120; ++vblank) {
    const auto t = static_cast<std::uint64_t>(vblank * refresh_ms);
    const auto p = frame_at(delays, 0, t);
    if (p.index != shown) {
      REQUIRE(t >= due);
      REQUIRE(static_cast<double>(t - due) < refresh_ms + 1.0);
      shown = p.index;
      due = p.next_change_ms;
    }
  }
}

TEST_CASE("the compositor applies disposal and blending, and never writes out of bounds",
          "[codec][anim]") {
  compositor c;
  REQUIRE_FALSE(c.reset(0, 4));
  REQUIRE(c.reset(2, 2));
  const auto px = [&c](std::uint32_t x, std::uint32_t y) {
    const auto p = c.pixels().subspan((static_cast<std::size_t>(y) * 2 + x) * 4, 4);
    return std::array<std::uint8_t, 4>{p[0], p[1], p[2], p[3]};
  };
  const std::vector<std::uint8_t> red(16, 0);
  std::vector<std::uint8_t> red_full = red;
  for (int i = 0; i < 4; ++i) {
    red_full[i * 4] = 255;
    red_full[i * 4 + 3] = 255;
  }
  // Frame 0: whole canvas red, dispose previous (treated as background on frame 0).
  REQUIRE(c.draw({0, 0, 2, 2, dispose_op::previous, blend_op::source}, red_full));
  REQUIRE(px(1, 1) == std::array<std::uint8_t, 4>{255, 0, 0, 255});

  // Frame 1: half-transparent blue over the bottom-right pixel. Frame 0's
  // disposal cleared the canvas first.
  const std::vector<std::uint8_t> blue = {0, 0, 255, 128};
  REQUIRE(c.draw({1, 1, 1, 1, dispose_op::previous, blend_op::over}, blue));
  REQUIRE(px(0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
  REQUIRE(px(1, 1) == std::array<std::uint8_t, 4>{0, 0, 255, 128});

  // Frame 2 restores what was under frame 1 (transparent), then draws green at 0,0.
  const std::vector<std::uint8_t> green = {0, 255, 0, 255};
  REQUIRE(c.draw({0, 0, 1, 1, dispose_op::none, blend_op::source}, green));
  REQUIRE(px(1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
  REQUIRE(px(0, 0) == std::array<std::uint8_t, 4>{0, 255, 0, 255});

  // Hostile regions and short pixel data are refused, and change nothing.
  const auto before = std::vector<std::uint8_t>(c.pixels().begin(), c.pixels().end());
  REQUIRE_FALSE(c.draw({1, 1, 2, 1, dispose_op::none, blend_op::source}, std::vector<std::uint8_t>(8)));
  REQUIRE_FALSE(c.draw({0xFFFFFFFF, 0, 2, 1, dispose_op::none, blend_op::source}, std::vector<std::uint8_t>(8)));
  REQUIRE_FALSE(c.draw({0, 0, 2, 2, dispose_op::none, blend_op::source}, std::vector<std::uint8_t>(8)));
  REQUIRE(std::vector<std::uint8_t>(c.pixels().begin(), c.pixels().end()) == before);
}

TEST_CASE("a ring-fed schedule presents on the file's cadence and wraps on time",
          "[codec][anim]") {
  // Frames 100 / 100 / 50 ms, looping, always ready; the render thread checks
  // once per 60 Hz vblank. Every change must land within one refresh of the
  // cumulative ideal, including frame 0 again at 250 ms and 500 ms.
  const std::array<std::uint32_t, 3> delays = {100, 100, 50};
  const double refresh_ms = 1000.0 / 60.0;
  frame_schedule schedule;
  std::uint32_t next_index = 0;
  std::uint64_t ideal = 0;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> changes;
  for (int vblank = 0; vblank < 60; ++vblank) {
    const auto now = static_cast<std::uint64_t>(vblank * refresh_ms);
    if (schedule.due(now)) {
      changes.emplace_back(next_index, now);
      REQUIRE(now >= ideal);
      REQUIRE(static_cast<double>(now - ideal) < refresh_ms + 1.0);
      schedule.shown(delays[next_index], now, 50);
      ideal += delays[next_index];
      next_index = (next_index + 1) % 3;
    }
  }
  REQUIRE(schedule.late() == 0);
  REQUIRE(changes.size() >= 10);
  REQUIRE(changes[3].first == 0);  // the loop wrap
  REQUIRE(changes[3].second >= 250);
  REQUIRE(static_cast<double>(changes[3].second - 250) < refresh_ms + 1.0);

  // The decoder falls behind by more than the slack: counted, cadence restarts.
  frame_schedule behind;
  behind.shown(100, 0, 50);
  REQUIRE_FALSE(behind.due(99));
  REQUIRE(behind.due(100));
  behind.shown(100, 400, 50);  // arrived 300 ms late
  REQUIRE(behind.late() == 1);
  REQUIRE(behind.next_due_ms() == 500);

  // Pause keeps what was left of the delay; a stepped frame gets its full delay.
  frame_schedule paused;
  paused.shown(100, 1000, 50);
  paused.pause(1040);
  REQUIRE_FALSE(paused.due(5000));
  paused.resume(5000);
  REQUIRE_FALSE(paused.due(5059));
  REQUIRE(paused.due(5060));
  paused.pause(5060);
  paused.stepped(80);
  paused.resume(6000);
  REQUIRE_FALSE(paused.due(6079));
  REQUIRE(paused.due(6080));
}

TEST_CASE("the animation clock plays, pauses and steps frames", "[codec][anim]") {
  const std::array<std::uint32_t, 3> delays = {100, 100, 50};
  animation_clock clock;
  clock.start(1000);
  REQUIRE(frame_at(delays, 0, clock.elapsed(1150)).index == 1);

  // Paused holds the frame however long the wall clock runs.
  clock.toggle(1150);
  REQUIRE(clock.paused());
  REQUIRE(frame_at(delays, 0, clock.elapsed(9000)).index == 1);
  // Resuming continues from where it stopped, not from the wall clock.
  clock.toggle(9000);
  REQUIRE_FALSE(clock.paused());
  REQUIRE(frame_at(delays, 0, clock.elapsed(9050)).index == 2);

  // `.` from frame 2 wraps to frame 0 and pauses; `,` goes back to frame 2.
  clock.step(delays, 0, +1, 9050);
  REQUIRE(clock.paused());
  REQUIRE(frame_at(delays, 0, clock.elapsed(20000)).index == 0);
  clock.step(delays, 0, -1, 20000);
  REQUIRE(frame_at(delays, 0, clock.elapsed(20000)).index == 2);
  clock.step(delays, 0, -1, 20000);
  REQUIRE(frame_at(delays, 0, clock.elapsed(20000)).index == 1);

  // A finished finite animation can still be stepped, and stepping does not
  // re-finish it.
  animation_clock once;
  once.start(0);
  REQUIRE(frame_at(delays, 1, once.elapsed(400)).finished);
  once.step(delays, 1, +1, 400);
  const auto stepped = frame_at(delays, 1, once.elapsed(400));
  REQUIRE(stepped.index == 0);
  REQUIRE_FALSE(stepped.finished);
  once.step({}, 1, +1, 400);  // no frames: nothing happens
  REQUIRE(frame_at(delays, 1, once.elapsed(400)).index == 0);
}

namespace {

// --- A tiny APNG writer for fixtures -------------------------------------

std::uint32_t crc(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::uint8_t b : bytes) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}

void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
  be32(out, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> typed(type, type + 4);
  typed.insert(typed.end(), data.begin(), data.end());
  out.insert(out.end(), typed.begin(), typed.end());
  be32(out, crc(typed));
}

// zlib stream of `raw` as one stored block.
std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& raw) {
  std::vector<std::uint8_t> z = {0x78, 0x01, 0x01};
  const auto n = static_cast<std::uint16_t>(raw.size());
  z.push_back(static_cast<std::uint8_t>(n));
  z.push_back(static_cast<std::uint8_t>(n >> 8));
  z.push_back(static_cast<std::uint8_t>(~n));
  z.push_back(static_cast<std::uint8_t>(~n >> 8));
  z.insert(z.end(), raw.begin(), raw.end());
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, (b << 16) | a);
  return z;
}

// RGBA8 scanlines with filter 0, for a w x h solid colour.
std::vector<std::uint8_t> solid(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> rgba) {
  std::vector<std::uint8_t> raw;
  for (std::uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (std::uint32_t x = 0; x < w; ++x) raw.insert(raw.end(), rgba.begin(), rgba.end());
  }
  return raw;
}

std::vector<std::uint8_t> fctl(std::uint32_t seq, std::uint32_t w, std::uint32_t h, std::uint32_t x,
                               std::uint32_t y, std::uint16_t num, std::uint16_t den,
                               std::uint8_t dispose, std::uint8_t blend) {
  std::vector<std::uint8_t> d;
  be32(d, seq);
  be32(d, w);
  be32(d, h);
  be32(d, x);
  be32(d, y);
  d.push_back(static_cast<std::uint8_t>(num >> 8));
  d.push_back(static_cast<std::uint8_t>(num));
  d.push_back(static_cast<std::uint8_t>(den >> 8));
  d.push_back(static_cast<std::uint8_t>(den));
  d.push_back(dispose);
  d.push_back(blend);
  return d;
}

struct fixture_options {
  std::uint32_t declared_frames = 2;
  std::uint32_t frame1_x = 1;
  std::uint32_t fdat_sequence = 2;
  bool actl = true;
};

std::vector<std::uint8_t> make_apng(const fixture_options& o = {}) {
  std::vector<std::uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  be32(ihdr, 2);
  be32(ihdr, 2);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  chunk(png, "IHDR", ihdr);
  if (o.actl) {
    std::vector<std::uint8_t> actl;
    be32(actl, o.declared_frames);
    be32(actl, 0);  // forever
    chunk(png, "acTL", actl);
    chunk(png, "fcTL", fctl(0, 2, 2, 0, 0, 1, 10, 0, 0));  // 100 ms, red default image
  }
  chunk(png, "IDAT", zlib_stored(solid(2, 2, {255, 0, 0, 255})));
  if (o.actl) {
    chunk(png, "fcTL", fctl(1, 1, 1, o.frame1_x, 1, 3, 100, 1, 1));  // 30 ms, blue 1x1
    std::vector<std::uint8_t> fdat;
    be32(fdat, o.fdat_sequence);
    const auto z = zlib_stored(solid(1, 1, {0, 0, 255, 255}));
    fdat.insert(fdat.end(), z.begin(), z.end());
    chunk(png, "fdAT", fdat);
  }
  chunk(png, "IEND", {});
  return png;
}

}  // namespace

TEST_CASE("an APNG parses into frames that decode on their own", "[codec][apng]") {
  const auto file = make_apng();
  auto parsed = parse_apng(file);
  REQUIRE(parsed);
  const apng_info& info = parsed.value();
  REQUIRE(info.width == 2);
  REQUIRE(info.height == 2);
  REQUIRE(info.plays == 0);
  REQUIRE(info.frames.size() == 2);
  REQUIRE(info.frames[0].delay_ms == 100);
  REQUIRE(info.frames[1].delay_ms == 30);
  REQUIRE(info.frames[1].region.x == 1);
  REQUIRE(info.frames[1].region.dispose == dispose_op::background);
  REQUIRE(info.frames[1].region.blend == blend_op::over);

  // Each frame is a PNG libspng can decode, at the frame's own size.
  auto f1 = apng_frame_png(info, info.frames[1]);
  REQUIRE(f1);
  auto decoded = decode_png(f1.value());
  REQUIRE(decoded);
  REQUIRE(decoded.value().width == 1);
  REQUIRE(decoded.value().rgba == std::vector<std::uint8_t>{0, 0, 255, 255});

  compositor c;
  REQUIRE(c.reset(info.width, info.height));
  auto f0 = decode_png(apng_frame_png(info, info.frames[0]).value());
  REQUIRE(f0);
  REQUIRE(c.draw(info.frames[0].region, f0.value().rgba));
  REQUIRE(c.draw(info.frames[1].region, decoded.value().rgba));
  const auto p = c.pixels();
  REQUIRE(p[0] == 255);                      // top-left still red
  REQUIRE((p[12] == 0 && p[14] == 255));     // bottom-right blue
}

TEST_CASE("an APNG source composites frame by frame and rewinds", "[codec][apng]") {
  auto bytes = std::make_shared<const std::vector<std::uint8_t>>(make_apng());
  auto opened = open_animation(bytes);
  REQUIRE(opened);
  animation_source& source = *opened.value();
  REQUIRE(source.info().frame_count == 2);
  REQUIRE(source.info().format == format_family::png);

  canvas_frame frame;
  REQUIRE(source.next(frame, nullptr).value());
  REQUIRE(frame.index == 0);
  const std::vector<std::uint8_t> frame0 = frame.rgba;
  REQUIRE(frame0[0] == 255);  // red
  REQUIRE(source.next(frame, nullptr).value());
  REQUIRE(frame.index == 1);
  REQUIRE((frame.rgba[12] == 0 && frame.rgba[14] == 255));  // blue over the corner
  REQUIRE_FALSE(source.next(frame, nullptr).value());

  REQUIRE(source.rewind());
  REQUIRE(source.next(frame, nullptr).value());
  REQUIRE(frame.rgba == frame0);

  auto still = std::make_shared<const std::vector<std::uint8_t>>(make_apng({.actl = false}));
  REQUIRE(open_animation(still).error() == mv::status::unsupported_format);
}

TEST_CASE("a broken APNG is corrupt, never out of bounds", "[codec][apng]") {
  REQUIRE(parse_apng(make_apng({.declared_frames = 3})).error() == mv::status::corrupt);
  REQUIRE(parse_apng(make_apng({.declared_frames = 0})).error() == mv::status::corrupt);
  REQUIRE(parse_apng(make_apng({.frame1_x = 2})).error() == mv::status::corrupt);   // rect past the canvas
  REQUIRE(parse_apng(make_apng({.fdat_sequence = 5})).error() == mv::status::corrupt);  // sequence gap
  // A still PNG is not an animation, and is left to the still decoder.
  REQUIRE(parse_apng(make_apng({.actl = false})).error() == mv::status::unsupported_format);
  const std::vector<std::uint8_t> not_png = {1, 2, 3};
  REQUIRE(parse_apng(not_png).error() == mv::status::unsupported_format);
  // Truncated mid-chunk.
  auto cut = make_apng();
  cut.resize(cut.size() - 20);
  REQUIRE_FALSE(parse_apng(cut));
}
