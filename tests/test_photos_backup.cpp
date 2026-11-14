// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/photos_backup (docs/design/26 "Backup"): the engine over a fake library. The
// real one is PhotoKit (shell/photos_backup_mac.mm) and needs the user's
// permission, so what is proved here is the engine: layout, verified copies,
// the manifest making a second run free, a deleted file held until someone chooses (and Leave deleted by hash), a cloud
// fetch, a name collision, a cancel that leaves nothing behind, and the
// folder layouts (issue #414) with a manifest that finds a file in any of them.
#include "catch_compat.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "import_fixture.h"
#include "io/content_hash.h"
#include "io/file_port.h"
#include "io/verified_copy.h"
#include "shell/photos_backup.h"

using namespace mv::test;
namespace bk = mv::shell::backup;

namespace {

struct fake_file {
  bk::asset_file meta;
  std::vector<std::uint8_t> bytes;
  bool local = true;  // on "this Mac": local_file answers a path; else fetched
  std::string path;   // where the fake library keeps it
};

class fake_library final : public bk::source {
 public:
  std::vector<fake_file> files;
  std::atomic<int> fetches{0};
  std::atomic<int> enumerations{0};
  // A fetch that waits, so a cancel can land mid-run.
  std::atomic<bool> slow_fetch{false};

  mv::result<std::vector<bk::asset_file>> enumerate(const std::atomic<bool>*) override {
    ++enumerations;
    std::vector<bk::asset_file> out;
    for (const auto& f : files) out.push_back(f.meta);
    return out;
  }
  mv::result<std::string> local_file(const bk::asset_file& file) override {
    for (const auto& f : files) {
      if (f.meta.id == file.id && f.meta.kind == file.kind) return f.local ? f.path : std::string();
    }
    return mv::err(mv::status::not_found);
  }
  mv::expected fetch(const bk::asset_file& file, const std::string& tmp, const std::atomic<bool>* cancel) override {
    ++fetches;
    for (const auto& f : files) {
      if (f.meta.id != file.id || f.meta.kind != file.kind) continue;
      if (slow_fetch) {
        for (int i = 0; i < 200 && !(cancel && cancel->load()); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (cancel && cancel->load()) return mv::err(mv::status::cancelled);
      }
      write_bytes(fs::path(tmp), f.bytes);
      return {};
    }
    return mv::err(mv::status::not_found);
  }
};

std::vector<std::uint8_t> blob(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> b(n);
  for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 8));
  return b;
}

// 2024-05-17 10:00 UTC (the layout folders are local time; the test derives
// the expected folder the same way).
constexpr std::int64_t kDay1 = 1715940000;
constexpr std::int64_t kDay2 = kDay1 + 86400 * 3;

fake_file make(const scratch_dir& lib, const char* id, bk::file_kind kind, const char* name,
               std::int64_t created, std::size_t bytes, std::uint8_t seed, bool local) {
  fake_file f;
  f.meta.id = id;
  f.meta.kind = kind;
  f.meta.filename = name;
  f.meta.created_unix = created;
  f.bytes = blob(bytes, seed);
  f.local = local;
  f.path = utf8(lib / (std::string(id) + "-" + std::to_string(static_cast<int>(kind)) + "-" + name));
  write_bytes(fs::path(f.path), f.bytes);
  return f;
}

bk::progress run_to_end(bk::engine& e) {
  e.join();
  return e.snapshot();
}

// The asset's date as the layout names it, derived here independently.
std::string local(std::int64_t unix, const char* format) {
  const time_t t = static_cast<time_t>(unix);
  struct tm tmv {};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char out[32] = {};
  std::strftime(out, sizeof(out), format, &tmv);
  return out;
}

bk::options opts_for(const fs::path& dest, const scratch_dir& scratch, bk::layout folders) {
  bk::options o{utf8(dest), utf8(scratch / "tmp")};
  o.folders = folders;
  return o;
}

