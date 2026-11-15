// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host drives copy` (docs/plans/self-host-drives-keep-a-copy.md):
// the pure planner (what to copy, what goes to the copy's Recycle Bin, what
// waits) and the bin's rules (bin.json, expiry, trimming for room, where a
// restored item goes). Nothing here touches a disk.
#include "catch_compat.h"

#include <set>
#include <string>
#include <vector>

#include "addons/driftwire/cli/drive_copy.h"

namespace dc = mv::driftwire::cli::drive_copy;

namespace {

constexpr std::int64_t kNow = 1'763'200'000;  // 2025-11-15 09:46:40 UTC
constexpr std::int64_t kOld = kNow - 86'400;  // a day ago: settled

dc::entry file(std::string path, std::uint64_t size, std::int64_t mtime = kOld) {
  return {std::move(path), dc::entry_kind::file, size, mtime};
}
dc::entry folder(std::string path) { return {std::move(path), dc::entry_kind::folder, 0, 0}; }
dc::entry link(std::string path) { return {std::move(path), dc::entry_kind::link, 0, 0}; }

dc::plan run(const std::vector<dc::entry>& source, const std::vector<dc::entry>& copy,
             std::uint64_t free_bytes = 1'000'000'000) {
  dc::plan_input in;
  in.source = source;
  in.copy = copy;
  in.now = kNow;
  in.copy_free = free_bytes;
  return dc::make_plan(in);
}

std::vector<std::string> paths_of(const dc::plan& p, dc::action_kind k) {
  std::vector<std::string> out;
  for (const auto& a : p.actions) {
    if (a.kind == k) out.push_back(a.path);
  }
  return out;
}

const dc::action* find(const dc::plan& p, dc::action_kind k, const std::string& path) {
  for (const auto& a : p.actions) {
    if (a.kind == k && a.path == path) return &a;
  }
  return nullptr;
}

std::size_t index_of(const dc::plan& p, dc::action_kind k, const std::string& path) {
  for (std::size_t i = 0; i < p.actions.size(); ++i) {
    if (p.actions[i].kind == k && p.actions[i].path == path) return i;
  }
  return p.actions.size();
}

dc::bin_item item(std::string id, std::uint64_t bytes, std::int64_t removed_at,
                  dc::bin_reason r = dc::bin_reason::deleted) {
  dc::bin_item it;
  it.id = std::move(id);
  it.original = "Photos/" + it.id + ".jpg";
  it.bytes = bytes;
  it.removed_at = removed_at;
  it.reason = r;
  return it;
}

}  // namespace

TEST_CASE("a first copy copies every file and nothing goes to the bin", "[drive-copy]") {
  const auto p = run({folder("2024"), file("2024/a.jpg", 100), file("b.mov", 50)}, {});
  CHECK(paths_of(p, dc::action_kind::copy).size() == 2);
  CHECK(paths_of(p, dc::action_kind::to_bin).empty());
  CHECK(p.bytes_to_copy == 150);
  CHECK(p.files_to_copy == 2);
  CHECK(p.fits);
}

TEST_CASE("an unchanged file is left alone", "[drive-copy]") {
  const auto p = run({file("a.jpg", 100)}, {file("a.jpg", 100)});
  CHECK(p.actions.empty());
}

TEST_CASE("a changed file sends the old one to the bin, then copies", "[drive-copy]") {
  SECTION("a different size") {
    const auto p = run({file("a.jpg", 120)}, {file("a.jpg", 100)});
    const auto* old = find(p, dc::action_kind::to_bin, "a.jpg");
    REQUIRE(old);
    CHECK(old->reason == dc::bin_reason::replaced);
    CHECK(old->size == 100);
    CHECK(index_of(p, dc::action_kind::to_bin, "a.jpg") < index_of(p, dc::action_kind::copy, "a.jpg"));
  }
  SECTION("the same size, a different time") {
    const auto p = run({file("a.jpg", 100, kOld + 5)}, {file("a.jpg", 100, kOld)});
    CHECK(find(p, dc::action_kind::to_bin, "a.jpg"));
    CHECK(find(p, dc::action_kind::copy, "a.jpg"));
  }
}

TEST_CASE("a deleted file goes to the bin, marked deleted", "[drive-copy]") {
  const auto p = run({file("keep.jpg", 1)}, {file("keep.jpg", 1), file("gone.jpg", 42)});
  const auto* a = find(p, dc::action_kind::to_bin, "gone.jpg");
  REQUIRE(a);
  CHECK(a->reason == dc::bin_reason::deleted);
  CHECK(a->size == 42);
  CHECK_FALSE(a->folder);
}

