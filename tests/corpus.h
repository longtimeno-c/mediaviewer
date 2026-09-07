// SPDX-License-Identifier: GPL-2.0-or-later
// Locating the PR 5 video corpus, and being honest when it is not there.
//
// The clips live in tools/testmedia/ and are gitignored (plan/09: the corpus
// does not go in git). Only tools/testmedia/generate.sh is tracked.
//
// WHY THIS HEADER EXISTS. The three video test files each had their own copy of
// the finder, and each skipped independently when a clip was missing. Catch2
// SKIP is not a failure and ctest reported the run as passed, so a clean clone
// — which has no corpus at all — went green with twelve cases silently gone,
// including the only assertions anywhere that D3D11VA is actually in use, that
// the colour description survives a real decode, that seek discards pre-seek
// frames, that frame step lands on exact frames, and that photo -> video ->
// photo leaks no textures. A build that proves none of that must not look like
// one that proves all of it.
//
// So: `clip()` still skips, because a developer without ffmpeg should not be
// blocked, but the skip is now visible three ways —
//   1. ctest marks these tests Skipped rather than Passed (SKIP_REGULAR_
//      EXPRESSION in CMakeLists.txt), so the summary line says how many;
//   2. tests/test_corpus.cpp reports which clips are missing, loudly;
//   3. with MV_REQUIRE_CORPUS=1 in the environment, a missing clip is a
//      FAILURE, not a skip. CI sets it. That is the switch that stops an
//      unverified build being mistaken for a verified one.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace corpus {

// Every clip the suite needs, in the order generate.sh produces them. Kept in
// sync with tools/testmedia/generate.sh --list; test_corpus.cpp checks the
// whole list so a clip nobody generates cannot go unnoticed the way
// av_transport.mp4 and hevc_4k_10bit_fullrange.mp4 did — both were required by
// tests and neither was ever in the generator.
inline constexpr const char* required[] = {
    "hevc_4k_10bit_bt709.mp4",
    "hevc_4k_8bit_bt709.mp4",
    "av1_4k_10bit_bt709.mp4",
    "hlg_4k_10bit.mp4",
    "pq_4k_10bit.mp4",
    "hevc_4k_10bit_fullrange.mp4",
    "untagged_1080p_8bit.mp4",
    "fullrange_1080p_8bit.mp4",
    "av_transport.mp4",
    "soak_10min_1080p_hevc.mp4",
};

// Walk up from the working directory: ctest runs from the build tree, and the
// corpus lives beside the sources.
[[nodiscard]] inline std::filesystem::path media_dir() {
  std::filesystem::path dir = std::filesystem::current_path();
  for (int up = 0; up < 6; ++up) {
    const std::filesystem::path candidate = dir / "tools" / "testmedia";
    if (std::filesystem::is_directory(candidate)) return candidate;
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}

// True when the environment demands a complete corpus. CI sets it; a developer
// box normally does not.
[[nodiscard]] inline bool corpus_required() {
  const char* value = std::getenv("MV_REQUIRE_CORPUS");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[nodiscard]] inline std::string path_of(const char* name) {
  const std::filesystem::path dir = media_dir();
  if (dir.empty()) return {};
  const std::filesystem::path path = dir / name;
  return std::filesystem::exists(path) ? path.string() : std::string{};
}

[[nodiscard]] inline std::vector<std::string> missing() {
  std::vector<std::string> out;
  for (const char* name : required) {
    if (path_of(name).empty()) out.emplace_back(name);
  }
  return out;
}

}  // namespace corpus

// The one call the video tests make. Declares `var` holding a usable path, or
// does not fall through at all: it SKIPs, or FAILs under MV_REQUIRE_CORPUS.
//
// A macro rather than a function because Catch2's SKIP and FAIL both expand to
// a `return` in the enclosing test case; behind a helper they would return from
// the helper and let the caller carry on with an empty path.
#define MV_REQUIRE_CLIP(var, name)                                                 \
  const std::string var = ::corpus::path_of(name);                                 \
  if (var.empty()) {                                                               \
    if (::corpus::corpus_required()) {                                             \
      FAIL("MV_REQUIRE_CORPUS is set and tools/testmedia/" name " is missing. "     \
           "Run tools/testmedia/generate.sh.");                                     \
    }                                                                              \
    SKIP("tools/testmedia/" name " not generated — run tools/testmedia/"            \
         "generate.sh. This test proved NOTHING.");                                 \
  }
