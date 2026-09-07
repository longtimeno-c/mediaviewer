// SPDX-License-Identifier: GPL-2.0-or-later
// CLAUDE.md: "Probe by magic bytes, never extension." These are the cases a
// real camera dump actually contains.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "player/container_probe.h"

using namespace mv::player;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values, std::size_t pad = 32) {
  std::vector<std::uint8_t> v;
  for (int b : values) v.push_back(static_cast<std::uint8_t>(b));
  v.resize(v.size() + pad, 0);
  return v;
}

std::vector<std::uint8_t> iso(const char* brand) {
  std::vector<std::uint8_t> v{0x00, 0x00, 0x00, 0x20};
  const char* ftyp = "ftyp";
  v.insert(v.end(), ftyp, ftyp + 4);
  v.insert(v.end(), brand, brand + 4);
  v.resize(64, 0);
  return v;
}

std::vector<std::uint8_t> ebml(const char* doctype) {
  std::vector<std::uint8_t> v{0x1A, 0x45, 0xDF, 0xA3};
  v.resize(24, 0);
  const std::size_t len = std::strlen(doctype);
  v.insert(v.end(), doctype, doctype + len);
  v.resize(128, 0);
  return v;
}

std::vector<std::uint8_t> ts(std::size_t packets, bool corrupt_cadence = false) {
  std::vector<std::uint8_t> v(188 * packets, 0x11);
  for (std::size_t i = 0; i < packets; ++i) v[i * 188] = 0x47;
  if (corrupt_cadence && packets > 1) v[188] = 0x00;
  return v;
}

}  // namespace

TEST_CASE("MP4 is recognised from its ftyp box", "[probe]") {
  REQUIRE(probe(iso("isom")) == container::mp4);
  REQUIRE(probe(iso("mp42")) == container::mp4);
  REQUIRE(probe(iso("avc1")) == container::mp4);
}

TEST_CASE("classic QuickTime is distinguished from MP4", "[probe]") {
  REQUIRE(probe(iso("qt  ")) == container::quicktime);
}

TEST_CASE("a phone .MOV that is really MP4 probes as MP4", "[probe]") {
  // The extension says QuickTime; the bytes say ISO-BMFF. The bytes win, which
  // is the entire reason this function exists.
  REQUIRE(probe(iso("mp42")) == container::mp4);
}

TEST_CASE("WebM and Matroska share EBML magic and are split by DocType", "[probe]") {
  REQUIRE(probe(ebml("webm")) == container::webm);
  REQUIRE(probe(ebml("matroska")) == container::matroska);
}

TEST_CASE("EBML with an unknown DocType falls back to Matroska", "[probe]") {
  // WebM is a subset, so treating one as the other costs nothing downstream.
  REQUIRE(probe(ebml("someothertype")) == container::matroska);
}

TEST_CASE("AVI is RIFF with the AVI form type, not any RIFF", "[probe]") {
  auto avi = bytes({'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00, 'A', 'V', 'I', ' '});
  REQUIRE(probe(avi) == container::avi);
}

TEST_CASE("a WAV file is RIFF but is not video", "[probe]") {
  // RIFF alone would misclassify every WAV in the folder as a clip.
  auto wav = bytes({'R', 'I', 'F', 'F', 0x10, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E'});
  REQUIRE(probe(wav) == container::unknown);
  REQUIRE_FALSE(is_video(probe(wav)));
}

TEST_CASE("MPEG-TS needs the 188-byte cadence, not one lucky byte", "[probe]") {
  // 0x47 is a common byte value. Accepting it alone misclassifies roughly one
  // file in 256 as a transport stream.
  REQUIRE(probe(ts(4)) == container::mpeg_ts);
  REQUIRE(probe(ts(4, /*corrupt_cadence=*/true)) == container::unknown);
}

TEST_CASE("a single 0x47 byte is not a transport stream", "[probe]") {
  auto lucky = bytes({0x47, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB});
  REQUIRE(probe(lucky) == container::unknown);
}

TEST_CASE("a JPEG renamed to .mp4 is not video", "[probe]") {
  // The case that actually bites in a camera dump: routing this to the video
  // decoder reports a corrupt clip instead of showing the photo.
  auto jpeg = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01});
  REQUIRE_FALSE(is_video(probe(jpeg)));
}

TEST_CASE("a PNG is not video", "[probe]") {
  auto png = bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D});
  REQUIRE_FALSE(is_video(probe(png)));
}

TEST_CASE("a truncated or empty file is not video and does not read past the end",
          "[probe]") {
  REQUIRE(probe({}) == container::unknown);
  auto tiny = bytes({0x00, 0x00, 0x00}, 0);
  REQUIRE(probe(tiny) == container::unknown);
  auto almost = bytes({0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p'}, 0);
  REQUIRE(probe(almost) == container::unknown);
}

TEST_CASE("every recognised container reports as video and has a name", "[probe]") {
  for (auto c : {container::mp4, container::quicktime, container::matroska,
                 container::webm, container::avi, container::mpeg_ts}) {
    REQUIRE(is_video(c));
    REQUIRE(container_name(c) != nullptr);
    REQUIRE(std::strlen(container_name(c)) > 0);
  }
  REQUIRE_FALSE(is_video(container::unknown));
}
