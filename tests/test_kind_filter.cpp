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
  CHECK(next_kind_filter(kind_filter::marked) == kind_filter::all);
  CHECK(mv::io::kind_filter_from(9) == kind_filter::all);
  CHECK(mv::io::kind_filter_from(2) == kind_filter::videos);
  CHECK(std::string_view(mv::io::kind_filter_name(kind_filter::raw)) == "RAW");
}
