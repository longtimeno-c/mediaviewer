// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's host side for a clip's picture (#297 slice 5): every frame out
// (clip::frame_reader) and a new clip in (clip::picture_writer), on
// synthetic clips (clip_fixture.h). The writer runs FFmpeg's MPEG-4 encoder
// through its tests-only hook, so the same cases run where there is no
// hardware encoder (Linux CI); the product opens hardware encoders only.
#include "catch_compat.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "edit/clip_sample.h"
#include "edit/picture_io.h"
#include "import_fixture.h"

using namespace mv::edit::clip;
using mv::test::scratch_dir;
using mv::test::utf8;

namespace {

namespace fs = std::filesystem;

struct frame {
  std::vector<std::uint8_t> rgb;
  std::int64_t pts_us = 0;
};

std::vector<frame> read_frames(const std::string& path, frames_facts* facts = nullptr) {
  auto r = frame_reader::open(path);
  REQUIRE(r);
  if (facts) *facts = (*r)->facts();
  std::vector<frame> out;
  while (true) {
    frame f;
    auto more = (*r)->next(f.rgb, f.pts_us);
    REQUIRE(more);
    if (!*more) break;
    out.push_back(std::move(f));
  }
  return out;
}

// Nearest-neighbour 2x: what an upscaler looks like to the writer.
std::vector<std::uint8_t> double_size(const std::vector<std::uint8_t>& rgb, int w, int h) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 12);
  for (int y = 0; y < 2 * h; ++y) {
    for (int x = 0; x < 2 * w; ++x) {
      for (int c = 0; c < 3; ++c) {
        out[(static_cast<std::size_t>(y) * 2 * w + x) * 3 + c] = rgb[(static_cast<std::size_t>(y / 2) * w + x / 2) * 3 + c];
      }
    }
  }
  return out;
}

picture_out_options software(std::uint32_t w, std::uint32_t h) {
  picture_out_options o;
  o.width = w;
  o.height = h;
  o.encoders = {"mpeg4"};
  o.allow_software = true;
  return o;
}

}  // namespace

TEST_CASE("picture: every frame comes out, in order, at the coded size", "[clip][picture]") {
  scratch_dir s("picture_read");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 45;
  spec.b_frames = 2;  // decode order is not display order
  REQUIRE(mv::test::clipfx::make(clip, spec));
  frames_facts f;
  const auto frames = read_frames(clip, &f);
  CHECK(f.width == 320u);
  CHECK(f.height == 240u);
  CHECK_FALSE(f.hdr);
  CHECK(std::abs(f.fps - 30.0) < 0.01);
  CHECK(std::llabs(f.frame_count - 45) <= 1);
  REQUIRE(frames.size() == 45u);
  for (std::size_t i = 1; i < frames.size(); ++i) CHECK(frames[i].pts_us > frames[i - 1].pts_us);
  // Frame i's bottom half is luma 16 + 4 (i % 50): in display order, and grey.
  for (const std::size_t i : {std::size_t{0}, std::size_t{10}, std::size_t{44}}) {
    const auto& px = frames[i].rgb;
    const std::size_t at = (static_cast<std::size_t>(200) * 320 + 160) * 3;
    const double want = (mv::test::clipfx::luma_of_frame(static_cast<int>(i)) - 16) * 255.0 / 219.0;
    CHECK(std::abs(px[at] - want) < 6);
    CHECK(std::abs(px[at] - px[at + 1]) < 4);
  }
  // A file with no picture is refused.
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 0.3));
  auto none = frame_reader::open(song);
  REQUIRE_FALSE(none);
  CHECK(none.error() == mv::status::unsupported_format);
}

TEST_CASE("picture: a new clip at twice the size keeps the frames, their times and the sound", "[clip][picture]") {
  scratch_dir s("picture_write");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 30;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  const auto before = mv::test::read_bytes(clip);
  const auto frames = read_frames(clip);

  auto w = picture_writer::open(clip, software(640, 480));
  REQUIRE(w);
  CHECK(std::string((*w)->encoder()) == "mpeg4");
  for (const frame& f : frames) {
    const auto big = double_size(f.rgb, 320, 240);
    REQUIRE((*w)->write_frame(big.data(), 640 * 3, f.pts_us));
  }
  CHECK_FALSE(fs::exists(s / "clip-enhance.mp4"));  // hidden until finished
  auto out = (*w)->finish();
  REQUIRE(out);
  CHECK(fs::path(*out).filename() == "clip-enhance.mp4");
  CHECK(mv::test::read_bytes(clip) == before);

  frames_facts f2;
  const auto again = read_frames(*out, &f2);
  CHECK(f2.width == 640u);
  CHECK(f2.height == 480u);
  REQUIRE(again.size() == frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) CHECK(std::llabs(again[i].pts_us - frames[i].pts_us) < 2000);
  mv::test::clipfx::decoded d;
  REQUIRE(mv::test::clipfx::decode_all(*out, d));
  CHECK(d.audio_packets > 0);  // the source's sound, copied
  for (std::size_t i = 0; i < d.luma.size(); ++i) {
    CHECK(std::abs(d.luma[i] - mv::test::clipfx::luma_of_frame(static_cast<int>(i))) <= 3);
  }
}

TEST_CASE("picture: new sound goes in with the new frames", "[clip][picture]") {
  scratch_dir s("picture_sound");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 30;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  const auto frames = read_frames(clip);
  auto o = software(320, 240);
  o.sound_rate = 48000;
  o.sound_channels = 2;
  auto w = picture_writer::open(clip, o);
  REQUIRE(w);
  // A second of sound, pushed beside the frames as the runner does.
  const std::size_t per_frame = 1600;
  std::vector<float> tone(per_frame * 2);
  for (std::size_t i = 0; i < per_frame; ++i) {
    tone[2 * i] = tone[2 * i + 1] = 0.2f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
  }
  for (const frame& f : frames) {
    REQUIRE((*w)->write_sound(tone));
    REQUIRE((*w)->write_frame(f.rgb.data(), 320 * 3, f.pts_us));
  }
  auto out = (*w)->finish();
  REQUIRE(out);
  // The OS's AAC keeps the clip's MP4; FLAC (no OS encoder) goes to Matroska.
  const std::string name = fs::path(*out).filename().string();
  CHECK((name == "clip-enhance.mp4" || name == "clip-enhance.mkv"));
  auto sound = audio_reader::open(*out, 48000, 0, nullptr, 0);
  REQUIRE(sound);
  CHECK((*sound)->facts().channels == 2u);
  CHECK(read_frames(*out).size() == frames.size());
}

TEST_CASE("picture: a writer that never finishes leaves nothing; bad sizes are refused", "[clip][picture]") {
  scratch_dir s("picture_abort");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 10;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  {
    auto w = picture_writer::open(clip, software(320, 240));
    REQUIRE(w);
    std::vector<std::uint8_t> grey(320 * 240 * 3, 128);
    REQUIRE((*w)->write_frame(grey.data(), 320 * 3, 0));
  }
  for (const auto& e : fs::directory_iterator(s.root())) CHECK(e.path().filename() == "clip.mp4");
  CHECK_FALSE(picture_writer::open(clip, software(321, 240)));  // odd
  CHECK_FALSE(picture_writer::open(clip, software(9000, 240)));
  // The product list has no software encoder: where there is no hardware
  // one, it says so rather than encoding with something else.
  picture_out_options hw;
  hw.width = 320;
  hw.height = 240;
  hw.encoders = {"mpeg4"};  // a software encoder without the tests' permission
  auto refused = picture_writer::open(clip, hw);
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::unsupported_format);
}
