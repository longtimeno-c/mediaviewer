// SPDX-License-Identifier: GPL-2.0-or-later
// Does this build actually prove anything about video?
//
// Every other video test skips itself when its clip is missing, which is the
// right behaviour for a developer without ffmpeg and the wrong behaviour for a
// build that then reports success. This test is the one that says so out loud:
// it names the missing clips, and under MV_REQUIRE_CORPUS it fails.
//
// It is deliberately the only place that decides what "complete" means, so the
// answer cannot drift between the three test files the way the clip list did.
#include <catch2/catch_test_macros.hpp>

#include "corpus.h"

TEST_CASE("the video corpus is present, or the build says it is not",
          "[corpus][video]") {
  const std::filesystem::path dir = corpus::media_dir();
  const std::vector<std::string> absent = corpus::missing();

  if (absent.empty()) {
    SUCCEED("video corpus complete: " << dir.string());
    return;
  }

  // WARN reaches the console even on a passing run, which is the point: a
  // partial corpus is the case most likely to be mistaken for a full one.
  std::string report = "INCOMPLETE VIDEO CORPUS — " + std::to_string(absent.size()) +
                       " of " + std::to_string(std::size(corpus::required)) +
                       " clips missing from " +
                       (dir.empty() ? std::string("tools/testmedia (directory not found)")
                                    : dir.string()) +
                       ":";
  for (const std::string& name : absent) report += "\n  - " + name;
  report +=
      "\nThe tests that need them SKIP, and a skip is not a pass: hardware"
      "\ndecode, colour, seek, frame step, texture leaks and the video ABI are"
      "\nall UNVERIFIED in this build. Run tools/testmedia/generate.sh.";

  if (corpus::corpus_required()) {
    FAIL(report << "\n\nMV_REQUIRE_CORPUS is set, so this is a failure.");
  }

  WARN(report);
  SKIP("video corpus incomplete — see the warning above. Set MV_REQUIRE_CORPUS=1 "
       "to make this a failure.");
}
