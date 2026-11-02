// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #612, Settings -> Storage (io/storage.h): what MediaViewer keeps on disk,
// measured; and Clear empties the thumbnails and nothing else.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "image/thumb.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "io/storage.h"

namespace fs = std::filesystem;

namespace {

struct temp_root {
  fs::path dir;
  temp_root() {
    dir = fs::temp_directory_path() / ("mv_storage_" + std::to_string(std::random_device{}()));
    for (const char* sub : {"thumbs", "clipboard", "snapshots", "addons", "open-addons", "addon-settings"}) {
      fs::create_directories(dir / sub);
    }
    mv::io::set_thumb_cache_dir_override((dir / "thumbs").string());
    mv::io::set_clipboard_dir_override((dir / "clipboard").string());
    mv::io::set_metadata_snapshot_dir_override((dir / "snapshots").string());
    mv::io::set_addons_dir_override((dir / "addons").string());
    mv::io::set_open_addons_dir_override((dir / "open-addons").string());
    mv::io::set_addon_settings_dir_override((dir / "addon-settings").string());
  }
  ~temp_root() {
    mv::io::set_thumb_cache_dir_override("");
    mv::io::set_clipboard_dir_override("");
    mv::io::set_metadata_snapshot_dir_override("");
    mv::io::set_addons_dir_override("");
    mv::io::set_open_addons_dir_override("");
    mv::io::set_addon_settings_dir_override("");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  temp_root(const temp_root&) = delete;
  temp_root& operator=(const temp_root&) = delete;
};

void write_file(const fs::path& p, std::size_t bytes) {
  fs::create_directories(p.parent_path());
  std::FILE* f = std::fopen(p.string().c_str(), "wb");
  REQUIRE(f != nullptr);
  const std::vector<std::uint8_t> data(bytes, 0x11);
  if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

const mv::io::storage_row* find(const std::vector<mv::io::storage_row>& rows, const std::string& id) {
  for (const auto& r : rows) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("tree_size and list_children", "[io][storage]") {
  temp_root t;
  write_file(t.dir / "tree" / "a.bin", 100);
  write_file(t.dir / "tree" / "sub" / "b.bin", 50);
  write_file(t.dir / "tree" / "sub" / "deeper" / "c.bin", 25);
  const mv::io::tree_usage u = mv::io::tree_size((t.dir / "tree").string());
  CHECK(u.bytes == 175);
  CHECK(u.files == 3);
  const mv::io::tree_usage file = mv::io::tree_size((t.dir / "tree" / "a.bin").string());
  CHECK(file.bytes == 100);
  CHECK(file.files == 1);
  const mv::io::tree_usage none = mv::io::tree_size((t.dir / "missing").string());
  CHECK(none.bytes == 0);
  CHECK(none.files == 0);

  auto files = mv::io::list_children((t.dir / "tree").string());
  REQUIRE(files);
  REQUIRE(files.value().size() == 1);
  CHECK(files.value()[0].name == "a.bin");
  CHECK(files.value()[0].size == 100);
  CHECK_FALSE(files.value()[0].is_directory);
  auto both = mv::io::list_children((t.dir / "tree").string(), true);
  REQUIRE(both);
  CHECK(both.value().size() == 2);
  auto missing = mv::io::list_children((t.dir / "missing").string());
  REQUIRE(missing);
  CHECK(missing.value().empty());
}

TEST_CASE("storage: every place with its size", "[io][storage]") {
  temp_root t;
  write_file(t.dir / "thumbs" / "0123456789abcdef.jpg", 1000);
  write_file(t.dir / "thumbs" / "thumbs.sqlite", 200);
  write_file(t.dir / "thumbs" / "meta_cache.db", 300);
  write_file(t.dir / "thumbs" / "meta_cache.db-wal", 30);
  write_file(t.dir / "clipboard" / "copy.png", 40);
  write_file(t.dir / "snapshots" / "x" / "snap.json", 7);
  write_file(t.dir / "addons" / "ai" / "1.0.0" / "pack.bin", 5000);
  write_file(t.dir / "addons" / "ai" / "data" / "index.db", 600);
  write_file(t.dir / "addons" / "places" / "data" / "places.mvgeo", 900);
  write_file(t.dir / "addons" / ".staging" / "part", 10);
  write_file(t.dir / "open-addons" / "com.example.theme" / "theme.json", 3);
  write_file(t.dir / "addon-settings" / "import.json", 2);
  write_file(t.dir / "crash" / "report.dmp", 77);

  const auto rows = mv::io::measure_storage({{"crash_reports", (t.dir / "crash").string()},
                                             {"telemetry", (t.dir / "nothing.jsonl").string()}});
  const auto* thumbs = find(rows, "thumbnails");
  REQUIRE(thumbs);
  CHECK(thumbs->bytes == 1200);
  CHECK(thumbs->files == 2);
  const auto* search = find(rows, "search_cache");
  REQUIRE(search);
  CHECK(search->bytes == 330);
  REQUIRE(find(rows, "clipboard"));
  CHECK(find(rows, "clipboard")->bytes == 40);
  REQUIRE(find(rows, "metadata_snapshots"));
  CHECK(find(rows, "metadata_snapshots")->bytes == 7);
  REQUIRE(find(rows, "addon:ai"));
  CHECK(find(rows, "addon:ai")->bytes == 5600);
  REQUIRE(find(rows, "addon:places"));
  CHECK(find(rows, "addon:places")->bytes == 900);
  CHECK(find(rows, "addon:.staging") == nullptr);
  REQUIRE(find(rows, "open_addon:com.example.theme"));
  REQUIRE(find(rows, "addon_settings"));
  REQUIRE(find(rows, "crash_reports"));
  CHECK(find(rows, "crash_reports")->bytes == 77);
  REQUIRE(find(rows, "telemetry"));
  CHECK(find(rows, "telemetry")->bytes == 0);

  const std::string json = mv::io::storage_json(rows);
  CHECK(json.find("\"id\":\"thumbnails\"") != std::string::npos);
  CHECK(json.find("\"bytes\":1200") != std::string::npos);
  CHECK(json.rfind("{\"rows\":[", 0) == 0);
  CHECK(json.find("\"total\":") != std::string::npos);
}

TEST_CASE("storage: JSON escapes paths", "[io][storage]") {
  const std::string json = mv::io::storage_json({{"x", "C:\\Users\\a\"b", 1, 1}});
  CHECK(json.find("C:\\\\Users\\\\a\\\"b") != std::string::npos);
}

TEST_CASE("storage: Clear and a sweep never touch metadata snapshots or the search cache",
          "[io][storage][thumb]") {
  temp_root t;
  write_file(t.dir / "snapshots" / "a" / "snap.json", 9);
  fs::last_write_time(t.dir / "snapshots" / "a" / "snap.json",
                      fs::file_time_type::clock::now() - std::chrono::hours(48));
  write_file(t.dir / "thumbs" / "meta_cache.db", 300);
  mv::image::thumb_store store;
  REQUIRE(store.open((t.dir / "thumbs").string()));
  REQUIRE(store.store(mv::image::thumb_key{"/a.jpg", 1, 2}, std::vector<std::uint8_t>{1, 2, 3}));
  std::atomic<mv::generation> gen{1};
  mv::image::thumb_sweep st;
  auto swept = store.sweep(st, 1, mv::job_context(1, mv::background_generation, &gen, 0));
  REQUIRE(swept);
  REQUIRE(store.clear());
  CHECK(fs::exists(t.dir / "snapshots" / "a" / "snap.json"));
  CHECK(fs::exists(t.dir / "thumbs" / "meta_cache.db"));
  const auto rows = mv::io::measure_storage();
  REQUIRE(find(rows, "metadata_snapshots"));
  CHECK(find(rows, "metadata_snapshots")->bytes == 9);
  REQUIRE(find(rows, "thumbnails"));
  CHECK(find(rows, "search_cache")->bytes == 300);
}
