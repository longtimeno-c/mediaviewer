// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #206: View > Show, the kind filter both hosts apply (io::keeps_stop).
#include <catch2/catch_test_macros.hpp>

#include "io/pairing.h"

using mv::io::kind_filter;
using mv::io::kind_filter_mask;
using mv::io::keeps_stop;

namespace {

bool shown(std::string_view name, kind_filter f, std::string_view secondary = {}) {
  return keeps_stop(name, secondary, false, false, 0, kind_filter_mask(f), false);
}

}  // namespace

TEST_CASE("each stop is one kind", "[io][filter]") {
  using namespace mv::io;
  CHECK(is_hidden_stop("A.JPG", {}, kHideStills));
  CHECK_FALSE(is_hidden_stop("A.JPG", {}, kHideRaw | kHideClips));
  CHECK(is_hidden_stop("A.CR3", {}, kHideRaw));
  CHECK_FALSE(is_hidden_stop("A.CR3", {}, kHideStills));
  // A RAW+JPEG pair is RAW whichever half leads.
  CHECK(is_hidden_stop("A.JPG", "A.NEF", kHideRaw));
  CHECK_FALSE(is_hidden_stop("A.JPG", "A.NEF", kHideStills));
  // A Live Photo is its still; a clip is a clip; audio is not a clip here.
  CHECK(is_hidden_stop("IMG.HEIC", "IMG.MOV", kHideStills));
  CHECK(is_hidden_stop("C.MP4", {}, kHideClips));
  CHECK_FALSE(is_hidden_stop("S.MP3", {}, kHideClips));
  CHECK(is_hidden_stop("S.MP3", {}, kHideAudio));
  CHECK(is_hidden_stop("D.PDF", {}, kHideDocuments));
  CHECK_FALSE(is_hidden_stop("D.PDF", {}, kHideStills));
  // The unmarked bit is not a kind.
  CHECK_FALSE(is_hidden_stop("A.JPG", {}, kHideUnmarked));
}

TEST_CASE("a filter shows only its kind", "[io][filter]") {
  CHECK(shown("a.jpg", kind_filter::all));
  CHECK(shown("s.mp3", kind_filter::all));

  CHECK(shown("a.jpg", kind_filter::photos));
  CHECK(shown("a.dng", kind_filter::photos));
  CHECK(shown("a.gif", kind_filter::photos));
  CHECK_FALSE(shown("c.mov", kind_filter::photos));
  CHECK_FALSE(shown("s.mp3", kind_filter::photos));
  CHECK_FALSE(shown("d.pdf", kind_filter::photos));

  CHECK(shown("c.mov", kind_filter::videos));
  CHECK_FALSE(shown("a.jpg", kind_filter::videos));
  CHECK_FALSE(shown("a.arw", kind_filter::videos));
  CHECK_FALSE(shown("s.m4a", kind_filter::videos));

  CHECK(shown("a.arw", kind_filter::raw));
  CHECK(shown("a.jpg", kind_filter::raw, "a.arw"));
  CHECK_FALSE(shown("a.jpg", kind_filter::raw));
  CHECK_FALSE(shown("c.mp4", kind_filter::raw));
}

TEST_CASE("marked keeps the kept set, of any kind", "[io][filter]") {
  const auto mask = kind_filter_mask(kind_filter::marked);
  CHECK(keeps_stop("a.jpg", {}, true, false, 0, mask, false));
  CHECK(keeps_stop("c.mp4", {}, true, false, 0, mask, false));
  CHECK_FALSE(keeps_stop("a.jpg", {}, false, false, 0, mask, false));
  // Settings' hidden kinds still apply to a marked stop.
  CHECK_FALSE(keeps_stop("s.mp3", {}, true, false, mv::io::kHideAudio, mask, false));
}

