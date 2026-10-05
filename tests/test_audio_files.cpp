// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Audio files (docs/plans/audio-and-documents.md §2.2): recognised by magic
// bytes, routed the clip's way by extension, and given a picture — the cover
// art or the card — by the poster path. No device, so every platform runs it.
#include "catch_compat.h"

#include <cstdint>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "io/pairing.h"
#include "codec/card.h"
#include "player/container_probe.h"
#include "player/poster.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;
using mv::player::container;

namespace {

std::vector<std::uint8_t> padded(std::vector<std::uint8_t> head) {
  head.resize(1024, 0);
  return head;
}

// A 128 kb/s, 44.1 kHz MPEG-1 Layer III header and its frame size (417 bytes).
std::vector<std::uint8_t> mp3_frames(int count) {
  std::vector<std::uint8_t> out;
  for (int i = 0; i < count; ++i) {
    std::vector<std::uint8_t> frame(417, 0x55);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    frame[3] = 0x64;
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

std::vector<std::uint8_t> iso(const char* major) {
  std::vector<std::uint8_t> b = {0, 0, 0, 0x20, 'f', 't', 'y', 'p'};
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(major[i]));
  b.insert(b.end(), {0, 0, 0, 0, 'i', 's', 'o', 'm', 'm', 'p', '4', '2'});
  return padded(b);
}

std::size_t glyph_pixels(const mv::codec::card& card) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < card.rgba.size(); i += 4) n += card.rgba[i] > 120 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("MP3 is recognised by its ID3 tag or a confirmed frame header", "[audio][probe]") {
  using mv::player::probe;
  CHECK(probe(padded({'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0})) == container::mp3);
  CHECK(probe(padded({'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0})) == container::mp3);
  CHECK(probe(padded(mp3_frames(2))) == container::mp3);
  // One plausible header followed by garbage is not enough.
  auto lone = mp3_frames(1);
  lone.resize(1024, 0x00);
  CHECK(probe(lone) == container::unknown);
  // JPEG and a bad bitrate index are not MP3.
  CHECK(probe(padded({0xFF, 0xD8, 0xFF, 0xE0})) == container::unknown);
  CHECK(probe(padded({0xFF, 0xFB, 0xF0, 0x64})) == container::unknown);
  CHECK(mv::player::is_audio(container::mp3));
  CHECK(mv::player::is_video(container::mp3));  // routed the clip's way
}

TEST_CASE("iTunes audio brands probe as m4a, not as a video", "[audio][probe]") {
  using mv::player::probe;
  CHECK(probe(iso("M4A ")) == container::m4a);
  CHECK(probe(iso("M4P ")) == container::m4a);
  CHECK(probe(iso("M4B ")) == container::m4a);
  CHECK(probe(iso("isom")) == container::mp4);
  CHECK(mv::player::is_audio(container::m4a));
  CHECK_FALSE(mv::player::is_audio(container::mp4));
}

TEST_CASE("Audio extensions route through the player", "[audio][pairing]") {
  using mv::io::is_audio_name;
  using mv::io::is_video_name;
  for (const char* name : {"song.mp3", "SONG.MP3", "a.m4a", "old.M4P"}) {
    CAPTURE(name);
    CHECK(is_audio_name(name));
    CHECK(is_video_name(name));
  }
  CHECK_FALSE(is_audio_name("clip.mp4"));
  CHECK_FALSE(is_audio_name("song.mp3.jpg"));
}

TEST_CASE("Settings can leave audio and documents out of a listing", "[audio][pairing][settings]") {
  using mv::io::is_hidden_kind;
  using mv::io::kHideAudio;
  using mv::io::kHideDocuments;
  CHECK(mv::io::is_document_name("Report.PDF"));
  CHECK(mv::io::is_document_name("letter.docx"));
  CHECK_FALSE(mv::io::is_document_name("photo.jpg"));
  CHECK_FALSE(is_hidden_kind("song.mp3", 0));
  CHECK(is_hidden_kind("song.mp3", kHideAudio));
  CHECK_FALSE(is_hidden_kind("song.mp3", kHideDocuments));
  CHECK(is_hidden_kind("a.pdf", kHideDocuments));
  CHECK_FALSE(is_hidden_kind("a.pdf", kHideAudio));
  for (const char* name : {"photo.jpg", "clip.mp4", "raw.nef"}) {
    CAPTURE(name);
    CHECK_FALSE(is_hidden_kind(name, kHideAudio | kHideDocuments));
  }
}

TEST_CASE("The audio card is opaque, even-sized and draws its glyph", "[audio][card]") {
  using mv::codec::card_kind;
  const auto music = mv::codec::make_card(card_kind::music, 641, 361);
  CHECK(music.width == 640);
  CHECK(music.height == 360);
  REQUIRE(music.rgba.size() == 640u * 360u * 4u);
  for (std::size_t i = 3; i < music.rgba.size(); i += 4) REQUIRE(music.rgba[i] == 255);
  // The corner is background; the glyph covers some, not most, of the card.
  CHECK(music.rgba[0] < 60);
  const auto lit = glyph_pixels(music);
  CHECK(lit > 640u * 360u / 200u);
  CHECK(lit < 640u * 360u / 4u);
  const auto lock = mv::codec::make_card(card_kind::locked, 640, 360);
  CHECK(glyph_pixels(lock) > 640u * 360u / 200u);
  CHECK(lock.rgba != music.rgba);
}

TEST_CASE("An audio file's poster is the card when it has no art", "[audio][poster]") {
  scratch_dir dir("audio_poster");
  const auto plain = utf8(dir / "tone.m4a");
  const auto locked = utf8(dir / "tone.m4p");
  REQUIRE(fx::make_audio(plain, 1.0));
  REQUIRE(fx::make_audio(locked, 1.0, true));

  auto a = mv::player::poster_frame(plain.c_str(), 512);
  REQUIRE(a);
  CHECK(a.value().width == 512);
  CHECK(a.value().height == 288);
  auto b = mv::player::poster_frame(locked.c_str(), 512);
  REQUIRE(b);
  CHECK(b.value().rgba != a.value().rgba);  // the padlock, not the note
}