// Every file under `root`, the housekeeping folder left out, as '/'-joined
// places relative to it.
std::vector<std::string> files_under(const fs::path& root) {
  std::vector<std::string> out;
  for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
    if (it->is_directory() && it->path().filename() == ".mediaviewer-photos-backup") {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file()) out.push_back(fs::relative(it->path(), root).generic_string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string hex_of(const fs::path& p) {
  auto h = mv::io::hash_file(utf8(p), mv::io::read_mode::sequential);
  REQUIRE(h);
  return h->hex();
}

}  // namespace

TEST_CASE("a first backup copies every original into YYYY/YYYY-MM-DD, verified", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib");
  scratch_dir dest("pb_dest");
  scratch_dir scratch("pb_scratch");
  auto src = std::make_unique<fake_library>();
  src->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 300'000, 1, true));
  src->files.push_back(make(lib, "A", bk::file_kind::paired_video, "IMG_0001.MOV", kDay1, 700'000, 2, false));
  src->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay2, 50'000, 3, false));
  src->files.push_back(make(lib, "B", bk::file_kind::alternate, "IMG_0002.DNG", kDay2, 900'000, 4, false));
  fake_library* raw = src.get();

  bk::engine e;
  REQUIRE(e.start(std::move(src), {utf8(dest.root()), utf8(scratch / "tmp")}));
  const bk::progress p = run_to_end(e);
  CHECK(p.state == bk::run_state::done);
  CHECK(p.total == 4);
  CHECK(p.done == 4);
  CHECK(p.skipped == 0);
  CHECK(p.failed == 0);
  CHECK(p.fetched == 3);
  CHECK(p.bytes == 300'000 + 700'000 + 50'000 + 900'000);
  CHECK(raw->fetches == 3);

  const std::string day1 = bk::layout_dir(utf8(dest.root()), kDay1);
  const std::string day2 = bk::layout_dir(utf8(dest.root()), kDay2);
  CHECK(day1 != day2);
  CHECK(day1.find(utf8(dest.root())) == 0);
  // <dest>/YYYY/YYYY-MM-DD: the day folder sits in its year folder.
  const fs::path d1(day1);
  CHECK(d1.parent_path().filename().string().size() == 4);
  CHECK(d1.filename().string().size() == 10);
  CHECK(d1.filename().string().substr(0, 4) == d1.parent_path().filename().string());

  CHECK(fs::exists(d1 / "IMG_0001.HEIC"));
  CHECK(fs::exists(d1 / "IMG_0001.MOV"));
  CHECK(fs::exists(fs::path(day2) / "IMG_0002.JPG"));
  CHECK(fs::exists(fs::path(day2) / "IMG_0002.DNG"));
  CHECK(hex_of(d1 / "IMG_0001.MOV") == hex_of(fs::path(raw->files[1].path)));
  CHECK(hex_of(fs::path(day2) / "IMG_0002.DNG") == hex_of(fs::path(raw->files[3].path)));
  // The scratch folder is gone; the manifest and the report are in the destination.
  CHECK_FALSE(fs::exists(scratch / "tmp"));
  const fs::path keep(bk::housekeeping_dir(utf8(dest.root())));
  CHECK(fs::exists(keep / "manifest.sqlite"));
  CHECK(fs::exists(keep / "last-run.txt"));
}

TEST_CASE("a second run writes nothing; a file deleted from the backup is held until someone chooses",
          "[shell][photos_backup]") {
  // Owner, 2026-11-14 (docs/plans/check-library.md §2.3, A+B): a photo removed
  // from the folder after it was backed up is not brought back by itself.
  scratch_dir lib("pb_lib2");
  scratch_dir dest("pb_dest2");
  scratch_dir scratch("pb_scratch2");
  auto source = [&] {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 5, true));
    s->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.MOV", kDay1, 400'000, 6, false));
    return s;
  };
  const auto run = [&](std::unique_ptr<fake_library> s, bool check_only = false) {
    bk::engine e;
    bk::options o{utf8(dest.root()), utf8(scratch / "tmp")};
    o.check_only = check_only;
    REQUIRE(e.start(std::move(s), std::move(o)));
    const bk::progress p = run_to_end(e);
    return std::make_pair(p, e.last_check());
  };
  CHECK(run(source()).first.done == 2);
  {
    const bk::progress p = run(source()).first;
    CHECK(p.state == bk::run_state::done);
    CHECK(p.done == 0);
    CHECK(p.skipped == 2);
    CHECK(p.bytes == 0);  // nothing was fetched from the "cloud" or written again
  }
  const fs::path gone = fs::path(bk::layout_dir(utf8(dest.root()), kDay1)) / "IMG_0002.MOV";
  REQUIRE(fs::exists(gone));
  fs::remove(gone);
  {
    const bk::progress p = run(source()).first;
    CHECK(p.done == 0);
    CHECK(p.held == 1);
    CHECK(p.skipped == 1);
    CHECK(p.bytes == 0);
    CHECK_FALSE(fs::exists(gone));
  }
  // Check library lists it with what was chosen (nothing yet).
  {
    const auto [p, rep] = run(source(), true);
    CHECK(p.state == bk::run_state::done);
    REQUIRE(rep.deleted.size() == 1);
    CHECK(rep.deleted[0].asset == "B");
    CHECK(rep.deleted[0].name == "IMG_0002.MOV");
    CHECK_FALSE(rep.deleted[0].left);
  }
  // Copy again: the next run copies it back.
  REQUIRE(bk::decide_deleted(utf8(dest.root()), {{"B", bk::file_kind::original, true}}));
  {
    const bk::progress p = run(source()).first;
    CHECK(p.done == 1);
    CHECK(p.held == 0);
    CHECK(fs::exists(gone));
  }
  // An unknown file is refused and nothing changes.
  CHECK_FALSE(bk::decide_deleted(utf8(dest.root()), {{"Z", bk::file_kind::original, false}}));
}

TEST_CASE("Leave deleted is remembered by hash, whatever asset the bytes come back as", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib2b");
  scratch_dir dest("pb_dest2b");
  scratch_dir scratch("pb_scratch2b");
  const auto run = [&](std::unique_ptr<fake_library> s, bool check_only = false) {
    bk::engine e;
    bk::options o{utf8(dest.root()), utf8(scratch / "tmp")};
    o.check_only = check_only;
    REQUIRE(e.start(std::move(s), std::move(o)));
    const bk::progress p = run_to_end(e);
    return std::make_pair(p, e.last_check());
  };
  auto first = std::make_unique<fake_library>();
  first->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 7, true));
  first->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay1, 90'000, 8, true));
  REQUIRE(run(std::move(first)).first.done == 2);
  const fs::path gone = fs::path(bk::layout_dir(utf8(dest.root()), kDay1)) / "IMG_0002.JPG";
  fs::remove(gone);
  REQUIRE(bk::decide_deleted(utf8(dest.root()), {{"B", bk::file_kind::original, false}}));

  // The same asset: left deleted, not held, not copied.
  {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 7, true));
    s->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay1, 90'000, 8, true));
    const bk::progress p = run(std::move(s)).first;
    CHECK(p.done == 0);
    CHECK(p.held == 0);
    CHECK(p.left_deleted == 1);
  }
  // The photo comes back into Photos as a new asset (re-downloaded, re-imported):
  // its bytes are still left deleted.
  {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 7, true));
    s->files.push_back(make(lib, "B2", bk::file_kind::original, "IMG_0002 copy.JPG", kDay2, 90'000, 8, false));
    const bk::progress p = run(std::move(s)).first;
    CHECK(p.done == 0);
    CHECK(p.left_deleted == 1);  // B2's bytes: fetched to learn them, then not written
    CHECK(p.bytes == 0);
    CHECK(files_under(dest.root()).size() == 1);
  }
  // Recorded for B2 too: the next run does not fetch it again.
  {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "B2", bk::file_kind::original, "IMG_0002 copy.JPG", kDay2, 90'000, 8, false));
    fake_library* raw = s.get();
    bk::engine e;
    REQUIRE(e.start(std::move(s), {utf8(dest.root()), utf8(scratch / "tmp")}));
    const bk::progress p = run_to_end(e);
    CHECK(p.left_deleted == 1);
    CHECK(p.done == 0);
    CHECK(raw->fetches == 0);
  }
  // Check library shows the choice, and Copy again undoes it.
  {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay1, 90'000, 8, true));
    const auto [p, rep] = run(std::move(s), true);
    REQUIRE(rep.deleted.size() == 1);
    CHECK(rep.deleted[0].left);
  }
  REQUIRE(bk::decide_deleted(utf8(dest.root()), {{"B", bk::file_kind::original, true}}));
  {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay1, 90'000, 8, true));
    const bk::progress p = run(std::move(s)).first;
    CHECK(p.done == 1);
    CHECK(fs::exists(gone));
  }
}

