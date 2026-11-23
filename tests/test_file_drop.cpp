// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/file_drop.h: what a drop on a folder becomes, and where each file lands.
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "shell/file_drop.h"

using namespace mv::shell;

namespace {

// "photos:" and "/ro" are read-only, "remote:" another computer's; the rest writable.
place_kind kinds(std::string_view p) {
  if (p.starts_with("photos:") || p.starts_with("/ro")) return place_kind::read_only;
  if (p.starts_with("remote:rw")) return place_kind::remote_writable;
  if (p.starts_with("remote:")) return place_kind::remote_read_only;
  return place_kind::writable;
}

drop_decision decide(std::vector<std::string> sources, std::string target, bool from_app, bool copy_key = false,
                     bool move_key = false) {
  drop_request r;
  r.sources = std::move(sources);
  r.target_dir = std::move(target);
  r.from_app = from_app;
  r.copy_key = copy_key;
  r.move_key = move_key;
  return decide_drop(r, kinds);
}

}  // namespace

TEST_CASE("file_drop: a drag inside the app moves, Option copies", "[file_drop]") {
  CHECK(decide({"/p/a.jpg"}, "/p/trip", true).verb == drop_verb::move);
  CHECK(decide({"/p/a.jpg"}, "/p/trip", true, /*copy_key=*/true).verb == drop_verb::copy);
}

TEST_CASE("file_drop: a drag from Finder or another app adds a copy, Cmd moves", "[file_drop]") {
  CHECK(decide({"/Users/me/Desktop/a.jpg"}, "/p", false).verb == drop_verb::copy);
  CHECK(decide({"/Users/me/Desktop/a.jpg"}, "/p", false, false, /*move_key=*/true).verb == drop_verb::move);
  // Option on an outside drag is still a copy.
  CHECK(decide({"/Users/me/Desktop/a.jpg"}, "/p", false, true).verb == drop_verb::copy);
}

TEST_CASE("file_drop: read-only and remote folders refuse, with their reason", "[file_drop]") {
  auto ro = decide({"/p/a.jpg"}, "/ro/lib", true);
  CHECK_FALSE(ro.allowed());
  CHECK(ro.refusal == drop_refusal::read_only_target);
  auto photos = decide({"/p/a.jpg"}, "photos:album", false);
  CHECK(photos.refusal == drop_refusal::read_only_target);
  auto remote = decide({"/p/a.jpg"}, "remote:nas/Trips", false);
  CHECK(remote.refusal == drop_refusal::remote_target);
  CHECK(std::string(drop_refusal_words(remote.refusal)).find("another computer") != std::string::npos);
  // A folder the service lets this device change takes the drop: phase 2 only swaps classify.
  CHECK(decide({"/p/a.jpg"}, "remote:rw/Trips", false).verb == drop_verb::copy);
}

TEST_CASE("file_drop: a folder never goes into itself", "[file_drop]") {
  CHECK(decide({"/p/trip"}, "/p/trip", true).refusal == drop_refusal::into_itself);
  CHECK(decide({"/p/trip"}, "/p/trip/day1", true).refusal == drop_refusal::into_itself);
  CHECK(decide({"/p/a.jpg", "/p/trip"}, "/p/trip/day1/", false, true).refusal == drop_refusal::into_itself);
  // A sibling whose name starts the same is not inside it.
  CHECK(decide({"/p/trip"}, "/p/trip2", true).verb == drop_verb::move);
}

TEST_CASE("file_drop: moving files onto the folder they are in does nothing; copying duplicates",
          "[file_drop]") {
  CHECK(decide({"/p/a.jpg", "/p/b.jpg"}, "/p", true).refusal == drop_refusal::already_there);
  CHECK(decide({"/p/a.jpg"}, "/p/", true).refusal == drop_refusal::already_there);
  CHECK(decide({"/p/a.jpg"}, "/p", true, true).verb == drop_verb::copy);
  // One of them from elsewhere: the move goes ahead (the plan skips the one already there).
  CHECK(decide({"/p/a.jpg", "/q/b.jpg"}, "/p", true).verb == drop_verb::move);
}

TEST_CASE("file_drop: what cannot leave where it is is copied", "[file_drop]") {
  CHECK(decide({"photos:ABC"}, "/p", true).verb == drop_verb::copy);
  CHECK(decide({"/ro/x.jpg", "/q/y.jpg"}, "/p", true).verb == drop_verb::copy);
  // A view device's file comes down as a copy; it is never removed from the server.
  CHECK(decide({"remote:nas/Trips/a.jpg"}, "/p", true).verb == drop_verb::copy);
}

