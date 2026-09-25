// SPDX-License-Identifier: GPL-2.0-or-later
// PR 15: what the Spotlight importer reports for a clip (shell/spotlight_fields).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "shell/spotlight_fields.h"

using mv::shell::spotlight_field;

namespace {

const spotlight_field* find(const std::vector<spotlight_field>& v, const char* key) {
  for (const auto& f : v) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

mv::meta::metadata clip() {
  mv::meta::metadata m;
  m.is_clip = true;
  m.s.width = 1920;
  m.s.height = 1080;
  m.s.duration_seconds = 12.5;
  m.s.bitrate_bps = 8'000'000;
  m.s.audio_channels = 2;
  m.s.audio_sample_rate = 48000;
  m.s.codecs = {"H.264 / AVC", "AAC (Advanced Audio Coding)"};
  m.s.date_taken_key = 1714572202;  // 2024-05-01 14:03:22 UTC
  m.s.camera = "GoPro HERO12";
  mv::meta::stream_info v;
  v.kind = mv::meta::stream_kind::video;
  mv::meta::stream_info a;
  a.kind = mv::meta::stream_kind::audio;
  mv::meta::stream_info sub;
  sub.kind = mv::meta::stream_kind::subtitle;
  m.streams = {v, a, sub};
  m.properties.push_back({mv::meta::origin::container, "Container", "title", "title", "Ridge run", "Ridge run",
                          "Container.title"});
  return m;
}

}  // namespace

TEST_CASE("a clip reports its size, length, streams and date", "[shell][spotlight]") {
  const auto f = mv::shell::spotlight_fields(clip());
  REQUIRE(find(f, "kMDItemPixelWidth"));
  CHECK(find(f, "kMDItemPixelWidth")->number == 1920);
  CHECK(find(f, "kMDItemPixelHeight")->number == 1080);
  CHECK(find(f, "kMDItemDurationSeconds")->number == 12.5);
  CHECK(find(f, "kMDItemTotalBitRate")->number == 8'000'000);
  CHECK(find(f, "kMDItemAudioChannelCount")->number == 2);
  CHECK(find(f, "kMDItemAudioSampleRate")->number == 48000);
  REQUIRE(find(f, "kMDItemCodecs"));
  CHECK(find(f, "kMDItemCodecs")->texts.size() == 2);
  REQUIRE(find(f, "kMDItemMediaTypes"));
  CHECK(find(f, "kMDItemMediaTypes")->texts == std::vector<std::string>{"Video", "Sound"});
  REQUIRE(find(f, "kMDItemContentCreationDate"));
  CHECK(find(f, "kMDItemContentCreationDate")->type == spotlight_field::kind::date);
  CHECK(find(f, "kMDItemContentCreationDate")->unix_seconds == 1714572202);
  CHECK(find(f, "kMDItemTitle")->text == "Ridge run");
  CHECK(find(f, "kMDItemAcquisitionModel")->text == "GoPro HERO12");
}

TEST_CASE("unknown facts are left out, not reported as zero", "[shell][spotlight]") {
  mv::meta::metadata m;
  m.is_clip = true;
  const auto f = mv::shell::spotlight_fields(m);
  CHECK(f.empty());
  mv::meta::metadata still;
  still.s.width = 100;
  still.s.height = 50;
  still.s.duration_seconds = 3;  // a still never reports clip fields
  const auto g = mv::shell::spotlight_fields(still);
  CHECK(g.size() == 2);
  CHECK_FALSE(find(g, "kMDItemDurationSeconds"));
}