TEST_CASE("a deleted folder goes to the bin as one item with all its bytes", "[drive-copy]") {
  const auto p = run({file("keep.jpg", 1)},
                     {file("keep.jpg", 1), folder("Trip"), file("Trip/1.jpg", 10), folder("Trip/Day 2"),
                      file("Trip/Day 2/2.jpg", 20)});
  const auto binned = paths_of(p, dc::action_kind::to_bin);
  REQUIRE(binned == std::vector<std::string>{"Trip"});
  const auto* a = find(p, dc::action_kind::to_bin, "Trip");
  CHECK(a->folder);
  CHECK(a->size == 30);
}

TEST_CASE("a renamed file is a new copy and the old name goes to the bin", "[drive-copy]") {
  const auto p = run({file("new.jpg", 7)}, {file("old.jpg", 7)});
  CHECK(find(p, dc::action_kind::copy, "new.jpg"));
  CHECK(find(p, dc::action_kind::to_bin, "old.jpg"));
}

TEST_CASE("a file still being written waits for the next pass", "[drive-copy]") {
  SECTION("changed in the last few seconds") {
    const auto p = run({file("live.mov", 500, kNow - 2), file("done.jpg", 5)}, {});
    CHECK(find(p, dc::action_kind::hold, "live.mov"));
    CHECK_FALSE(find(p, dc::action_kind::copy, "live.mov"));
    CHECK(find(p, dc::action_kind::copy, "done.jpg"));
    CHECK(p.held == 1);
    CHECK(p.bytes_to_copy == 5);
  }
  SECTION("grew since the last pass, though its time is old (a copy tool that sets the time first)") {
    const std::vector<dc::entry> before{file("in.mov", 100)};
    const std::vector<dc::entry> now{file("in.mov", 300)};
    dc::plan_input in;
    in.source = now;
    in.previous_source = before;
    in.has_previous = true;
    in.now = kNow;
    in.copy_free = 1'000'000;
    const auto p = dc::make_plan(in);
    CHECK(find(p, dc::action_kind::hold, "in.mov"));
    // The pass after, unchanged: it is copied.
    in.previous_source = now;
    CHECK(find(dc::make_plan(in), dc::action_kind::copy, "in.mov"));
  }
  SECTION("a file first seen this pass with an old time is copied at once") {
    const std::vector<dc::entry> before{};
    const std::vector<dc::entry> now{file("moved-in.jpg", 9)};
    dc::plan_input in;
    in.source = now;
    in.previous_source = before;
    in.has_previous = true;
    in.now = kNow;
    in.copy_free = 1'000;
    CHECK(find(dc::make_plan(in), dc::action_kind::copy, "moved-in.jpg"));
  }
}

TEST_CASE("links are listed and skipped, never followed", "[drive-copy]") {
  const auto p = run({link("shortcut"), file("a.jpg", 1)}, {});
  CHECK(find(p, dc::action_kind::skip_link, "shortcut"));
  CHECK_FALSE(find(p, dc::action_kind::copy, "shortcut"));
  CHECK(p.links_skipped == 1);
}

