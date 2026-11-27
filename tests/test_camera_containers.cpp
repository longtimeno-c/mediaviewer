// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Camcorder and older-camera containers: AVCHD .MTS/.M2TS (MPEG-TS with a
// 4-byte timecode per packet) and .MPG/.MPEG (MPEG program stream). Real bytes
// from FFmpeg's own muxers, so the probe is checked against what a camera
// writes rather than a hand-built header, and the clip decodes to a poster.
#include "catch_compat.h"

#include <cstdint>
#include <string>
#include <vector>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "io/pairing.h"
#include "player/container_probe.h"
#include "player/poster.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;
using mv::player::container;

TEST_CASE("an AVCHD .MTS and an .MPG are listed, probe as video and give a poster",
          "[probe][camera-containers]") {
  scratch_dir d("camera_containers");
  const struct {
    const char* name;
    const char* muxer;
    container expect;
  } cases[] = {
      // The mpegts muxer writes 192-byte M2TS packets when the name ends .m2ts.
      {"00000.m2ts", "mpegts", container::mpeg_ts},
      {"MOV001.mpg", "mpeg", container::mpeg_ps},
  };
  for (const auto& c : cases) {
    INFO(c.name);
    fx::spec s;
    s.muxer = c.muxer;
    s.audio = false;
    s.frames = 10;
    REQUIRE(fx::make(utf8(d / c.name), s));

    CHECK(mv::io::is_video_name(c.name));

    std::vector<std::uint8_t> head = read_bytes(d / c.name);
    if (head.size() > mv::player::probe_bytes) head.resize(mv::player::probe_bytes);
    CHECK(mv::player::probe(head) == c.expect);

    auto poster = mv::player::poster_frame(utf8(d / c.name).c_str(), 64, nullptr);
    REQUIRE(poster);
    CHECK(poster->width > 0);
    CHECK(poster->height > 0);
  }

  // An .MTS from the camera is the same bytes as .m2ts; only the name differs.
  CHECK(mv::io::is_video_name("00001.MTS"));
}