TEST_CASE("the staying stop survives a relist, not a new filter", "[io][filter]") {
  const auto videos = kind_filter_mask(kind_filter::videos);
  // The file a folder was opened on, or the stop on screen: an explicit open
  // still shows under a filter, as under Settings' kinds.
  CHECK(keeps_stop("a.jpg", {}, false, true, 0, videos, false));
  CHECK(keeps_stop("s.mp3", {}, false, true, mv::io::kHideAudio, 0, false));
  // The relist a new filter choice makes moves on.
  CHECK_FALSE(keeps_stop("a.jpg", {}, false, true, 0, videos, true));
  // Settings' rule is unchanged by a filter change.
  CHECK(keeps_stop("s.mp3", {}, false, true, mv::io::kHideAudio, 0, true));
}

TEST_CASE("the filter cycles in menu order", "[io][filter]") {
  using mv::io::next_kind_filter;
  CHECK(next_kind_filter(kind_filter::all) == kind_filter::photos);
  CHECK(next_kind_filter(kind_filter::raw) == kind_filter::marked);
  CHECK(next_kind_filter(kind_filter::marked) == kind_filter::picks);  // issue #207 continues it
  CHECK(mv::io::kind_filter_from(99) == kind_filter::all);
  CHECK(mv::io::kind_filter_from(2) == kind_filter::videos);
  CHECK(std::string_view(mv::io::kind_filter_name(kind_filter::raw)) == "RAW");
}

// ---- Issue #207: the cull filters, on rating and colour label ----------------

namespace {

bool culled_shown(kind_filter f, std::int8_t rating, mv::io::cull_label label,
                  std::string_view name = "a.jpg") {
  return keeps_stop(name, {}, false, false, 0, kind_filter_mask(f), false,
                    mv::io::cull_marks{rating, label});
}

}  // namespace

TEST_CASE("label words map to Lightroom's colours", "[io][filter][cull]") {
  using mv::io::cull_label;
  using mv::io::cull_label_of;
  CHECK(cull_label_of("") == cull_label::none);
  CHECK(cull_label_of("  ") == cull_label::none);
  CHECK(cull_label_of("Red") == cull_label::red);
  CHECK(cull_label_of("red") == cull_label::red);
  CHECK(cull_label_of(" YELLOW ") == cull_label::yellow);
  CHECK(cull_label_of("Green") == cull_label::green);
  CHECK(cull_label_of("Blue") == cull_label::blue);
  CHECK(cull_label_of("Purple") == cull_label::purple);
  CHECK(cull_label_of("To Do") == cull_label::other);
  CHECK(cull_label_of("Reds") == cull_label::other);
}

TEST_CASE("picks, not rejected and a colour filter on the marks", "[io][filter][cull]") {
  using mv::io::cull_label;
  // Picks: one star or more.
  CHECK(culled_shown(kind_filter::picks, 1, cull_label::none));
  CHECK(culled_shown(kind_filter::picks, 5, cull_label::red));
  CHECK_FALSE(culled_shown(kind_filter::picks, 0, cull_label::red));
  CHECK_FALSE(culled_shown(kind_filter::picks, -1, cull_label::none));
  // Not rejected: everything but -1, unrated included.
  CHECK(culled_shown(kind_filter::not_rejected, 0, cull_label::none));
  CHECK(culled_shown(kind_filter::not_rejected, 3, cull_label::none));
  CHECK_FALSE(culled_shown(kind_filter::not_rejected, -1, cull_label::green));
  // A colour: that label only, whatever the rating.
  CHECK(culled_shown(kind_filter::red, 0, cull_label::red));
  CHECK(culled_shown(kind_filter::red, -1, cull_label::red));
  CHECK_FALSE(culled_shown(kind_filter::red, 5, cull_label::yellow));
  CHECK_FALSE(culled_shown(kind_filter::red, 5, cull_label::none));
  CHECK_FALSE(culled_shown(kind_filter::red, 5, cull_label::other));
  CHECK(culled_shown(kind_filter::yellow, 0, cull_label::yellow));
  CHECK(culled_shown(kind_filter::green, 0, cull_label::green));
  CHECK(culled_shown(kind_filter::blue, 0, cull_label::blue));
  CHECK(culled_shown(kind_filter::purple, 0, cull_label::purple));
  CHECK_FALSE(culled_shown(kind_filter::purple, 0, cull_label::blue));
  // They do not care what kind a file is: a labelled clip or RAW shows.
  CHECK(culled_shown(kind_filter::red, 0, cull_label::red, "c.mov"));
  CHECK(culled_shown(kind_filter::picks, 2, cull_label::none, "a.nef"));
  // A stop not read yet counts as unrated and unlabelled.
  const auto red = kind_filter_mask(kind_filter::red);
  CHECK_FALSE(keeps_stop("a.jpg", {}, false, false, 0, red, false));
  CHECK(keeps_stop("a.jpg", {}, false, false, 0, kind_filter_mask(kind_filter::not_rejected), false));
}

