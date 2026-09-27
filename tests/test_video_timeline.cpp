// SPDX-License-Identifier: GPL-3.0-or-later
// shell/video_timeline.h: the Video Editor's cut list (PR 30, plan/21).
#include "catch_compat.h"

#include "shell/video_timeline.h"

using mv::shell::video_timeline;

namespace {
constexpr std::int64_t kS = 1'000'000'000;
}

TEST_CASE("a loaded clip is one piece; split makes two back to back", "[timeline][pr30]") {
  video_timeline t;
  CHECK_FALSE(t.loaded());
  t.load(10 * kS);
  REQUIRE(t.pieces().size() == 1);
  CHECK(t.length() == 10 * kS);
  CHECK_FALSE(t.edited());
  CHECK(t.split(4 * kS));
  REQUIRE(t.pieces().size() == 2);
  CHECK(t.pieces()[0].out_ns == 4 * kS);
  CHECK(t.pieces()[1].in_ns == 4 * kS);
  CHECK(t.length() == 10 * kS);           // a split cuts nothing
  CHECK_FALSE(t.split(4 * kS));          // on an edge: nothing to split
  CHECK_FALSE(t.split(4 * kS + 1'000));  // a sliver is not a piece
}

TEST_CASE("delete, in and out cut the program; the clocks map through the cuts", "[timeline][pr30]") {
  video_timeline t;
  t.load(10 * kS);
  REQUIRE(t.split(2 * kS));
  REQUIRE(t.split(5 * kS));
  REQUIRE(t.remove(1));  // cut [2, 5)
  CHECK(t.edited());
  CHECK(t.length() == 7 * kS);
  CHECK(t.to_source(1 * kS) == 1 * kS);
  CHECK(t.to_source(2 * kS) == 5 * kS);  // the join
  CHECK(t.to_source(3 * kS) == 6 * kS);
  CHECK(t.to_timeline(3 * kS) == std::nullopt);  // cut out
  CHECK(t.to_timeline(6 * kS) == 3 * kS);
  // Playback jumps the cut and stops at the end.
  CHECK(t.next_play_start(1 * kS) == 1 * kS);
  CHECK(t.next_play_start(2 * kS) == 5 * kS);
  CHECK(t.next_play_start(3 * kS) == 5 * kS);
  CHECK(t.next_play_start(2 * kS - 10'000'000, 20'000'000) == 5 * kS);  // within the lead of the end
  CHECK(t.next_play_start(10 * kS) == -1);

  REQUIRE(t.set_in(1 * kS));   // timeline 1 s = source 1 s
  CHECK(t.pieces().front().in_ns == 1 * kS);
  REQUIRE(t.set_out(5 * kS));  // timeline 5 s (from 1 s) = source 9 s
  CHECK(t.pieces().back().out_ns == 9 * kS);
  CHECK(t.length() == 5 * kS);
  REQUIRE(t.pieces().size() == 2);
  CHECK_FALSE(t.remove(5));
  REQUIRE(t.remove(0));
  CHECK_FALSE(t.remove(0));  // never the last piece
}

TEST_CASE("undo and redo walk the edits; a new edit drops the redo", "[timeline][pr30]") {
  video_timeline t;
  t.load(8 * kS);
  REQUIRE(t.split(3 * kS));
  REQUIRE(t.remove(0));
  CHECK(t.length() == 5 * kS);
  REQUIRE(t.undo());
  CHECK(t.pieces().size() == 2);
  REQUIRE(t.undo());
  CHECK_FALSE(t.edited());
  CHECK_FALSE(t.undo());
  REQUIRE(t.redo());
  CHECK(t.pieces().size() == 2);
  REQUIRE(t.split(6 * kS));
  CHECK_FALSE(t.can_redo());
  t.load(4 * kS);  // another clip: a fresh history
  CHECK_FALSE(t.can_undo());
}

TEST_CASE("the export is keep_ranges over the pieces", "[timeline][pr30]") {
  video_timeline t;
  t.load(10 * kS);
  REQUIRE(t.split(2 * kS));
  REQUIRE(t.split(6 * kS));
  REQUIRE(t.remove(1));
  const auto fast = t.export_request("a.mp4", false);
  CHECK(fast.kind == mv::edit::clip::op::keep_ranges);
  CHECK_FALSE(fast.ranges_exact);
  REQUIRE(fast.ranges.size() == 2);
  CHECK(fast.ranges[0].in_ns == 0);
  CHECK(fast.ranges[0].out_ns == 2 * kS);
  CHECK(fast.ranges[1].in_ns == 6 * kS);
  CHECK(fast.ranges[1].out_ns == -1);  // to the end of the clip
  CHECK(t.export_request("a.mp4", true).ranges_exact);
}