TEST_CASE("file_drop: a change device's remote folders take drops and give files", "[file_drop]") {
  // Up: from Finder a copy, from this window a move (the local file goes once the copy is there).
  CHECK(decide({"/p/a.jpg"}, "remote:rw/Trips/", false).verb == drop_verb::copy);
  CHECK(decide({"/p/a.jpg"}, "remote:rw/Trips/", true).verb == drop_verb::move);
  CHECK(decide({"/p/day1"}, "remote:rw/Trips/", false).verb == drop_verb::copy);  // a local folder goes up whole
  // Within its server: the service's move, files or folders; a move onto its own folder is nothing.
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "remote:rw/Trips/2026/", true).verb == drop_verb::move);
  CHECK(decide({"remote:rw/Trips/day1/"}, "remote:rw/Archive/", true).verb == drop_verb::move);
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "remote:rw/Trips/", true).refusal == drop_refusal::already_there);
  // A copy within it comes down and goes up again (a file); a folder has no such copy.
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "remote:rw/Archive/", true, true).verb == drop_verb::copy);
  CHECK(decide({"remote:rw/Trips/day1/"}, "remote:rw/Archive/", true, true).refusal == drop_refusal::remote_source);
  // Down: a file is a download (a move removes it from the server only after); a folder is not.
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "/p", true).verb == drop_verb::move);
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "/p", true, true).verb == drop_verb::copy);
  CHECK(decide({"remote:rw/Trips/day1/"}, "/p", true).refusal == drop_refusal::remote_source);
  // Never from one server to another.
  CHECK(decide({"remote:nas/Trips/a.jpg"}, "remote:rw/Trips/", true).refusal == drop_refusal::remote_source);
  CHECK(decide({"remote:rw/Trips/a.jpg"}, "remote:rw2/Trips/", true).refusal == drop_refusal::remote_source);
}

TEST_CASE("file_drop: no folder, or no files of ours, is no drop", "[file_drop]") {
  CHECK(decide({}, "/p", true).refusal == drop_refusal::nothing);
  CHECK(decide({"/p/a.jpg"}, "", false).refusal == drop_refusal::nothing);
}

TEST_CASE("file_drop: an outside drag hovering is judged by the folder alone", "[file_drop]") {
  CHECK(decide({}, "/p", false).verb == drop_verb::copy);
  CHECK(decide({}, "/p", false, false, true).verb == drop_verb::move);
  CHECK(decide({}, "remote:nas/Trips", false).refusal == drop_refusal::remote_target);
  CHECK(decide({}, "/ro/lib", false).refusal == drop_refusal::read_only_target);
}

TEST_CASE("plan_transfer: never a name that exists, nor one the batch already took", "[file_drop]") {
  const std::set<std::string> there = {"/t/a.jpg", "/t/a (2).jpg", "/t/trip"};
  auto exists = [&](std::string_view p) { return there.count(std::string(p)) != 0; };
  const auto plan = plan_transfer({"/x/a.jpg", "/y/a.jpg", "/x/b.jpg", "/x/trip"}, "/t/", drop_verb::copy, exists);
  REQUIRE(plan.size() == 4);
  CHECK(plan[0].dest == "/t/a (3).jpg");
  CHECK(plan[1].dest == "/t/a (4).jpg");
  CHECK(plan[2].dest == "/t/b.jpg");
  CHECK(plan[3].dest == "/t/trip (2)");
}

TEST_CASE("plan_transfer: a move skips what is already in the folder", "[file_drop]") {
  auto none = [](std::string_view) { return false; };
  const auto plan = plan_transfer({"/t/a.jpg", "/x/b.jpg"}, "/t", drop_verb::move, none);
  REQUIRE(plan.size() == 1);
  CHECK(plan[0].source == "/x/b.jpg");
  CHECK(plan[0].dest == "/t/b.jpg");
  // A copy into the same folder is Finder's duplicate.
  const auto dup = plan_transfer({"/t/a.jpg"}, "/t", drop_verb::copy,
                                 [](std::string_view p) { return p == "/t/a.jpg"; });
  REQUIRE(dup.size() == 1);
  CHECK(dup[0].dest == "/t/a (2).jpg");
}

TEST_CASE("plan_transfer: Windows folders keep their separator", "[file_drop]") {
  auto none = [](std::string_view) { return false; };
  const auto plan = plan_transfer({"D:\\cam\\IMG_1.JPG"}, "C:\\Photos\\", drop_verb::copy, none);
  REQUIRE(plan.size() == 1);
  CHECK(plan[0].dest == "C:\\Photos\\IMG_1.JPG");
  const auto root = plan_transfer({"D:\\cam\\IMG_1.JPG"}, "C:\\", drop_verb::copy, none);
  CHECK(root[0].dest == "C:\\IMG_1.JPG");
}

TEST_CASE("file_drop: a delete is all here, all on one server, or refused as a mix", "[file_drop]") {
  CHECK(trash_kind_of({"/p/a.jpg", "/p/b.jpg"}) == trash_kind::local);
  CHECK(trash_kind_of({"remote:aa/r1/a.jpg", "remote:aa/r2/b.jpg"}) == trash_kind::remote);
  CHECK(trash_kind_of({"remote:aa/r1/a.jpg", "/p/b.jpg"}) == trash_kind::mixed);
  CHECK(trash_kind_of({"/p/b.jpg", "remote:aa/r1/a.jpg"}) == trash_kind::mixed);
  CHECK(trash_kind_of({"remote:aa/r1/a.jpg", "remote:bb/r1/a.jpg"}) == trash_kind::mixed);
  CHECK(trash_kind_of({}) == trash_kind::local);
}
