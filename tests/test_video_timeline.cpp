// SPDX-License-Identifier: GPL-3.0-or-later
// shell/video_timeline.h: the Video Editor's cut list (PR 30, docs/design/21).
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

TEST_CASE("I and O mark a range; marking cuts nothing, removing it is one edit", "[timeline][editor-polish]") {
  video_timeline t;
  t.load(10 * kS);
  const auto rev = t.revision();
  t.mark_in(2 * kS);
  t.mark_out(5 * kS);
  CHECK(t.has_marks());
  CHECK(t.marked_in() == 2 * kS);
  CHECK(t.marked_out() == 5 * kS);
  CHECK_FALSE(t.edited());
  CHECK(t.revision() == rev);  // marks are not edits
  t.mark_out(1 * kS);           // before the in: the in goes
  CHECK(t.marked_in() == -1);
  t.mark_in(2 * kS);            // after the out: the out goes
  CHECK(t.marked_out() == -1);
  t.mark_out(5 * kS);
  REQUIRE(t.remove_marked());
  CHECK(t.length() == 7 * kS);
  CHECK_FALSE(t.has_marks());  // the times they named have moved
  CHECK(t.to_source(2 * kS) == 5 * kS);
  REQUIRE(t.undo());
  CHECK_FALSE(t.edited());
  CHECK_FALSE(t.remove_marked());  // nothing marked
}

TEST_CASE("an open-ended mark runs to the program's start or end", "[timeline][editor-polish]") {
  video_timeline t;
  t.load(10 * kS);
  t.mark_in(7 * kS);
  REQUIRE(t.remove_marked());
  CHECK(t.length() == 7 * kS);
  t.mark_out(1 * kS);
  REQUIRE(t.remove_marked());
  CHECK(t.length() == 6 * kS);
  CHECK(t.pieces().front().in_ns == 1 * kS);
  t.mark_in(0);
  t.mark_out(6 * kS);
  CHECK_FALSE(t.remove_marked());  // the whole program: never leave nothing
}

TEST_CASE("a range across a cut removes from both sides and leaves no sliver", "[timeline][editor-polish]") {
  video_timeline t;
  t.load(10 * kS);
  REQUIRE(t.split(4 * kS));
  REQUIRE(t.remove_range(3 * kS, 6 * kS));  // the tail of piece 0 and the head of piece 1
  REQUIRE(t.pieces().size() == 2);
  CHECK(t.pieces()[0].out_ns == 3 * kS);
  CHECK(t.pieces()[1].in_ns == 6 * kS);
  // A range that would leave 10 ms of a piece takes it all.
  REQUIRE(t.remove_range(0, 3 * kS - 10'000'000));
  REQUIRE(t.pieces().size() == 1);
  CHECK(t.pieces()[0].in_ns == 6 * kS);
}

TEST_CASE("dragging a piece's edge trims it inside its neighbours as one undo", "[timeline][editor-polish]") {
  video_timeline t;
  t.load(10 * kS);
  REQUIRE(t.split(3 * kS));
  REQUIRE(t.split(6 * kS));
  REQUIRE(t.remove(1));  // [0,3) [6,10)
  REQUIRE(t.begin_trim(1, video_timeline::edge::in));
  CHECK(t.trim_to(5 * kS) == 5 * kS);  // pull the cut back in
  CHECK(t.trim_to(2 * kS) == 3 * kS);  // never over the piece before
  CHECK(t.trim_to(4 * kS) == 4 * kS);
  t.end_trim();
  CHECK(t.pieces()[1].in_ns == 4 * kS);
  CHECK(t.length() == 9 * kS);
  REQUIRE(t.undo());                   // the whole drag
  CHECK(t.pieces()[1].in_ns == 6 * kS);

  REQUIRE(t.begin_trim(0, video_timeline::edge::out));
  CHECK(t.trim_to(0) == video_timeline::kMinPiece);  // a piece keeps a frame
  CHECK(t.trim_to(9 * kS) == 6 * kS);                // up to the next piece
  t.end_trim();
  REQUIRE(t.begin_trim(1, video_timeline::edge::out));
  CHECK(t.trim_to(20 * kS) == 10 * kS);  // the clip's end
  t.end_trim();

  // A drag that ends where it began leaves no undo step behind.
  video_timeline u;
  u.load(10 * kS);
  REQUIRE(u.begin_trim(0, video_timeline::edge::out));
  u.trim_to(8 * kS);
  u.trim_to(10 * kS);
  u.end_trim();
  CHECK_FALSE(u.can_undo());
  CHECK_FALSE(u.begin_trim(3, video_timeline::edge::in));
  CHECK(u.trim_to(1 * kS) == -1);
}

TEST_CASE("the revision moves on every change to the pieces", "[timeline][editor-polish]") {
  video_timeline t;
  t.load(10 * kS);
  auto r = t.revision();
  REQUIRE(t.split(5 * kS));
  CHECK(t.revision() > r);
  r = t.revision();
  CHECK_FALSE(t.split(5 * kS));
  CHECK(t.revision() == r);  // nothing changed
  REQUIRE(t.undo());
  CHECK(t.revision() > r);
  r = t.revision();
  REQUIRE(t.redo());
  CHECK(t.revision() > r);
}

TEST_CASE("J K L: L goes faster, K stops, J skims back further in a burst", "[timeline][editor-polish]") {
  mv::shell::editor_shuttle s;
  CHECK(s.forward() == 1.0);
  CHECK(s.forward() == 2.0);
  CHECK(s.forward() == 4.0);
  CHECK(s.forward() == 4.0);  // the player's ceiling
  s.stop();
  CHECK(s.forward() == 1.0);

  const std::int64_t ms = 1'000'000;
  CHECK(s.back(30 * kS, 0) == 29 * kS);
  CHECK(s.back(29 * kS, 200 * ms) == 27 * kS);  // 2 s from where the last went
  CHECK(s.back(27 * kS, 400 * ms) == 23 * kS);  // 4 s
  CHECK(s.back(23 * kS, 500 * ms, true) == 19 * kS);  // a held key keeps 4 s
  CHECK(s.back(19 * kS, 600 * ms) == 11 * kS);  // 8 s
  CHECK(s.back(11 * kS, 700 * ms) == 3 * kS);   // still 8 s: the ceiling
  CHECK(s.back(3 * kS, 800 * ms) == 0);         // never before the start
  CHECK(s.back(20 * kS, 5000 * ms) == 19 * kS);  // a pause ends the burst
}