TEST_CASE("a copy that does not fit says so", "[drive-copy]") {
  const auto p = run({file("big.mov", 2'000)}, {}, 1'999);
  CHECK_FALSE(p.fits);
  CHECK(run({file("big.mov", 2'000)}, {}, 2'000).fits);
}

TEST_CASE("leftover half-written files of ours are dropped, not binned", "[drive-copy]") {
  const auto p = run({file("a.jpg", 10)}, {file("a.jpg.mvpart", 4)});
  CHECK(find(p, dc::action_kind::drop_partial, "a.jpg.mvpart"));
  CHECK(paths_of(p, dc::action_kind::to_bin).empty());
  CHECK(find(p, dc::action_kind::copy, "a.jpg"));
}

TEST_CASE("a name changed only in case is renamed, parent first, not copied again", "[drive-copy]") {
  const auto p = run({folder("Trip"), file("Trip/IMG.jpg", 5)}, {folder("trip"), file("trip/img.jpg", 5)});
  const auto* parent = find(p, dc::action_kind::rename_case, "Trip");
  const auto* child = find(p, dc::action_kind::rename_case, "Trip/IMG.jpg");
  REQUIRE(parent);
  REQUIRE(child);
  CHECK(parent->from == "trip");
  CHECK(child->from == "Trip/img.jpg");  // spelled after its parent's rename
  CHECK(index_of(p, dc::action_kind::rename_case, "Trip") < index_of(p, dc::action_kind::rename_case, "Trip/IMG.jpg"));
  CHECK(paths_of(p, dc::action_kind::copy).empty());
  CHECK(paths_of(p, dc::action_kind::to_bin).empty());
}

TEST_CASE("a file deleted inside a folder renamed in case is binned under the new spelling", "[drive-copy]") {
  const auto p = run({folder("Trip")}, {folder("trip"), file("trip/old.jpg", 3)});
  CHECK(find(p, dc::action_kind::to_bin, "Trip/old.jpg"));
}

TEST_CASE("a file where the copy has a folder of that name: the folder goes to the bin first", "[drive-copy]") {
  const auto p = run({file("x", 4)}, {folder("x"), file("x/inner.jpg", 2)});
  const auto* binned = find(p, dc::action_kind::to_bin, "x");
  REQUIRE(binned);
  CHECK(binned->folder);
  CHECK(index_of(p, dc::action_kind::to_bin, "x") < index_of(p, dc::action_kind::copy, "x"));
}

TEST_CASE("smaller top-level folders are copied first", "[drive-copy]") {
  const auto p = run({file("Big/1.mov", 900), file("Small/1.jpg", 10), file("Small/2.jpg", 10)}, {});
  const auto copies = paths_of(p, dc::action_kind::copy);
  REQUIRE(copies.size() == 3);
  CHECK(copies.back() == "Big/1.mov");
}

TEST_CASE("bin.json round-trips", "[drive-copy]") {
  dc::bin b;
  b.items.push_back(item("20261115-140203-1", 10, kNow));
  auto f = item("20261115-140203-2", 20, kNow + 1, dc::bin_reason::replaced);
  f.folder = true;
  f.original = "Trip/Day 2";
  b.items.push_back(f);
  const auto back = dc::bin_from_json(dc::to_json(b));
  REQUIRE(back);
  REQUIRE(back->items.size() == 2);
  CHECK(back->items[1].original == "Trip/Day 2");
  CHECK(back->items[1].reason == dc::bin_reason::replaced);
  CHECK(back->items[1].folder);
  CHECK(back->items[0].bytes == 10);
  CHECK(back->items[0].removed_at == kNow);
}

TEST_CASE("a bin.json that is cut short or changed is refused", "[drive-copy]") {
  dc::bin b;
  b.items.push_back(item("a-1", 10, kNow));
  const std::string good = dc::to_json(b);
  CHECK_FALSE(dc::bin_from_json(good.substr(0, good.size() - 3)));
  CHECK_FALSE(dc::bin_from_json(""));
  CHECK_FALSE(dc::bin_from_json(R"({"items":[{"id":"../x","original":"a.jpg","bytes":1,"removed_at":1,"reason":"deleted"}]})"));
  CHECK_FALSE(dc::bin_from_json(R"({"items":[{"id":"a","original":"../../Windows/x","bytes":1,"removed_at":1,"reason":"deleted"}]})"));
  CHECK_FALSE(dc::bin_from_json(R"({"items":[{"id":"a","original":"C:/x","bytes":1,"removed_at":1,"reason":"deleted"}]})"));
  CHECK_FALSE(dc::bin_from_json(R"({"items":[{"id":"a","original":"x","bytes":-1,"removed_at":1,"reason":"deleted"}]})"));
  CHECK_FALSE(dc::bin_from_json(R"({"items":[{"id":"a","original":"x","bytes":1,"removed_at":1,"reason":"eaten"}]})"));
  CHECK_FALSE(dc::bin_from_json(
      R"({"items":[{"id":"a","original":"x","bytes":1,"removed_at":1,"reason":"deleted"},{"id":"a","original":"y","bytes":1,"removed_at":1,"reason":"deleted"}]})"));
  // A field it does not know is fine (a later version's).
  CHECK(dc::bin_from_json(R"({"version":2,"items":[{"id":"a","original":"x","bytes":1,"removed_at":1,"reason":"deleted","note":"later"}]})"));
}

TEST_CASE("safe paths for the bin", "[drive-copy]") {
  CHECK(dc::safe_relative("Trip/Day 2/IMG.jpg"));
  CHECK_FALSE(dc::safe_relative(""));
  CHECK_FALSE(dc::safe_relative("/etc/passwd"));
  CHECK_FALSE(dc::safe_relative("a/../b"));
  CHECK_FALSE(dc::safe_relative("a//b"));
  CHECK_FALSE(dc::safe_relative("a\\b"));
  CHECK_FALSE(dc::safe_relative("./a"));
  CHECK(dc::safe_id("20261115-140203-1"));
  CHECK_FALSE(dc::safe_id("a/b"));
  CHECK_FALSE(dc::safe_id(".."));
}

TEST_CASE("a new item's id is its UTC time and the lowest free number", "[drive-copy]") {
  dc::bin b;
  const auto first = dc::next_id(b, kNow);
  CHECK(first == "20251115-094640-1");
  b.items.push_back(item(first, 1, kNow));
  CHECK(dc::next_id(b, kNow) == "20251115-094640-2");
  CHECK(dc::next_id(b, 0) == "19700101-000000-1");
  CHECK(dc::next_id(b, 951'782'400) == "20000229-000000-1");  // a leap day
}

TEST_CASE("items expire after the chosen time, oldest first", "[drive-copy]") {
  constexpr std::int64_t day = 86'400;
  dc::bin b;
  b.items.push_back(item("new", 1, kNow - 1 * day));
  b.items.push_back(item("month", 1, kNow - 30 * day));
  b.items.push_back(item("week", 1, kNow - 8 * day));
  b.items.push_back(item("ancient", 1, kNow - 100 * day));
  CHECK(dc::expired(b, dc::keep_for::days30, kNow) == std::vector<std::string>{"ancient", "month"});
  CHECK(dc::expired(b, dc::keep_for::days7, kNow) == std::vector<std::string>{"ancient", "month", "week"});
  CHECK(dc::expired(b, dc::keep_for::days90, kNow) == std::vector<std::string>{"ancient"});
  CHECK(dc::expired(b, dc::keep_for::until_emptied, kNow).empty());
  CHECK(dc::kDefaultKeep == dc::keep_for::days30);
}

TEST_CASE("trimming for room takes the oldest, and only while the bin is over its share", "[drive-copy]") {
  dc::bin b;
  b.items.push_back(item("b", 50, kNow - 2));
  b.items.push_back(item("a", 50, kNow - 3));
  b.items.push_back(item("c", 50, kNow - 1));
  // Drive 1000: the share is 100. The bin holds 150.
  SECTION("there is room: nothing goes") { CHECK(dc::trim_for_room(b, 1'000, 300, 200).empty()); }
  SECTION("short of room: the oldest goes, then it stops at the share") {
    CHECK(dc::trim_for_room(b, 1'000, 0, 500) == std::vector<std::string>{"a"});
  }
  SECTION("one item is enough") { CHECK(dc::trim_for_room(b, 1'000, 160, 200) == std::vector<std::string>{"a"}); }
  SECTION("a bin within its share is never trimmed") {
    CHECK(dc::trim_for_room(b, 10'000, 0, 5'000).empty());
  }
}

TEST_CASE("a restored item never overwrites what is there", "[drive-copy]") {
  std::set<std::string> taken;
  const auto exists = [&](std::string_view p) { return taken.count(std::string(p)) > 0; };
  CHECK(dc::restore_path("Trip/IMG.jpg", false, exists) == "Trip/IMG.jpg");
  taken = {"Trip/IMG.jpg"};
  CHECK(dc::restore_path("Trip/IMG.jpg", false, exists) == "Trip/IMG (restored).jpg");
  taken.insert("Trip/IMG (restored).jpg");
  CHECK(dc::restore_path("Trip/IMG.jpg", false, exists) == "Trip/IMG (restored 2).jpg");
  taken = {"notes", ".profile", "Trip.2024", "a.tar.gz"};
  CHECK(dc::restore_path("notes", false, exists) == "notes (restored)");
  CHECK(dc::restore_path(".profile", false, exists) == ".profile (restored)");
  CHECK(dc::restore_path("Trip.2024", true, exists) == "Trip.2024 (restored)");
  CHECK(dc::restore_path("a.tar.gz", false, exists) == "a.tar (restored).gz");
}

TEST_CASE("the bin's line gives counts and size, in each OS's word", "[drive-copy]") {
  dc::bin b;
  CHECK(dc::bin_line(b, false) == "Recycle Bin: empty");
  CHECK(dc::bin_line(b, true) == "Trash: empty");
  b.items.push_back(item("a", 1'500'000'000, kNow));
  CHECK(dc::bin_line(b, false) == "Recycle Bin: 1 item, 1.5 GB");
  b.items.push_back(item("b", 1'700'000'000, kNow));
  CHECK(dc::bin_line(b, true) == "Trash: 2 items, 3.2 GB");
}
