// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/folder_model_mac (docs/design/26): a result list may hold VIRTUAL items (a
// Photos library asset, "photos:<id>") with no file: listed as given, their
// tiles from the virtual thumb provider under the same cache key as a file's.
// The real provider is PhotoKit (shell/photos_items_mac.mm); here it is a
// counter that hands back a JPEG.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/job_system.h"
#include "image/thumb.h"
#include "import_fixture.h"
#include "io/paths.h"
#include "shell/folder_model_mac.h"

using namespace mv::test;

namespace {

std::vector<std::uint8_t> red_jpeg() {
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 0);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = 220;
    rgba[i + 3] = 255;
  }
  auto jpeg = mv::image::encode_thumb_rgba(rgba, 16, 16);
  REQUIRE(jpeg);
  return std::move(jpeg).value();
}

std::string wait_thumb(mv::shell::folder_model& folder, const std::string& key, std::int64_t mtime, std::uint64_t size) {
  std::atomic<bool> done{false};
  std::string got;
  std::mutex m;
  folder.request_thumb(key, mtime, size, [&](std::string, std::string thumb) {
    std::lock_guard lock(m);
    got = std::move(thumb);
    done.store(true);
  });
  for (int i = 0; i < 1000 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::lock_guard lock(m);
  return got;
}

}  // namespace

TEST_CASE("a list of virtual items lists as given and tiles through the provider", "[shell][folder][photos]") {
  scratch_dir d("folder_virtual");
  fs::create_directories(d / "thumbs");
  mv::io::set_thumb_cache_dir_override(utf8(d / "thumbs"));
  mv::job_system jobs;
  REQUIRE(jobs.start(2) == mv::status::ok);
  std::atomic<int> asked{0};
  const std::vector<std::uint8_t> jpeg = red_jpeg();
  mv::shell::folder_model folder;
  folder.set_virtual_items("photos:", [&](const std::string& key) -> mv::result<std::vector<std::uint8_t>> {
    ++asked;
    if (key == "photos:GONE") return mv::err(mv::status::not_found);
    return jpeg;
  });
  CHECK(folder.is_virtual_path("photos:ABC/L0/001"));
  CHECK_FALSE(folder.is_virtual_path("/Users/a/IMG_1.JPG"));
  CHECK_FALSE(folder.is_virtual_path("photos:"));

  // A real file beside the virtual ones: both kinds in one list.
  const fs::path real = d / "IMG_9.JPG";
  write_bytes(real, jpeg);
  std::vector<mv::shell::folder_model::list_entry> entries;
  mv::shell::folder_model::list_entry a;
  a.path_utf8 = "photos:ABC/L0/001";
  a.is_virtual = true;
  a.name_utf8 = "IMG_0412.HEIC";
  a.mtime_unix = 1'700'000'000;
  a.size = 12'000'000;
  entries.push_back(a);
  mv::shell::folder_model::list_entry v;
  v.path_utf8 = "photos:DEF/L0/002";
  v.is_virtual = true;
  v.name_utf8 = "IMG_0413.MOV";
  v.mtime_unix = 1'700'000'500;
  v.size = 8'000'000;
  entries.push_back(v);
  mv::shell::folder_model::list_entry nameless;  // dropped: nothing to show
  nameless.path_utf8 = "photos:NONAME";
  nameless.is_virtual = true;
  entries.push_back(nameless);
  mv::shell::folder_model::list_entry file;
  file.path_utf8 = utf8(real);
  entries.push_back(file);
  mv::shell::folder_model::list_entry missing;  // a file that is gone drops out, as before
  missing.path_utf8 = utf8(d / "gone.JPG");
  entries.push_back(missing);

  REQUIRE(folder.open_list("Photos Library", entries, jobs));
  const auto listing = folder.snapshot();
  CHECK(listing.is_list);
  CHECK(listing.title == "Photos Library");
  REQUIRE(listing.items.size() == 3);
  CHECK(listing.items[0].path_utf8 == "photos:ABC/L0/001");
  CHECK(listing.items[0].name_utf8 == "IMG_0412.HEIC");
  CHECK(listing.items[0].mtime_unix == 1'700'000'000);
  CHECK(listing.items[0].size == 12'000'000);
  CHECK(listing.items[1].name_utf8 == "IMG_0413.MOV");
  CHECK(listing.items[2].path_utf8 == utf8(real));
  CHECK(listing.items[2].name_utf8 == "IMG_9.JPG");

  // A virtual tile: the provider once, then the cache (same key as a file's).
  const std::string t1 = wait_thumb(folder, "photos:ABC/L0/001", 1'700'000'000, 12'000'000);
  CHECK_FALSE(t1.empty());
  CHECK(fs::exists(fs::path(t1)));
  CHECK(asked == 1);
  const std::string t2 = wait_thumb(folder, "photos:ABC/L0/001", 1'700'000'000, 12'000'000);
  CHECK(t2 == t1);
  CHECK(asked == 1);
  // A clip key never goes to the poster decoder (no file): the provider answers.
  const std::string t3 = wait_thumb(folder, "photos:DEF/L0/002", 1'700'000'500, 8'000'000);
  CHECK_FALSE(t3.empty());
  CHECK(asked == 2);
  // An asset that is gone: no tile, no crash.
  CHECK(wait_thumb(folder, "photos:GONE", 1, 1).empty());
  // An edit in Photos (a new stamp) is a new row.
  const std::string t4 = wait_thumb(folder, "photos:ABC/L0/001", 1'700'000'900, 12'000'000);
  CHECK_FALSE(t4.empty());
  CHECK(t4 != t1);

  folder.close();
  jobs.shutdown();
  mv::io::set_thumb_cache_dir_override("");
}