TEST_CASE("two assets with one name on one day never overwrite each other", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib3");
  scratch_dir dest("pb_dest3");
  scratch_dir scratch("pb_scratch3");
  auto src = std::make_unique<fake_library>();
  src->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.JPG", kDay1, 10'000, 7, true));
  src->files.push_back(make(lib, "Z", bk::file_kind::original, "IMG_0001.JPG", kDay1 + 60, 20'000, 8, true));
  bk::engine e;
  REQUIRE(e.start(std::move(src), {utf8(dest.root()), utf8(scratch / "tmp")}));
  const bk::progress p = run_to_end(e);
  CHECK(p.done == 2);
  const fs::path day(bk::layout_dir(utf8(dest.root()), kDay1));
  CHECK(fs::exists(day / "IMG_0001.JPG"));
  CHECK(fs::exists(day / "IMG_0001 (2).JPG"));
  CHECK(fs::file_size(day / "IMG_0001.JPG") == 10'000);
  CHECK(fs::file_size(day / "IMG_0001 (2).JPG") == 20'000);
}

TEST_CASE("cancel stops the run and leaves no part file or scratch behind", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib4");
  scratch_dir dest("pb_dest4");
  scratch_dir scratch("pb_scratch4");
  auto src = std::make_unique<fake_library>();
  src->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.JPG", kDay1, 10'000, 9, true));
  src->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.MOV", kDay1, 300'000, 10, false));
  src->files.push_back(make(lib, "C", bk::file_kind::original, "IMG_0003.JPG", kDay1, 10'000, 11, true));
  src->slow_fetch = true;
  bk::engine e;
  REQUIRE(e.start(std::move(src), {utf8(dest.root()), utf8(scratch / "tmp")}));
  // Let the first file land and the slow fetch begin, then cancel.
  for (int i = 0; i < 400 && e.snapshot().done < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  e.cancel();
  const bk::progress p = run_to_end(e);
  CHECK(p.state == bk::run_state::cancelled);
  CHECK(p.done == 1);
  const fs::path day(bk::layout_dir(utf8(dest.root()), kDay1));
  CHECK(fs::exists(day / "IMG_0001.JPG"));
  CHECK_FALSE(fs::exists(day / "IMG_0002.MOV"));
  CHECK_FALSE(fs::exists(day / "IMG_0003.JPG"));
  CHECK_FALSE(fs::exists(scratch / "tmp"));
  for (const auto& entry : fs::recursive_directory_iterator(dest.root())) {
    CHECK(entry.path().extension() != ".mvtmp");
    CHECK(entry.path().extension() != ".part");
  }
}

TEST_CASE("a backup moved to a new mount point is still found; nothing is copied twice", "[shell][photos_backup]") {
  // A NAS share remounts as /Volumes/photos-1 when a stale /Volumes/photos is
  // in the way: the manifest keeps each file's place under the destination,
  // not its absolute path, so the next run sees the same backup.
  scratch_dir lib("pb_lib5");
  scratch_dir dest("pb_dest5");
  scratch_dir scratch("pb_scratch5");
  auto first = std::make_unique<fake_library>();
  first->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 12, true));
  first->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.MOV", kDay2, 200'000, 13, false));
  auto second = std::make_unique<fake_library>();
  second->files = first->files;
  fake_library* raw2 = second.get();

  const fs::path old_root = dest / "photos";
  const fs::path new_root = dest / "photos-1";
  {
    bk::engine e;
    REQUIRE(e.start(std::move(first), {utf8(old_root), utf8(scratch / "tmp")}));
    CHECK(run_to_end(e).done == 2);
  }
  fs::rename(old_root, new_root);
  {
    bk::engine e;
    REQUIRE(e.start(std::move(second), {utf8(new_root), utf8(scratch / "tmp")}));
    const bk::progress p = run_to_end(e);
    CHECK(p.state == bk::run_state::done);
    CHECK(p.skipped == 2);
    CHECK(p.done == 0);
    CHECK(raw2->fetches == 0);
  }
  CHECK_FALSE(fs::exists(fs::path(bk::layout_dir(utf8(new_root), kDay1)) / "IMG_0001 (2).HEIC"));
}

namespace {

// A library whose fetch pulls the destination out from under the run, as a
// NAS dropping off the network does.
class vanishing_destination final : public bk::source {
 public:
  fake_library inner;
  fs::path destination;
  std::atomic<int> fetches{0};
  std::atomic<bool> removed{false};

  mv::result<std::vector<bk::asset_file>> enumerate(const std::atomic<bool>* c) override { return inner.enumerate(c); }
  mv::result<std::string> local_file(const bk::asset_file& f) override { return inner.local_file(f); }
  mv::expected fetch(const bk::asset_file& f, const std::string& tmp, const std::atomic<bool>* c) override {
    if (++fetches == 1) {
      std::error_code ec;
      fs::remove_all(destination, ec);
      removed = !fs::exists(destination);
    }
    return inner.fetch(f, tmp, c);
  }
};

}  // namespace

TEST_CASE("a destination that disappears stops the run instead of failing every file", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib6");
  scratch_dir dest("pb_dest6");
  scratch_dir scratch("pb_scratch6");
  auto src = std::make_unique<vanishing_destination>();
  src->destination = dest / "nas";
  src->inner.files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.JPG", kDay1, 10'000, 14, false));
  src->inner.files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.JPG", kDay1, 10'000, 15, false));
  src->inner.files.push_back(make(lib, "C", bk::file_kind::original, "IMG_0003.JPG", kDay2, 10'000, 16, false));
  vanishing_destination* raw = src.get();
  bk::engine e;
  REQUIRE(e.start(std::move(src), {utf8(dest / "nas"), utf8(scratch / "tmp")}));
  const bk::progress p = run_to_end(e);
  // Windows will not delete a folder holding an open file, and the run keeps
  // its manifest open in the destination, so the folder cannot be pulled away
  // here; a share that drops off the network is a different event.
  if (!raw->removed) SKIP("the destination could not be removed while the manifest is open (Windows)");
  CHECK(p.state == bk::run_state::failed);
  CHECK(p.failed == 1);
  CHECK(p.done == 0);
  CHECK_FALSE(p.error.empty());
  CHECK(raw->fetches == 1);  // nothing more was downloaded for a folder that is gone
}

TEST_CASE("each layout writes its own folders", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib7");
  scratch_dir dest("pb_dest7");
  scratch_dir scratch("pb_scratch7");
  const std::string y = local(kDay1, "%Y");
  const std::string ymd = local(kDay1, "%Y-%m-%d");
  struct expectation {
    bk::layout folders;
    std::string place;
  };
  const expectation cases[] = {
      {bk::layout::year, y + "/IMG_0001.HEIC"},
      {bk::layout::year_day, y + "/" + ymd + "/IMG_0001.HEIC"},
      {bk::layout::year_month, y + "/" + local(kDay1, "%m") + "/IMG_0001.HEIC"},
      {bk::layout::year_month_day, y + "/" + local(kDay1, "%m") + "/" + local(kDay1, "%d") + "/IMG_0001.HEIC"},
      {bk::layout::day, ymd + "/IMG_0001.HEIC"},
      {bk::layout::flat, "IMG_0001.HEIC"},
  };
  int n = 0;
  for (const expectation& c : cases) {
    const fs::path root = dest / ("d" + std::to_string(n++));
    auto src = std::make_unique<fake_library>();
    src->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 20'000, 17, true));
    bk::engine e;
    REQUIRE(e.start(std::move(src), opts_for(root, scratch, c.folders)));
    CHECK(run_to_end(e).done == 1);
    // Only that file, in only that folder: no day folder under a year-only
    // backup, nothing but the file in a flat one.
    CHECK(files_under(root) == std::vector<std::string>{c.place});
    CHECK(fs::path(bk::layout_dir(utf8(root), kDay1, c.folders)) / "IMG_0001.HEIC" == root / fs::path(c.place));
  }
  CHECK(bk::layout_subdir(kDay1, bk::layout::flat).empty());
  CHECK(bk::layout_dir(utf8(dest.root()), kDay1, bk::layout::flat) == utf8(dest.root()));
}

TEST_CASE("two originals with one name in one year are name and name (2) under that year",
          "[shell][photos_backup]") {
  scratch_dir lib("pb_lib8");
  scratch_dir dest("pb_dest8");
  scratch_dir scratch("pb_scratch8");
  REQUIRE(local(kDay1, "%Y") == local(kDay2, "%Y"));
  auto src = std::make_unique<fake_library>();
  src->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.JPG", kDay1, 10'000, 18, true));
  src->files.push_back(make(lib, "Z", bk::file_kind::original, "IMG_0001.JPG", kDay2, 20'000, 19, false));
  bk::engine e;
  REQUIRE(e.start(std::move(src), opts_for(dest.root(), scratch, bk::layout::year)));
  CHECK(run_to_end(e).done == 2);
  const fs::path year = dest / local(kDay1, "%Y");
  CHECK(fs::file_size(year / "IMG_0001.JPG") == 10'000);
  CHECK(fs::file_size(year / "IMG_0001 (2).JPG") == 20'000);
  CHECK(files_under(dest.root()).size() == 2);
}

TEST_CASE("a layout change moves nothing; only a new file is written in the new layout",
          "[shell][photos_backup]") {
  scratch_dir lib("pb_lib9");
  scratch_dir dest("pb_dest9");
  scratch_dir scratch("pb_scratch9");
  const fake_file a = make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 30'000, 20, false);
  const fake_file b = make(lib, "B", bk::file_kind::original, "IMG_0002.HEIC", kDay2, 40'000, 21, false);
  const fs::path old_root = dest / "photos";
  const fs::path new_root = dest / "photos-1";
  {
    auto src = std::make_unique<fake_library>();
    src->files.push_back(a);
    bk::engine e;
    REQUIRE(e.start(std::move(src), opts_for(old_root, scratch, bk::layout::year_day)));
    CHECK(run_to_end(e).done == 1);
  }
  const std::string kept = local(kDay1, "%Y") + "/" + local(kDay1, "%Y-%m-%d") + "/IMG_0001.HEIC";
  // The share remounts elsewhere too: only the place under the destination matches.
  fs::rename(old_root, new_root);
  auto src = std::make_unique<fake_library>();
  src->files = {a, b};
  fake_library* raw = src.get();
  bk::engine e;
  REQUIRE(e.start(std::move(src), opts_for(new_root, scratch, bk::layout::year)));
  const bk::progress p = run_to_end(e);
  CHECK(p.state == bk::run_state::done);
  CHECK(p.skipped == 1);
  CHECK(p.done == 1);
  CHECK(raw->fetches == 1);  // only the new one
  CHECK(files_under(new_root) == std::vector<std::string>{kept, local(kDay2, "%Y") + "/IMG_0002.HEIC"});

  // And a third run in the year layout finds both where they are.
  auto again = std::make_unique<fake_library>();
  again->files = {a, b};
  bk::engine e2;
  REQUIRE(e2.start(std::move(again), opts_for(new_root, scratch, bk::layout::year)));
  const bk::progress p2 = run_to_end(e2);
  CHECK(p2.skipped == 2);
  CHECK(p2.done == 0);
}

TEST_CASE("manifest rows from before 2026-10-05 (absolute paths) still match after a remount",
          "[shell][photos_backup]") {
  scratch_dir lib("pb_lib10");
  scratch_dir dest("pb_dest10");
  scratch_dir scratch("pb_scratch10");
  const fake_file a = make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 30'000, 22, false);
  const fs::path old_root = dest / "photos";
  const fs::path new_root = dest / "photos-1";
  {
    auto src = std::make_unique<fake_library>();
    src->files.push_back(a);
    bk::engine e;
    REQUIRE(e.start(std::move(src), opts_for(old_root, scratch, bk::layout::year_day)));
    CHECK(run_to_end(e).done == 1);
  }
  // Rewrite the row as the old engine stored it: the absolute path.
  const std::string absolute = bk::layout_dir(utf8(old_root), kDay1) + "/IMG_0001.HEIC";
  {
    sqlite3* db = nullptr;
    const std::string manifest = utf8(fs::path(bk::housekeeping_dir(utf8(old_root))) / "manifest.sqlite");
    REQUIRE(sqlite3_open(manifest.c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "UPDATE files SET path=?", -1, &st, nullptr) == SQLITE_OK);
    sqlite3_bind_text(st, 1, absolute.c_str(), -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(db);
  }
  fs::rename(old_root, new_root);
  REQUIRE_FALSE(fs::exists(absolute));
  auto src = std::make_unique<fake_library>();
  src->files.push_back(a);
  fake_library* raw = src.get();
  bk::engine e;
  REQUIRE(e.start(std::move(src), opts_for(new_root, scratch, bk::layout::flat)));
  const bk::progress p = run_to_end(e);
  CHECK(p.skipped == 1);
  CHECK(p.done == 0);
  CHECK(raw->fetches == 0);
  CHECK(files_under(new_root).size() == 1);
}

#if !defined(_WIN32)
TEST_CASE("a file whose stat is refused is never held as deleted", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib_perm");
  scratch_dir dest("pb_dest_perm");
  scratch_dir scratch("pb_scratch_perm");
  const auto source = [&] {
    auto s = std::make_unique<fake_library>();
    s->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 50'000, 3, true));
    return s;
  };
  {
    bk::engine e;
    REQUIRE(e.start(source(), {utf8(dest.root()), utf8(scratch / "tmp")}));
    REQUIRE(run_to_end(e).done == 1);
  }
  const fs::path day(bk::layout_dir(utf8(dest.root()), kDay1));
  fs::permissions(day, fs::perms::none);  // the folder answers nothing about its files
  bk::progress p;
  {
    bk::engine e;
    REQUIRE(e.start(source(), {utf8(dest.root()), utf8(scratch / "tmp")}));
    p = run_to_end(e);
  }
  fs::permissions(day, fs::perms::owner_all);
  CHECK(p.held == 0);
  CHECK(p.skipped == 1);
  CHECK(p.done == 0);
}
#endif
