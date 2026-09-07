// SPDX-License-Identifier: GPL-2.0-or-later
// OWNER: mediaviewer-08 (5b). Already listed in CMakeLists.txt — fill it in.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("test_audio_sink placeholder", "[.pending]") {
  // [.pending] excludes it from a default run. A test that reports green
  // without asserting anything is worse than no test.
  SUCCEED();
}
