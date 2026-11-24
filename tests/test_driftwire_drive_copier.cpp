// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keep a copy, slice 2 (docs/plans/self-host-drives-keep-a-copy.md §7): the
// copier on temporary folders standing in for the two drives, copies.json and
// its words, and the copy keeper running passes from copies.json.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/drive_copier.h"
#include "addons/driftwire/cli/host_copy.h"
#include "import_fixture.h"

using namespace mv::test;
namespace cli = mv::driftwire::cli;
namespace dc = mv::driftwire::cli::drive_copy;

namespace {

const std::int64_t kNow = static_cast<std::int64_t>(std::time(nullptr)) + 3600;  // every file written here has settled

void put(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << bytes;
}

std::string got(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// Back-dated, so two writes in one second still differ in time.
void age(const fs::path& p, int hours) {
  fs::last_write_time(p, fs::last_write_time(p) - std::chrono::hours(hours));
}

struct drives_pair {
  scratch_dir dir{"drive_copier"};
  fs::path source = dir / "src";
  fs::path copy = dir / "dst";
  fs::path copied = copy / "MediaViewer copy of Family Media";
  drives_pair() {
    fs::create_directories(source);
    fs::create_directories(copy);
  }
  dc::pass_options options(std::int64_t now = kNow) const {
    dc::pass_options o;
    o.source = utf8(source);
    o.copy_mount = utf8(copy);
    o.label = "Family Media";
    o.now = now;
    return o;
  }
};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("drive copier: the first pass copies everything and changes nothing on the source", "[drive-copier]") {
  drives_pair d;
  put(d.source / "2024" / "Trip" / "IMG_0001.JPG", "jpeg one");
  put(d.source / "2024" / "Trip" / "IMG_0002.JPG", "jpeg two");
  put(d.source / "notes.txt", "hello");
  put(d.source / "System Volume Information" / "x.dat", "the OS's");
  fs::create_directories(d.source / "Empty");
  age(d.source / "notes.txt", 48);
  const auto before = fs::last_write_time(d.source / "notes.txt");
  put(d.copy / "already there.txt", "left alone");

  const dc::pass_result r = dc::run_pass(d.options());
  CHECK(r.trouble == dc::pass_trouble::none);
  CHECK(r.copied == 3);
  CHECK(r.complete());
  CHECK(got(d.copied / "2024" / "Trip" / "IMG_0001.JPG") == "jpeg one");
  CHECK(got(d.copied / "notes.txt") == "hello");
  // Compared as a bool: libc++'s file time is an __int128 Catch can't print.
  const bool kept = fs::last_write_time(d.copied / "notes.txt") == before;
  CHECK(kept);  // the source's time kept
  CHECK(!fs::exists(d.copied / "System Volume Information"));
  // The source: the same bytes and times.
  CHECK(got(d.source / "notes.txt") == "hello");
  const bool untouched = fs::last_write_time(d.source / "notes.txt") == before;
  CHECK(untouched);
  // What was on the copy drive is left alone; the marker says the copy is whole.
  CHECK(got(d.copy / "already there.txt") == "left alone");
  const auto m = dc::read_marker(utf8(d.copy));
  REQUIRE(m);
  CHECK(m->label == "Family Media");
  CHECK(m->complete_at == kNow);
  CHECK(dc::has_marker(utf8(d.copy)));

  // A second pass with nothing changed copies nothing.
  const dc::pass_result again = dc::run_pass(d.options());
  CHECK(again.copied == 0);
  CHECK(again.to_bin == 0);
  CHECK(again.complete());
}

TEST_CASE("drive copier: a file saved over or deleted goes to the copy's Recycle Bin", "[drive-copier]") {
  drives_pair d;
  put(d.source / "a.jpg", "version one");
  put(d.source / "gone.jpg", "to be deleted");
  put(d.source / "Old Trip" / "1.jpg", "one");
  put(d.source / "Old Trip" / "2.jpg", "two");
  age(d.source / "a.jpg", 48);
  REQUIRE(dc::run_pass(d.options()).complete());

  put(d.source / "a.jpg", "version two, longer");
  fs::remove(d.source / "gone.jpg");
  fs::remove_all(d.source / "Old Trip");
  const dc::pass_result r = dc::run_pass(d.options());
  CHECK(r.complete());
  CHECK(got(d.copied / "a.jpg") == "version two, longer");
  CHECK(!fs::exists(d.copied / "gone.jpg"));
  CHECK(!fs::exists(d.copied / "Old Trip"));
  // Three items: the replaced version, the deleted file, and the folder as one.
  REQUIRE(r.bin_now.items.size() == 3);
  const dc::bin b = dc::read_bin(utf8(d.copy));
  REQUIRE(b.items.size() == 3);
  int replaced = 0, folders = 0;
  for (const auto& it : b.items) {
    const fs::path held = d.copy / ".mediaviewer-copy" / "bin" / it.id;
    CHECK(fs::exists(held));
    if (it.reason == dc::bin_reason::replaced) {
      ++replaced;
      CHECK(it.original == "a.jpg");
      CHECK(got(held) == "version one");
    }
    if (it.folder) {
      ++folders;
      CHECK(it.original == "Old Trip");
      CHECK(got(held / "2.jpg") == "two");
    }
  }
  CHECK(replaced == 1);
  CHECK(folders == 1);

  // 31 days on, the bin's items are gone; the live copy stays.
  const dc::pass_result later = dc::run_pass(d.options(kNow + 31 * 86400));
  CHECK(later.expired == 3);
  CHECK(dc::read_bin(utf8(d.copy)).items.empty());
  CHECK(got(d.copied / "a.jpg") == "version two, longer");
}

TEST_CASE("drive copier: a copy cut off mid-file never leaves it under its real name", "[drive-copier]") {
  drives_pair d;
  const std::string big(3u << 20, 'x');  // three chunks
  put(d.source / "big.mov", big);
  put(d.source / "small.jpg", "small");
  dc::pass_options o = d.options();
  o.fail_at = [](std::string_view path, std::uint64_t written) { return path == "big.mov" && written >= (1u << 20); };
  const dc::pass_result r = dc::run_pass(o);
  CHECK(r.failed == 1);
  CHECK(!r.complete());
  CHECK(r.files_left == 1);
  CHECK(!fs::exists(d.copied / "big.mov"));
  CHECK(!fs::exists(d.copied / "big.mov.mvpart"));
  CHECK(got(d.copied / "small.jpg") == "small");
  CHECK(!dc::read_marker(utf8(d.copy)));  // not whole yet
  // A leftover part from a crash is dropped, and the next pass copies the file whole.
  put(d.copied / "big.mov.mvpart", "half");
  const dc::pass_result next = dc::run_pass(d.options());
  CHECK(next.complete());
  CHECK(got(d.copied / "big.mov") == big);
  CHECK(!fs::exists(d.copied / "big.mov.mvpart"));
}

TEST_CASE("drive copier: a file still being written waits; a missing drive takes nothing to the bin", "[drive-copier]") {
  drives_pair d;
  put(d.source / "settled.jpg", "s");
  put(d.source / "writing.mov", "growing");
  const std::int64_t real_now = static_cast<std::int64_t>(std::time(nullptr));
  age(d.source / "settled.jpg", 1);
  const dc::pass_result r = dc::run_pass(d.options(real_now));
  CHECK(r.held == 1);
  CHECK(!r.complete());
  CHECK(fs::exists(d.copied / "settled.jpg"));
  CHECK(!fs::exists(d.copied / "writing.mov"));

  // The source drive gone, its folder left empty: not "everything deleted".
  REQUIRE(dc::run_pass(d.options()).complete());
  fs::remove_all(d.source);
  fs::create_directories(d.source);
  dc::pass_result gone = dc::run_pass(d.options());
  CHECK(gone.trouble == dc::pass_trouble::source_missing);
  CHECK(dc::read_bin(utf8(d.copy)).items.empty());
  CHECK(fs::exists(d.copied / "settled.jpg"));
  fs::remove_all(d.source);
  CHECK(dc::run_pass(d.options()).trouble == dc::pass_trouble::source_missing);
  fs::remove_all(d.copy);
  fs::create_directories(d.source);
  CHECK(dc::run_pass(d.options()).trouble == dc::pass_trouble::copy_missing);
}

TEST_CASE("drive copier: a pass stops between chunks when the keeper stops", "[drive-copier]") {
  drives_pair d;
  put(d.source / "a.jpg", "a");
  std::atomic<bool> stop{true};
  dc::pass_options o = d.options();
  o.stop = &stop;
  const dc::pass_result r = dc::run_pass(o);
  CHECK(r.stopped);
  CHECK(r.copied == 0);
  CHECK(!r.complete());
}

TEST_CASE("drive copier: copies.json and the words for each copy", "[drive-copier]") {
  const std::vector<cli::copy_entry> copies{{"Family Media", "Family Media Copy", "E:\\", "\\\\?\\Volume{a}\\", "F:\\", ""}};
  const auto back = cli::copies_from_json(cli::copies_json(copies));
  REQUIRE(back);
  REQUIRE(back->size() == 1);
  CHECK((*back)[0].from == "E:\\");
  CHECK((*back)[0].from_volume == "\\\\?\\Volume{a}\\");
  CHECK(!cli::copies_from_json("{\"copies\":[{\"label\":\"x\"}]}"));  // no drives: not a copies.json
  CHECK(!cli::copies_from_json("{\"copies\":"));

  cli::copy_status s;
  s.label = "Family Media";
  s.to_name = "Family Media Copy";
  s.state = cli::copy_state::copying;
  s.files_left = 1204;
  s.bytes_left = 420'000'000'000ull;
  CHECK(cli::copy_line(s, false) == "Copy on \xE2\x80\x9C" "Family Media Copy\xE2\x80\x9D: copying, 1204 files (420 GB) left");
  s.state = cli::copy_state::up_to_date;
  s.bin_items = 2;
  s.bin_bytes = 3'200'000'000ull;
  CHECK(contains(cli::copy_line(s, false), "up to date. Recycle Bin: 2 items, 3.2 GB"));
  CHECK(contains(cli::copy_line(s, true), "Trash: 2 items"));
  s.state = cli::copy_state::copy_full;
  s.needed = 1'200'000'000'000ull;
  s.free = 500'000'000'000ull;
  CHECK(contains(cli::copy_line(s, false), "the copy drive is full: needs 1.2 TB, has 500 GB free"));
  s.state = cli::copy_state::source_missing;
  CHECK(contains(cli::copy_line(s, false), "\"Family Media\" isn't connected"));
  const auto round = cli::copy_statuses_from_json(cli::copy_statuses_json(std::vector<cli::copy_status>{s}));
  REQUIRE(round.size() == 1);
  CHECK(round[0].state == cli::copy_state::source_missing);
  CHECK(round[0].bin_items == 2);
  CHECK(contains(cli::copies_status_line(round), "copy        Family Media: Copy on"));
}

TEST_CASE("drive copier: the keeper copies what copies.json names, and stops", "[drive-copier]") {
  drives_pair d;
  const fs::path host = d.dir / "host";
  fs::create_directories(host);
  put(d.source / "a.jpg", "a");
  age(d.source / "a.jpg", 1);
  cli::copy_keeper_config c;
  c.on_loopback = true;
  c.plain_folders = true;
  c.config_ms = 20;
  c.settle_seconds = 0;
  auto k = cli::make_copy_keeper(c);
  cli::keeper_context ctx;
  ctx.loopback_only = true;
  ctx.folder = utf8(host);
  k->start(ctx);
  const auto until = [](auto pred) {
    for (int i = 0; i < 500 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return pred();
  };
  CHECK(k->status_line().empty());  // nothing to copy yet
  REQUIRE(cli::write_copies(utf8(host), std::vector<cli::copy_entry>{
                                            {"Family Media", "Family Media Copy", utf8(d.source), "", utf8(d.copy), ""}}));
  CHECK(until([&] { return fs::exists(d.copied / "a.jpg"); }));
  CHECK(until([&] { return contains(k->status_line(), "up to date"); }));
  const auto st = cli::read_copy_statuses(utf8(host));
  REQUIRE(st.size() == 1);
  CHECK(st[0].state == cli::copy_state::up_to_date);
  k->stop();
}
