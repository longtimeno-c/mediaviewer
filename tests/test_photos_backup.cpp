// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/photos_backup (docs/design/26 "Backup"): the engine over a fake library. The
// real one is PhotoKit (shell/photos_backup_mac.mm) and needs the user's
// permission, so what is proved here is the engine: layout, verified copies,
// the manifest making a second run free, a deleted file coming back, a cloud
// fetch, a name collision, and a cancel that leaves nothing behind.
#include "catch_compat.h"

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

TEST_CASE("a second run writes nothing; a file deleted from the backup comes back", "[shell][photos_backup]") {
  scratch_dir lib("pb_lib2");
  scratch_dir dest("pb_dest2");
  scratch_dir scratch("pb_scratch2");
  auto first = std::make_unique<fake_library>();
  first->files.push_back(make(lib, "A", bk::file_kind::original, "IMG_0001.HEIC", kDay1, 120'000, 5, true));
  first->files.push_back(make(lib, "B", bk::file_kind::original, "IMG_0002.MOV", kDay1, 400'000, 6, false));
  auto second = std::make_unique<fake_library>();
  second->files = first->files;
  auto third = std::make_unique<fake_library>();
  third->files = first->files;
  fake_library* raw2 = second.get();
  fake_library* raw3 = third.get();

  {
    bk::engine e;
    REQUIRE(e.start(std::move(first), {utf8(dest.root()), utf8(scratch / "tmp")}));
    CHECK(run_to_end(e).done == 2);
  }
  {
    bk::engine e;
    REQUIRE(e.start(std::move(second), {utf8(dest.root()), utf8(scratch / "tmp")}));
    const bk::progress p = run_to_end(e);
    CHECK(p.state == bk::run_state::done);
    CHECK(p.done == 0);
    CHECK(p.skipped == 2);
    CHECK(p.bytes == 0);
    CHECK(raw2->fetches == 0);  // nothing was fetched from the "cloud" again
  }
  const fs::path gone = fs::path(bk::layout_dir(utf8(dest.root()), kDay1)) / "IMG_0002.MOV";
  REQUIRE(fs::exists(gone));
  fs::remove(gone);
  {
    bk::engine e;
    REQUIRE(e.start(std::move(third), {utf8(dest.root()), utf8(scratch / "tmp")}));
    const bk::progress p = run_to_end(e);
    CHECK(p.done == 1);
    CHECK(p.skipped == 1);
    CHECK(raw3->fetches == 1);
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
