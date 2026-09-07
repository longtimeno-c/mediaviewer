// SPDX-License-Identifier: GPL-2.0-or-later
// OWNER: mediaviewer-48 (5a). Already listed in CMakeLists.txt — fill it in.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("test_video_colour placeholder", "[.pending]") {
  // Tagged [.pending] so it is EXCLUDED from a default run rather than passing
  // vacuously. An empty test that reports green is worse than no test.
  SUCCEED();
}
