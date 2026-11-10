// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md M5 / M6 local half: src/mobile against a generation the publisher really
// builds. No model is loaded here: tools/ios/run-parity.sh proves the engine with the real tower.
#include "catch_compat.h"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "addons/ai/catalog.h"
#include "addons/ai/index_db.h"
#include "addons/ai/tiers.h"
#include "core/json.h"
#include "infer/models.h"
#include "mobile/catalogue.h"
#include "mobile/mv_mobile_search.h"
#include "mobile/search.h"
#include "mobile/testing.h"
#include "mobile_generation_fixture.h"

namespace fs = std::filesystem;
namespace cat = mv::ai::catalog;

namespace {

struct generation_fixture {
  fs::path root;
  fs::path nas;
  fs::path gen;
  std::string uuid = "aaaaaaaa-0000-4000-8000-000000000001";

  // `extra_rel`: one more photo, at this path under the root (a hostile path, say).
  explicit generation_fixture(const std::string& name, const std::string& spec = "clip-vit-l14/fp16/pre1",
                              const std::string& extra_rel = "", int variant = 0) {
    root = fs::temp_directory_path() / ("mv-mobile-test-" + name + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::vector<char> g(8192), l(8192);
    REQUIRE(mv_mobile_test_make_generation(root.string().c_str(), variant, 1, 3, spec.c_str(), extra_rel.c_str(), g.data(),
                                           static_cast<uint32_t>(g.size()), l.data(), static_cast<uint32_t>(l.size())) == 0);
    gen = g.data();
    nas = fs::path(l.data()).parent_path();
  }
  ~generation_fixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  mv_mobile_library* open(mv_mobile_status* status = nullptr) {
    const std::string g = gen.string(), l = (nas / ".mediaviewer").string(), w = (root / "work" / "000003").string();
    const mv_mobile_open_options o{sizeof o, g.c_str(), l.c_str(), w.c_str()};
    mv_mobile_library* out = nullptr;
    const mv_mobile_status st = mv_mobile_library_open(&o, &out);
    if (status) *status = st;
    return out;
  }
};

std::string row_name(const mv_mobile_row& r) { return std::string(r.file_name, r.file_name_len); }

struct folder_row {
  std::string key, name;
  uint32_t items;
  uint32_t subs;
};
void add_folder(void* user, const mv_mobile_folder* f) {
  static_cast<std::vector<folder_row>*>(user)->push_back({f->key, f->name, f->item_count, f->has_subfolders});
}

}  // namespace

TEST_CASE("a generation opens as the phone's grid: newest first, folders, tiles", "[mobile]") {
  generation_fixture g("grid");
  mv_mobile_status st = MV_MOBILE_ERR_INTERNAL;
  mv_mobile_library* lib = g.open(&st);
  REQUIRE(st == MV_MOBILE_OK);
  REQUIRE(lib);

  mv_mobile_library_info info{};
  REQUIRE(mv_mobile_library_info_get(lib, &info) == MV_MOBILE_OK);
  CHECK(info.generation == 3);
  CHECK(info.assets == 3);  // the document is not a phone tile
  CHECK(info.has_tiny == 1);
  CHECK(std::string(info.active_spec) == "clip-vit-l14/fp16/pre1");

  mv_mobile_list* list = nullptr;
  REQUIRE(mv_mobile_list_open(lib, MV_MOBILE_SCOPE_EVERYTHING, nullptr, &list) == MV_MOBILE_OK);
  REQUIRE(mv_mobile_list_count(list) == 3);
  mv_mobile_row r{};
  REQUIRE(mv_mobile_list_row_at(list, 0, &r) == MV_MOBILE_OK);
  CHECK(row_name(r) == "b.jpg");
  CHECK(r.kind == MV_MOBILE_KIND_PHOTO);
  mv_mobile_list_row_at(list, 1, &r);
  CHECK(row_name(r) == "c.mov");
  CHECK(r.kind == MV_MOBILE_KIND_CLIP);
  CHECK(r.duration_ms == 9000);
  mv_mobile_list_row_at(list, 2, &r);
  CHECK(row_name(r) == "a.jpg");
  CHECK(r.wall_seconds == 1000000);
  CHECK(mv_mobile_list_month_count(list) == 1);
  mv_mobile_list_close(list);

  CHECK(r.tile_width == 0);
  mv_mobile_library_fill_aspects(lib);
  mv_mobile_library_row(lib, 1, &r);
  CHECK(r.tile_width == 48);
  CHECK(r.tile_height == 32);

  const uint8_t* bytes = nullptr;
  uint32_t length = 0;
  REQUIRE(mv_mobile_library_tile(lib, 2, &bytes, &length) == MV_MOBILE_OK);
  REQUIRE(length > 100);
  CHECK(bytes[0] == 0xFF);
  CHECK(bytes[1] == 0xD8);
  CHECK(mv_mobile_library_tile(lib, 99, &bytes, &length) == MV_MOBILE_ERR_NOT_FOUND);

  std::vector<folder_row> top, under;
  REQUIRE(mv_mobile_library_folders(lib, nullptr, &add_folder, &top) == MV_MOBILE_OK);
  REQUIRE(top.size() == 1);
  CHECK(top[0].name == "Photos");
  CHECK(top[0].items == 3);
  CHECK(top[0].subs == 1);
  REQUIRE(mv_mobile_library_folders(lib, top[0].key.c_str(), &add_folder, &under) == MV_MOBILE_OK);
  REQUIRE(under.size() == 1);
  CHECK(under[0].name == "trip");
  CHECK(under[0].items == 2);  // a folder counts what is beneath it

  mv_mobile_list* in_trip = nullptr;
  REQUIRE(mv_mobile_list_open(lib, MV_MOBILE_SCOPE_FOLDER, under[0].key.c_str(), &in_trip) == MV_MOBILE_OK);
  CHECK(mv_mobile_list_count(in_trip) == 2);
  mv_mobile_list_close(in_trip);

  mv_mobile_details d{};
  REQUIRE(mv_mobile_library_details(lib, 1, &d) == MV_MOBILE_OK);
  CHECK(std::string(d.place) == "Ortisei, South Tyrol, Italy");
  CHECK(std::string(d.camera) == "Cam A");
  CHECK(std::string(d.folder) == "Photos");
  CHECK(std::string(d.format) == "JPG");
  CHECK(d.file_bytes == 11);

  std::vector<std::string> places, cameras;
  std::vector<int32_t> years;
  REQUIRE(mv_mobile_library_filter_options(
              lib, [](void* u, const char* v) { static_cast<std::vector<std::string>*>(static_cast<void**>(u)[0])->push_back(v); },
              [](void* u, const char* v) { static_cast<std::vector<std::string>*>(static_cast<void**>(u)[1])->push_back(v); },
              [](void* u, int32_t y) { static_cast<std::vector<int32_t>*>(static_cast<void**>(u)[2])->push_back(y); },
              std::array<void*, 3>{&places, &cameras, &years}.data()) == MV_MOBILE_OK);
  CHECK(places == std::vector<std::string>{"Ortisei"});
  CHECK(cameras == std::vector<std::string>{"Cam A", "Cam B"});
  CHECK(years == std::vector<int32_t>{1970});
  mv_mobile_library_close(lib);
}

TEST_CASE("a generation's text tower is laid out as pack.cpp reads it", "[mobile]") {
  generation_fixture g("layout");
  mv_mobile_library* lib = g.open();
  REQUIRE(lib);
  auto opened = mv::mobile::catalogue::open(g.gen.string(), (g.nas / ".mediaviewer").string());
  REQUIRE(opened);
  const fs::path work = g.root / "work2";
  REQUIRE(mv::mobile::lay_out_work_dir(**opened, work.string()));
  CHECK(fs::file_size(work / "index.db") == fs::file_size(g.gen / "catalog.db"));
  CHECK(fs::exists(work / "settings.json"));
  CHECK(fs::exists(work / "core" / "models" / "tower" / "text.onnx"));
  CHECK(fs::exists(work / "core" / "models" / "clip-tokenizer" / "merges.txt"));
  const std::string core = (work / "core").string();
  const auto profile = mv::ai::read_tier_profile(core, "default");
  CHECK(profile.picture_high == "models/tower");
  const auto spec = mv::infer::read_clip_spec(core + "/models/tower", core + "/models/tower");
  REQUIRE(spec);
  CHECK(spec->spec_key() == "clip-vit-l14/fp16/pre1");
  CHECK(spec->text_file == core + "/models/tower/text.onnx");
  CHECK(spec->vocab_file == core + "/models/clip-tokenizer/vocab.json");
  mv_mobile_library_close(lib);
}

TEST_CASE("a truncated catalogue is refused", "[mobile]") {
  generation_fixture g("truncated");
  fs::resize_file(g.gen / "catalog.db", fs::file_size(g.gen / "catalog.db") - 1);
  mv_mobile_status st = MV_MOBILE_OK;
  CHECK(g.open(&st) == nullptr);
  CHECK(st == MV_MOBILE_ERR_CORRUPT);
}

TEST_CASE("a library indexed with ViT-H/14 is refused with the plan's sentence", "[mobile]") {
  generation_fixture g("h14", "openclip-vit-h14-laion2b/fp16/pre1");
  mv_mobile_status st = MV_MOBILE_ERR_INTERNAL;
  mv_mobile_library* lib = g.open(&st);
  REQUIRE(st == MV_MOBILE_OK);  // browsing the library still works
  CHECK(mv_mobile_search_start(lib) == MV_MOBILE_ERR_UNSUPPORTED_TOWER);
  CHECK(mv_mobile_search_state(lib) == MV_MOBILE_SEARCH_UNSUPPORTED);
  CHECK(std::string(mv_mobile_status_message(MV_MOBILE_ERR_UNSUPPORTED_TOWER)) ==
        "This library uses the largest search model, which this phone can't run yet.");
  mv_mobile_results* r = nullptr;
  CHECK(mv_mobile_search_run(lib, "mountains", 0, 1000, &r) == MV_MOBILE_ERR_UNSUPPORTED_TOWER);
  mv_mobile_library_close(lib);
  CHECK(mv::mobile::tower_supported("clip-vit-b32/fp16/pre1"));
  CHECK_FALSE(mv::mobile::tower_supported("openclip-vit-h14-laion2b/fp16/pre1"));
}

TEST_CASE("a new generation opens beside the old one and the old one's rows outlive it", "[mobile]") {
  generation_fixture a("swap-a"), b("swap-b");
  mv_mobile_library* first = a.open();
  mv_mobile_library* second = b.open();
  REQUIRE(first);
  REQUIRE(second);
  mv_mobile_list* list = nullptr;
  REQUIRE(mv_mobile_list_open(first, MV_MOBILE_SCOPE_EVERYTHING, nullptr, &list) == MV_MOBILE_OK);
  mv_mobile_library_close(first);
  mv_mobile_row r{};
  mv_mobile_list_row_at(list, 0, &r);
  CHECK(row_name(r) == "b.jpg");
  mv_mobile_list_close(list);
  mv_mobile_library_close(second);
}

TEST_CASE("a catalogue path too deep or too long is refused, never walked on the stack", "[mobile]") {
  std::string deep;
  for (int i = 0; i < 3000; ++i) deep += "a/";  // 6 KB, 3000 folders: the review's crash
  generation_fixture g("deep", "clip-vit-l14/fp16/pre1", deep + "x.jpg");
  mv_mobile_status st = MV_MOBILE_OK;
  CHECK(g.open(&st) == nullptr);
  CHECK(st == MV_MOBILE_ERR_CORRUPT);

  // Opened on a thread with a small stack, as the review's was: refused, not crashed.
  std::thread small([&] { CHECK(g.open(&st) == nullptr); });
  small.join();

  std::string fine;
  for (int i = 0; i < 100; ++i) fine += "f" + std::to_string(i) + "/";
  generation_fixture ok("deep-ok", "clip-vit-l14/fp16/pre1", fine + "y.jpg");
  mv_mobile_library* lib = ok.open(&st);
  REQUIRE(st == MV_MOBILE_OK);
  mv_mobile_library_info info{};
  REQUIRE(mv_mobile_library_info_get(lib, &info) == MV_MOBILE_OK);
  CHECK(info.assets == 4);
  mv_mobile_library_close(lib);
}

TEST_CASE("names, dates and files answer without a model; meaning waits for one", "[mobile]") {
  // The fixture's text.onnx is no model, and no runtime is laid out here: the tower never loads.
  generation_fixture g("no-model");
  mv_mobile_library* lib = g.open();
  REQUIRE(lib);
  REQUIRE(mv_mobile_search_start(lib) == MV_MOBILE_OK);
  for (int i = 0; i < 500 && mv_mobile_search_state(lib) == MV_MOBILE_SEARCH_WARMING; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(mv_mobile_search_state(lib) == MV_MOBILE_SEARCH_FAILED);

  const auto paths = [&](const char* q, mv_mobile_status* st) {
    std::vector<std::string> out;
    mv_mobile_results* r = nullptr;
    *st = mv_mobile_search_run(lib, q, 0, 10000, &r);
    if (*st != MV_MOBILE_OK) return out;
    for (uint32_t i = 0; i < mv_mobile_results_count(r); ++i) out.push_back(mv_mobile_results_path(r, i));
    mv_mobile_results_close(r);
    return out;
  };
  mv_mobile_status st = MV_MOBILE_ERR_INTERNAL;
  CHECK(paths("in:1970", &st).size() == 4);  // the document too: kinds 0 is every kind
  CHECK(st == MV_MOBILE_OK);
  const auto by_file = paths("file:c", &st);
  CHECK(st == MV_MOBILE_OK);
  REQUIRE(by_file.size() == 1);
  CHECK(by_file[0] == "/" + g.uuid + "/trip/day1/c.mov");
  CHECK(paths("place:Ortisei", &st).size() == 2);
  CHECK(st == MV_MOBILE_OK);
  // A name: faces.db is read before the tower, so people answer without it.
  for (const char* q : {"@Anna", "person:Anna"}) {
    const auto by_name = paths(q, &st);
    CHECK(st == MV_MOBILE_OK);
    REQUIRE(by_name.size() == 1);
    CHECK(by_name[0] == "/" + g.uuid + "/a.jpg");
  }
  // A picture's meaning needs the tower: an error, never a silent "nothing found".
  CHECK(paths("mountains", &st).empty());
  CHECK(st == MV_MOBILE_ERR_IO);

  mv_mobile_row row{};
  CHECK(mv_mobile_library_row(lib, 1, &row) == MV_MOBILE_OK);
  mv_mobile_list* list = nullptr;
  REQUIRE(mv_mobile_list_open(lib, MV_MOBILE_SCOPE_EVERYTHING, nullptr, &list) == MV_MOBILE_OK);
  CHECK(mv_mobile_list_row_at(list, 99, &row) == MV_MOBILE_ERR_INVALID_ARG);
  mv_mobile_list_close(list);
  mv_mobile_library_close(lib);
}

TEST_CASE("cancel and state never wait for an engine that is starting", "[mobile]") {
  generation_fixture g("latch");
  mv_mobile_library* lib = g.open();
  REQUIRE(lib);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  mv::mobile::testing::set_start_hook([&] {
    entered.set_value();
    released.wait();
  });
  std::thread starter([&] { (void)mv_mobile_search_start(lib); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);

  // The start is held inside: neither call may wait for it.
  auto quick = std::async(std::launch::async, [&] {
    mv_mobile_search_cancel(lib);
    return mv_mobile_search_state(lib);
  });
  const bool answered = quick.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  starter.join();
  mv::mobile::testing::set_start_hook({});
  REQUIRE(answered);
  CHECK(quick.get() == MV_MOBILE_SEARCH_WARMING);
  mv_mobile_library_close(lib);
}
