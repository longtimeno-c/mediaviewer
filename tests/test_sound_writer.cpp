// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's host side (#297 slice 4): a file's sound at full quality
// (clip::audio_reader with the source's channels) and a new file beside it
// with new sound (clip::sound_writer), on synthetic clips (clip_fixture.h).
//
// The source is never written; the output appears only when finished, under
// a free name; an audio file gets an M4A, a clip keeps its picture packets
// byte for byte with the new sound beside them.
#include "catch_compat.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "edit/clip_sample.h"
#include "edit/sound_writer.h"
#include "import_fixture.h"

using namespace mv::edit::clip;
using mv::test::scratch_dir;
using mv::test::utf8;

namespace {

namespace fs = std::filesystem;

std::vector<float> read_all(const std::string& path, std::uint32_t rate, std::uint32_t channels,
                            audio_facts* facts = nullptr) {
  auto r = audio_reader::open(path, rate, 0, nullptr, channels);
  REQUIRE(r);
  if (facts) *facts = (*r)->facts();
  std::vector<float> all;
  std::int64_t t = 0;
  while (true) {
    auto chunk = (*r)->read(48000, t);
    REQUIRE(chunk);
    if (chunk->empty()) break;
    all.insert(all.end(), chunk->begin(), chunk->end());
  }
  return all;
}

// A stereo test tone: left 300 Hz, right 900 Hz, quarter scale.
std::vector<float> tone(std::size_t frames) {
  std::vector<float> v(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    v[2 * i] = 0.25f * static_cast<float>(std::sin(2 * 3.14159265 * 300 * static_cast<double>(i) / 48000));
    v[2 * i + 1] = 0.25f * static_cast<float>(std::sin(2 * 3.14159265 * 900 * static_cast<double>(i) / 48000));
  }
  return v;
}

bool no_part_files(const fs::path& dir) {
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().filename().string().find(".mvpart") != std::string::npos) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("sound: the reader keeps the source's channels when asked", "[clip][sound]") {
  scratch_dir s("sound_read");
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 1.0));
  audio_facts f;
  const auto stereo = read_all(song, 48000, 0, &f);
  CHECK(f.channels == 2);
  CHECK(f.source_channels == 2);
  CHECK(f.source_rate == 48000);
  CHECK_FALSE(f.has_picture);
  CHECK(stereo.size() % 2 == 0);
  CHECK(std::abs(static_cast<long>(stereo.size() / 2) - 48000) < 2048);  // the encoder's priming
  // The default stays mono (the AI pack's audio index reads it so).
  const auto mono = read_all(song, 16000, 1, &f);
  CHECK(f.channels == 1);
  CHECK(std::abs(static_cast<long>(mono.size()) - 16000) < 1024);
}

TEST_CASE("sound: an audio file gets a new M4A beside it, the original untouched", "[clip][sound]") {
  scratch_dir s("sound_m4a");
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 1.0));
  const auto before = mv::test::read_bytes(song);

  sound_out_options o;
  o.sample_rate = 48000;
  o.channels = 2;
  auto w = sound_writer::open(song, o);
  REQUIRE(w);
  CHECK_FALSE((*w)->with_picture());
  const auto pcm = tone(48000);
  for (std::size_t at = 0; at < pcm.size(); at += 9600) {
    const std::size_t n = std::min<std::size_t>(9600, pcm.size() - at);
    REQUIRE((*w)->write(std::span<const float>(pcm.data() + at, n)));
  }
  // Nothing is there until it is finished.
  CHECK_FALSE(fs::exists(s / "song-enhance.m4a"));
  auto out = (*w)->finish();
  REQUIRE(out);
  CHECK(fs::path(*out).filename() == "song-enhance.m4a");
  CHECK(mv::test::read_bytes(song) == before);
  CHECK(no_part_files(s.root()));

  // It reads back as the tone: the right length and each channel its own.
  audio_facts f;
  const auto back = read_all(*out, 48000, 0, &f);
  CHECK(f.channels == 2);
  CHECK(std::abs(static_cast<long>(back.size() / 2) - 48000) < 2048);
  double l = 0, r = 0;
  for (std::size_t i = 0; i < back.size() / 2; ++i) {
    l += back[2 * i] * back[2 * i];
    r += back[2 * i + 1] * back[2 * i + 1];
  }
  CHECK(l > 100.0);
  CHECK(r > 100.0);

  // A second run takes the next free name.
  auto again = sound_writer::open(song, o);
  REQUIRE(again);
  REQUIRE((*again)->write(std::span<const float>(pcm.data(), 9600)));
  auto second = (*again)->finish();
  REQUIRE(second);
  CHECK(fs::path(*second).filename() != "song-enhance.m4a");
  CHECK(fs::exists(s / "song-enhance.m4a"));
}

TEST_CASE("sound: a clip keeps its picture packets with the new sound beside them", "[clip][sound]") {
  scratch_dir s("sound_clip");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 60;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  const auto before = mv::test::read_bytes(clip);
  audio_facts f;
  const auto src = read_all(clip, 48000, 0, &f);
  CHECK(f.has_picture);

  sound_out_options o;
  o.channels = f.channels;
  auto w = sound_writer::open(clip, o);
  REQUIRE(w);
  CHECK((*w)->with_picture());
  REQUIRE((*w)->write(src));
  auto out = (*w)->finish();
  REQUIRE(out);
  CHECK(fs::path(*out).filename() == "clip-enhance.mp4");
  CHECK(mv::test::read_bytes(clip) == before);
  CHECK(mv::test::clipfx::video_packets(*out) == mv::test::clipfx::video_packets(clip));
  mv::test::clipfx::decoded d;
  REQUIRE(mv::test::clipfx::decode_all(*out, d));
  CHECK(d.luma.size() == 60u);
  CHECK(d.audio_packets > 0);
}

TEST_CASE("sound: a writer that never finishes leaves nothing", "[clip][sound]") {
  scratch_dir s("sound_abort");
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 0.5));
  {
    sound_out_options o;
    auto w = sound_writer::open(song, o);
    REQUIRE(w);
    const auto pcm = tone(4800);
    REQUIRE((*w)->write(pcm));
  }
  CHECK_FALSE(fs::exists(s / "song-enhance.m4a"));
  CHECK(no_part_files(s.root()));
  // Bad options are refused before anything is made.
  sound_out_options bad;
  bad.suffix = "../x";
  CHECK_FALSE(sound_writer::open(song, bad));
  bad.suffix = "enhance";
  bad.channels = 0;
  CHECK_FALSE(sound_writer::open(song, bad));
}