TEST_CASE("a cull filter keeps the staying stop and Settings' kinds", "[io][filter][cull]") {
  using mv::io::cull_label;
  const auto picks = kind_filter_mask(kind_filter::picks);
  const mv::io::cull_marks unrated{};
  // The stop on screen stays under a relist (a rating just changed under it),
  // but not under the relist a new choice makes.
  CHECK(keeps_stop("a.jpg", {}, false, true, 0, picks, false, unrated));
  CHECK_FALSE(keeps_stop("a.jpg", {}, false, true, 0, picks, true, unrated));
  // Settings' hidden audio stays hidden, rated or not.
  CHECK_FALSE(keeps_stop("s.mp3", {}, false, false, mv::io::kHideAudio, picks, false,
                         mv::io::cull_marks{4, cull_label::none}));
}

TEST_CASE("cull filter masks, names and the wire", "[io][filter][cull]") {
  using namespace mv::io;
  for (int i = 0; i < kKindFilterCount; ++i) {
    const auto f = kind_filter_from(i);
    CHECK(static_cast<int>(f) == i);
    CHECK((kind_filter_mask(f) & ~kHideKnown) == 0);
    CHECK((kind_filter_mask(f) & ~(kHideFilterBits | kHideAudio | kHideDocuments)) == 0);
    CHECK(needs_cull_marks(kind_filter_mask(f)) == (i >= kFirstCullFilter));
    CHECK(std::string_view(kind_filter_notice(f)).size() > 0);
  }
  CHECK(kind_filter_from(kKindFilterCount) == kind_filter::all);
  // Windows carries the filter in four bits of the chrome's sort word.
  CHECK(kKindFilterCount <= 16);
  CHECK(next_kind_filter(kind_filter::marked) == kind_filter::picks);
  CHECK(next_kind_filter(kind_filter::purple) == kind_filter::all);
  // The keyed cycle passes over Marked with nothing marked.
  CHECK(next_kind_filter(kind_filter::raw, false) == kind_filter::picks);
  CHECK(next_kind_filter(kind_filter::raw, true) == kind_filter::marked);
  CHECK(next_kind_filter(kind_filter::green, false) == kind_filter::blue);
  CHECK(std::string_view(kind_filter_name(kind_filter::not_rejected)) == "Not rejected");
  CHECK(std::string_view(kind_filter_notice(kind_filter::all)) == "Showing all");
  CHECK(std::string_view(kind_filter_notice(kind_filter::photos)) == "Showing Photos only");
  // The label field holds every colour and nothing else.
  CHECK(label_filter_bits(cull_label::purple) == (5u << kHideLabelShift));
  CHECK((label_filter_bits(cull_label::purple) & ~kHideOtherLabels) == 0);
  // The kind bits never read the marks, and the cull bits never hide a kind.
  CHECK_FALSE(is_hidden_stop("a.jpg", {}, kHideCullBits));
}
