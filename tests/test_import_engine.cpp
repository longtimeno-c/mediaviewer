// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Import engine end to end, over the real host table (io ports, pairing,
// verified copy), at card scale: docs/design/18's PR 16-19 verify lines, the ones a
// machine without a card reader can check. The 64 GB timing, eject, and the
// present-loop gates while importing are hardware runs (docs/design/10).
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_import.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addons/import/engine.h"
#include "addons/import/host.h"
#include "addons/import/library_index.h"
#include "addons/import/naming.h"
#include "addons/import/paths.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;

namespace {

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string join(const std::string& root, const std::string& rel) {
  return mv::import::join_native(root, rel);
}

constexpr std::int64_t kSat = 1789999385;  // 2026-09-21 14:03:05
constexpr std::int64_t kSun = 1790069400;  // 2026-09-22 09:30:00

// The card's file time: the UTC instant this machine shows as a minute before
// kSat. Capture times are local wall clock, and the scanner reads a file time
// as local wall clock too, so stamping kSat - 60 as UTC sorts the dateless HEIC
// after kSat on any machine east of UTC.
std::int64_t card_mtime() {
  const std::int64_t wall = kSat - 60;
  return wall - (mv::import::local_wall_from_utc(wall) - wall);
}

// Wraps the real host table: counts copies and hashes, injects faults, and
// answers volume_of for fake "cards" (a Linux temp folder is no removable
// volume).
struct wrap_state {
  const mv_host_api* real = nullptr;
  std::atomic<int> copies{0};
  std::atomic<int> hashes{0};
  std::atomic<std::uint64_t> copy_bytes{0};
  std::uint32_t fault_times = 0;
  std::int32_t fault_target = 0;
  std::function<void(int)> before_copy;
  std::map<std::string, std::string> cards;  // root -> volume id
  // Destinations that are mounts of their own, so one can "remount" elsewhere.
  struct fake_disk {
    std::string volume_id;
    std::string device_key;
    bool network = false;
  };
  std::map<std::string, fake_disk> disks;  // root -> disk
  // A UNC share root that no real stat should reach.
  std::string share_root;
  bool share_up = false;
  std::atomic<int> share_stats{0};
};
wrap_state* g_wrap = nullptr;

mv_status MV_CALL w_copy(void* host, const mv_addon_copy_request* req, mv_addon_copy_result* out) {
  const int n = g_wrap->copies.fetch_add(1);
  if (g_wrap->before_copy) g_wrap->before_copy(n);
  mv_addon_copy_request r = *req;
  if (g_wrap->fault_times > 0) {
    r.fault_target = g_wrap->fault_target;
    r.fault_offset = 3;
    r.fault_times = g_wrap->fault_times;
  }
  const mv_status s = g_wrap->real->copy_verified(host, &r, out);
  g_wrap->copy_bytes += out->bytes;
  return s;
}

mv_status MV_CALL w_hash(void* host, const char* path, uint32_t uncached,
                         int32_t(MV_CALL* c)(void*), void(MV_CALL* y)(void*), void* u,
                         uint8_t out[32]) {
  ++g_wrap->hashes;
  return g_wrap->real->hash_file(host, path, uncached, c, y, u, out);
}

void fill_disk(mv_addon_volume& out, const std::string& root, const wrap_state::fake_disk& d) {
  out = mv_addon_volume{};
  std::snprintf(out.volume_id, sizeof out.volume_id, "%s", d.volume_id.c_str());
  std::snprintf(out.root_utf8, sizeof out.root_utf8, "%s", root.c_str());
  std::snprintf(out.device_key, sizeof out.device_key, "%s", d.device_key.c_str());
  out.network = d.network ? 1 : 0;
}

mv_status MV_CALL w_stat(void* host, const char* path, uint64_t* size, int64_t* mtime, uint32_t* dir) {
  if (!g_wrap->share_root.empty() && g_wrap->share_root == path) {
    ++g_wrap->share_stats;
    if (!g_wrap->share_up) return MV_ERR_NOT_FOUND;
    *size = 0;
    *mtime = 0;
    *dir = 1;
    return MV_OK;
  }
  return g_wrap->real->stat_file(host, path, size, mtime, dir);
}

mv_status MV_CALL w_list_volumes(void* host, mv_addon_volume* out, uint32_t cap, uint32_t* count) {
  uint32_t n = 0;
  const mv_status s = g_wrap->real->list_volumes(host, out, cap, &n);
  if (s != MV_OK) return s;
  n = std::min(n, cap);
  for (const auto& [root, disk] : g_wrap->disks) {
    if (n < cap) fill_disk(out[n++], root, disk);
  }
  *count = n;
  return MV_OK;
}

mv_status MV_CALL w_volume_of(void* host, const char* path, mv_addon_volume* out) {
  const mv_status s = g_wrap->real->volume_of(host, path, out);
  const std::string p(path);
  for (const auto& [root, disk] : g_wrap->disks) {
    if (p == root || (p.rfind(root, 0) == 0 && (p[root.size()] == '/' || p[root.size()] == '\\'))) {
      fill_disk(*out, root, disk);
      return MV_OK;
    }
  }
  for (const auto& [root, id] : g_wrap->cards) {
    if (std::string(path).rfind(root, 0) == 0) {
      *out = mv_addon_volume{};
      std::snprintf(out->volume_id, sizeof out->volume_id, "%s", id.c_str());
      std::snprintf(out->root_utf8, sizeof out->root_utf8, "%s", root.c_str());
      std::snprintf(out->label_utf8, sizeof out->label_utf8, "EOS_DIGITAL");
      std::snprintf(out->device_key, sizeof out->device_key, "card:%s", id.c_str());
      out->removable = 1;
      return MV_OK;
    }
  }
  return s;
}

mv_status MV_CALL w_eject(void*, const char*) { return MV_OK; }

struct rig {
  scratch_dir dir{"import"};
  std::map<std::string, std::int64_t> dates;  // file name -> capture time
  std::mutex events_m;
  std::vector<mv_addon_event> events;
  wrap_state wrap;
  std::unique_ptr<mv::addon::host_table> table;
  mv_host_api api{};
  std::unique_ptr<mv::import::engine> eng;
  // A stand-in Recycle Bin / Trash: a folder beside the test tree, so a test
  // never touches the machine's own bin. `bin_refuses` plays a network share.
  bool bin_refuses = false;
  int binned = 0;

  rig() {
    mv::addon::host_services svc;
    svc.capture = [this](const std::string& path, mv_addon_capture& out) {
      const auto it = dates.find(utf8(fs::path(path).filename()));
      if (it == dates.end()) return false;
      out.taken_unix = it->second;
      out.has_date = 1;
      std::snprintf(out.camera_utf8, sizeof out.camera_utf8, "Canon EOS R5");
      return true;
    };
    svc.post = [this](const mv_addon_event& e) {
      std::lock_guard lock(events_m);
      events.push_back(e);
    };
    svc.recycle = [this](const std::string& path) -> mv::result<bool> {
      if (bin_refuses) return false;
      const fs::path to = dir / "Bin" / std::to_string(binned++);
      fs::create_directories(to);
      std::error_code ec;
      fs::rename(from_utf8(path), to / from_utf8(path).filename(), ec);
      if (ec) return mv::err(mv::status::io);
      return true;
    };
    svc.data_dir = utf8(dir / "data");
    svc.default_library = utf8(dir / "Pictures/MediaViewer");
    svc.test_hooks = true;
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    wrap.real = table->api();
    api = *table->api();
    api.copy_verified = &w_copy;
    api.hash_file = &w_hash;
    api.volume_of = &w_volume_of;
    api.list_volumes = &w_list_volumes;
    api.stat_file = &w_stat;
    api.eject_volume = &w_eject;
    api.watch_volumes = nullptr;  // tests call on_volume directly
    g_wrap = &wrap;
    restart();
  }
  ~rig() {
    eng.reset();
    g_wrap = nullptr;
  }

  void restart() {
    eng.reset();
    eng = std::make_unique<mv::import::engine>(&api);
    REQUIRE(eng->start(utf8(dir / "data/import.db")));
  }

  fs::path card() const { return dir / "EOS_DIGITAL"; }
  fs::path dest() const { return dir / "Photos"; }

  // A camera-shaped card: a RAW+JPEG pair with an .xmp, a lone JPEG, a clip
  // with its .THM, a Live Photo, a camera system file, and a HEIC with no
  // capture date. Two shooting days.
  void make_card() {
    const fs::path d = card() / "DCIM/100CANON";
    write_bytes(d / "IMG_0001.JPG", pattern(300000, 1));
    write_bytes(d / "IMG_0001.CR3", pattern(900000, 2));
    write_text(d / "IMG_0001.xmp", "<x:xmpmeta/>");
    write_bytes(d / "IMG_0002.JPG", pattern(310000, 3));
    write_bytes(d / "MVI_0003.MP4", pattern(1200000, 4));
    write_bytes(d / "MVI_0003.THM", pattern(9000, 5));
    write_bytes(d / "IMG_0004.HEIC", pattern(200000, 6));
    write_bytes(d / "IMG_0004.MOV", pattern(700000, 7));
    write_bytes(card() / "MISC/INDEX.BDM", pattern(100, 8));
    write_bytes(dir / "EOS_DIGITAL/DCIM/101CANON/IMG_0100.HEIC", pattern(150000, 9));
    for (const char* n : {"IMG_0001.JPG", "IMG_0001.CR3", "IMG_0002.JPG", "IMG_0004.HEIC", "IMG_0004.MOV"}) {
      dates[n] = kSat;
    }
    dates["MVI_0003.MP4"] = kSun;
    // Every file gets a fixed mtime, like a camera's.
    for (const auto& e : fs::recursive_directory_iterator(card())) {
      if (e.is_regular_file()) set_mtime(e.path(), card_mtime());
    }
    wrap.cards[utf8(card())] = "uuid:CARD-A";
  }

  std::string preset(const std::string& extra = "") const {
    return R"({"name":"Test","destination":")" + json_path(dest()) + R"(","eject_after":false)" + extra +
           "}";
  }

  std::string preset_to(const fs::path& to, const std::string& extra = "") const {
    return R"({"name":"Test","destination":")" + json_path(to) + R"(","eject_after":false)" + extra + "}";
  }

  void mount(const fs::path& root, const std::string& volume_id, const std::string& device_key,
             bool network) {
    fs::create_directories(root);
    wrap.disks[utf8(root)] = wrap_state::fake_disk{volume_id, device_key, network};
  }
  void unmount(const fs::path& root) { wrap.disks.erase(utf8(root)); }

  std::unique_ptr<mv::import::library_index> open_index() {
    auto idx = mv::import::library_index::open(utf8(dir / "data/import.db"));
    REQUIRE(idx);
    return std::move(*idx);
  }

  mv::json::value plan_of(std::uint64_t plan_id) {
    auto text = eng->plan_json(plan_id);
    REQUIRE(text);
    auto v = mv::json::parse(*text);
    REQUIRE(v);
    return *v;
  }

  // scan -> plan -> json.
  std::pair<std::uint64_t, mv::json::value> plan(const fs::path& root, const std::string& p) {
    auto s = eng->scan(utf8(root));
    REQUIRE(s);
    auto pl = eng->plan(*s, &p, nullptr);
    REQUIRE(pl);
    eng->wait_idle();
    return {*pl, plan_of(*pl)};
  }

  std::uint64_t run(std::uint64_t plan_id) {
    auto j = eng->start_job(plan_id);
    REQUIRE(j);
    eng->wait_idle();
    return *j;
  }

  mv::json::value summary(std::uint64_t job) {
    auto text = eng->summary_json(job);
    REQUIRE(text);
    auto v = mv::json::parse(*text);
    REQUIRE(v);
    return *v;
  }

  mv_import_progress progress(std::uint64_t job) {
    mv_import_progress p{};
    REQUIRE(eng->progress(job, p));
    return p;
  }
};

std::int64_t totals(const mv::json::value& plan, const char* key) {
  return *plan.find("totals")->integer(key);
}

}  // namespace

TEST_CASE("a card imports into dated folders, pairs together, every copy verified",
          "[import][engine]") {
  rig r;
  r.make_card();
  auto [plan_id, plan] = r.plan(r.card(), r.preset());
  // 6 units: pair, lone JPEG, clip, Live Photo, dateless HEIC, and none for INDEX.BDM.
  REQUIRE(totals(plan, "units") == 5);
  REQUIRE(totals(plan, "new") == 5);
  REQUIRE(totals(plan, "selected_units") == 5);

  const std::uint64_t job = r.run(plan_id);
  const auto p = r.progress(job);
  REQUIRE(p.state == MV_IMPORT_JOB_DONE);
  REQUIRE(p.units_done == 5);
  REQUIRE(p.units_failed == 0);

  const auto files = list_tree(r.dest());
  const std::vector<std::string> want_sat{
      "2026/2026-09-21/IMG_0001.CR3", "2026/2026-09-21/IMG_0001.JPG",
      "2026/2026-09-21/IMG_0001.xmp", "2026/2026-09-21/IMG_0002.JPG",
      "2026/2026-09-21/IMG_0004.HEIC", "2026/2026-09-21/IMG_0004.MOV"};
  for (const auto& f : want_sat) REQUIRE(std::find(files.begin(), files.end(), f) != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-22/MVI_0003.MP4") != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-22/MVI_0003.THM") != files.end());
  // Not media: never copied.
  for (const auto& f : files) REQUIRE(f.find("INDEX.BDM") == std::string::npos);
  // Every destination file is byte-identical to its source.
  REQUIRE(read_bytes(r.dest() / "2026/2026-09-21/IMG_0001.CR3") ==
          read_bytes(r.card() / "DCIM/100CANON/IMG_0001.CR3"));
  REQUIRE(read_bytes(r.dest() / "2026/2026-09-22/MVI_0003.MP4") ==
          read_bytes(r.card() / "DCIM/100CANON/MVI_0003.MP4"));
  // The source is never touched: nothing deleted from the card.
  REQUIRE(list_tree(r.card()).size() == 10);

  // The layout preview is what landed, file for file (PR 18 verify).
  std::vector<std::string> previewed;
  for (const auto& u : plan.find("units")->a) {
    const std::string folder = *u.str("folder");
    for (const auto& n : u.find("names")->a) previewed.push_back(folder + "/" + n.s);
  }
  std::sort(previewed.begin(), previewed.end());
  REQUIRE(previewed == files);

  const auto s = r.summary(job);
  REQUIRE(*s.str("state") == "done");
  REQUIRE(*s.find("copied")->integer("units") == 5);
  REQUIRE(fs::exists(*s.str("report")));
}

TEST_CASE("re-importing writes zero bytes and does not re-read the destination",
          "[import][engine]") {
  rig r;
  r.make_card();
  auto [plan_id, plan] = r.plan(r.card(), r.preset());
  r.run(plan_id);
  const auto before = list_tree(r.dest());

  r.restart();  // a later session: everything from import.db
  r.wrap.copies = 0;
  r.wrap.hashes = 0;
  auto [again, plan2] = r.plan(r.card(), r.preset(R"(,"selection":"all")"));
  REQUIRE(totals(plan2, "selected_units") == 0);
  REQUIRE(totals(plan2, "duplicates") == 5);
  REQUIRE(r.wrap.hashes == 0);  // card memory + library index: no reads at all
  const std::uint64_t job = r.run(again);
  REQUIRE(r.wrap.copies == 0);
  REQUIRE(list_tree(r.dest()) == before);
  const auto s = r.summary(job);
  REQUIRE(s.find("skipped")->a.size() == 5);
  REQUIRE_FALSE(s.find("skipped")->a[0].str("matched")->empty());

  SECTION("a renamed file on the card is still skipped, by content") {
    fs::rename(r.card() / "DCIM/100CANON/IMG_0002.JPG", r.card() / "DCIM/100CANON/IMG_9999.JPG");
    r.wrap.hashes = 0;
    auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"selection":"all")"));
    REQUIRE(totals(pl, "duplicates") == 5);
    REQUIRE(r.wrap.hashes == 1);  // that one file, read once
  }
  SECTION("one byte changed in a same-named JPEG is kept under a safe name") {
    auto bytes = read_bytes(r.card() / "DCIM/100CANON/IMG_0002.JPG");
    bytes[1000] ^= 0x40;
    write_bytes(r.card() / "DCIM/100CANON/IMG_0002.JPG", bytes);
    auto [pid, pl] = r.plan(r.card(), r.preset());
    REQUIRE(totals(pl, "selected_units") == 1);
    r.run(pid);
    REQUIRE(read_bytes(r.dest() / "2026/2026-09-21/IMG_0002 (2).JPG") == bytes);
    REQUIRE(read_bytes(r.dest() / "2026/2026-09-21/IMG_0002.JPG") != bytes);  // untouched
  }
}

TEST_CASE("new since last import is a lookup, and the default selection", "[import][engine]") {
  rig r;
  r.make_card();
  auto [plan_id, plan] = r.plan(r.card(), r.preset());
  r.run(plan_id);
  write_bytes(r.card() / "DCIM/100CANON/IMG_0005.JPG", pattern(123456, 50));
  r.dates["IMG_0005.JPG"] = kSun;
  auto [pid, pl] = r.plan(r.card(), r.preset());
  REQUIRE(totals(pl, "selected_units") == 1);
  const std::string sources = r.eng->sources_json();
  (void)sources;  // lists removable volumes only; the fake card is not one to the OS
}

TEST_CASE("a bad write is retried, then reported; Retry failed finishes it", "[import][engine]") {
  rig r;
  r.make_card();
  SECTION("once: retried and done") {
    r.wrap.fault_times = 1;
    auto [pid, pl] = r.plan(r.card(), r.preset());
    const auto job = r.run(pid);
    REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  }
  SECTION("every time for one file: that unit fails, whole, and is reported") {
    r.wrap.before_copy = [&](int n) { r.wrap.fault_times = n == 0 ? 2 : 0; };
    auto [pid, pl] = r.plan(r.card(), r.preset());
    const auto job = r.run(pid);
    const auto p = r.progress(job);
    REQUIRE(p.state == MV_IMPORT_JOB_FAILED);
    REQUIRE(p.units_failed == 1);
    const auto s = r.summary(job);
    REQUIRE_FALSE(s.find("failed")->a.empty());
    for (const std::string& f : list_tree(r.dest())) REQUIRE(f.find(".mvtmp") == std::string::npos);

    r.wrap.before_copy = nullptr;
    r.wrap.fault_times = 0;
    auto retry = r.eng->retry_failed(job);
    REQUIRE(retry);
    r.eng->wait_idle();
    REQUIRE(r.progress(*retry).state == MV_IMPORT_JOB_DONE);
    REQUIRE(r.progress(*retry).units_done == 1);
  }
}

TEST_CASE("a pulled card interrupts; resume re-copies only what was not verified",
          "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset());
  const fs::path gone = r.dir / "pulled";
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(r.card(), gone);  // the card leaves mid-job
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  const int copied_before = r.wrap.copies;
  REQUIRE(copied_before == 4);
  const auto partial = list_tree(r.dest());

  // A new session sees it as unfinished.
  r.wrap.before_copy = nullptr;
  fs::rename(gone, r.card());
  r.restart();
  const auto unfinished = mv::json::parse(r.eng->unfinished_json());
  REQUIRE(unfinished);
  REQUIRE(unfinished->a.size() == 1);
  r.wrap.copies = 0;
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  // 9 media and sidecar files in all; the verified ones were not copied again.
  REQUIRE(r.wrap.copies == 9 - static_cast<int>(partial.size()));
  REQUIRE(list_tree(r.dest()).size() == 9);
}

TEST_CASE("cancel stops at a unit boundary and leaves no temporary", "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset());
  std::uint64_t job = 0;
  r.wrap.before_copy = [&](int n) {
    if (n == 2) (void)r.eng->cancel(job);
  };
  auto started = r.eng->start_job(pid);
  REQUIRE(started);
  job = *started;
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_CANCELLED);
  for (const std::string& f : list_tree(r.dest())) REQUIRE(f.find(".mvtmp") == std::string::npos);
}

TEST_CASE("a backup destination is written from the same read", "[import][engine]") {
  rig r;
  r.make_card();
  const fs::path backup = r.dir / "Backup";
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"backup":")" + json_path(backup) + R"(")"));
  r.run(pid);
  REQUIRE(list_tree(r.dest()) == list_tree(backup));
  REQUIRE(r.wrap.copies == 9);  // one copy call (one card read) per file
  std::uint64_t card_bytes = 0;
  for (const auto& e : fs::recursive_directory_iterator(r.card())) {
    if (e.is_regular_file() && e.path().extension() != ".BDM") card_bytes += e.file_size();
  }
  REQUIRE(r.wrap.copy_bytes == card_bytes);
}

TEST_CASE("a file the backup could not take is taken back from the main destination",
          "[import][engine]") {
  rig r;
  r.make_card();
  const fs::path backup = r.dir / "Backup";
  // The first file verifies on the main destination but fails twice on the
  // backup: the unit is failed, and neither destination keeps any of it, so
  // Retry failed finds the names free and writes both.
  r.wrap.fault_target = 1;
  r.wrap.before_copy = [&](int n) { r.wrap.fault_times = n == 0 ? 2 : 0; };
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"backup":")" + json_path(backup) + R"(")"));
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_FAILED);
  REQUIRE(r.progress(job).units_failed == 1);
  REQUIRE(list_tree(r.dest()) == list_tree(backup));
  REQUIRE(list_tree(r.dest()).size() < 9);

  r.wrap.before_copy = nullptr;
  r.wrap.fault_times = 0;
  auto retry = r.eng->retry_failed(job);
  REQUIRE(retry);
  r.eng->wait_idle();
  REQUIRE(r.progress(*retry).state == MV_IMPORT_JOB_DONE);
  REQUIRE(list_tree(r.dest()).size() == 9);
  REQUIRE(list_tree(r.dest()) == list_tree(backup));
}

TEST_CASE("a duplicate already in the library still goes to a backup that lacks it",
          "[import][engine]") {
  rig r;
  r.make_card();
  // The main library already holds this content, imported from a copy of the
  // card in another reader (a different volume, so not "imported before").
  const fs::path other = r.dir / "OTHER_CARD";
  fs::copy(r.card(), other, fs::copy_options::recursive);
  for (const auto& e : fs::recursive_directory_iterator(other)) {
    if (e.is_regular_file()) set_mtime(e.path(), card_mtime());  // as make_card() stamps them
  }
  r.wrap.cards[utf8(other)] = "uuid:CARD-B";
  auto [p1, pl1] = r.plan(other, r.preset());
  r.run(p1);
  const auto library = list_tree(r.dest());
  REQUIRE(library.size() == 9);

  const fs::path backup = r.dir / "Backup";
  const std::string with_backup = r.preset(R"(,"backup":")" + json_path(backup) + R"(")");
  auto [p2, pl2] = r.plan(r.card(), with_backup);
  REQUIRE(totals(pl2, "duplicates") == 5);
  REQUIRE(totals(pl2, "backup_only") == 5);
  REQUIRE(totals(pl2, "selected_units") == 5);  // work to do: the Import button counts it
  r.wrap.copies = 0;
  const auto job = r.run(p2);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  REQUIRE(r.wrap.copies == 9);
  REQUIRE(list_tree(r.dest()) == library);  // nothing new in the library
  REQUIRE(list_tree(backup) == library);    // the backup mirrors the card
  const auto sum = r.summary(job);
  REQUIRE(sum.find("skipped")->a.size() == 5);  // still reported as duplicates on the main
  REQUIRE(*sum.find("copied")->integer("files") == 9);

  // Now both hold it: nothing to do, for either.
  auto [p3, pl3] = r.plan(r.card(), with_backup);
  REQUIRE(totals(pl3, "backup_only") == 0);
  REQUIRE(totals(pl3, "selected_units") == 0);

  // Clearing the selection clears the backup work too. (The card is
  // "imported before" now, so "new only" would select nothing: take all.)
  const std::string again = json_path(r.dir / "Backup2");
  auto [p4, pl4] =
      r.plan(r.card(), r.preset(R"(,"selection":"all","backup":")" + again + R"(")"));
  REQUIRE(totals(pl4, "backup_only") == 5);
  REQUIRE(r.eng->select(p4, -1, nullptr, false));
  r.eng->wait_idle();
  REQUIRE(totals(r.plan_of(p4), "backup_only") == 0);
  REQUIRE(totals(r.plan_of(p4), "selected_units") == 0);
}

TEST_CASE("resume copies only the destination a crash left without the file",
          "[import][engine]") {
  rig r;
  r.make_card();
  const fs::path backup = r.dir / "Backup";
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"backup":")" + json_path(backup) + R"(")"));
  const fs::path gone = r.dir / "pulled";
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(r.card(), gone);
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  r.wrap.before_copy = nullptr;

  // A crash between the two renames: one pending file is on the main
  // destination, verified bytes and all, and not on the backup.
  std::string crashed;
  {
    auto idx = mv::import::library_index::open(utf8(r.dir / "data/import.db"));
    REQUIRE(idx);
    for (const auto& row : (*idx)->journal(job)) {
      if (row.state != mv::import::member_state::pending || row.targets.size() != 2) continue;
      const std::string rel = row.src.substr(utf8(r.card()).size() + 1);
      // The journal keeps a target relative to its destination when the
      // destination has a volume identity (a Windows temp folder does).
      const auto live = [](const fs::path& home, const std::string& t) {
        return fs::path(t).is_relative() ? home / fs::path(t) : fs::path(t);
      };
      const fs::path first = live(r.dest(), row.targets[0]);
      fs::create_directories(first.parent_path());
      fs::copy_file(gone / rel, first);
      crashed = utf8(live(backup, row.targets[1]));
      break;
    }
  }
  REQUIRE_FALSE(crashed.empty());
  fs::rename(gone, r.card());
  r.restart();
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  REQUIRE(fs::exists(crashed));
  REQUIRE(list_tree(r.dest()).size() == 9);
  REQUIRE(list_tree(r.dest()) == list_tree(backup));
}

namespace {
const char* const kSmbUser = "dev://tristan@NAS._smb._tcp.local/photos";
const char* const kSmbOther = "dev://TRISTAN@nas.local/Photos";
}  // namespace

TEST_CASE("a destination that remounts under a new path still dedupes and keeps {seq}",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path nas = r.dir / "NAS";
  const fs::path moved = r.dir / "Remounted/NAS-1";
  // A share's filesystem id is new at every mount; its server and share are not.
  r.mount(nas, "fsid:00000001", kSmbUser, true);
  const std::string numbered = R"(,"rename":"{date}_{seq}","selection":"all")";
  auto [pid, pl] = r.plan(r.card(), r.preset_to(nas / "Photos", numbered));
  REQUIRE(totals(pl, "selected_units") == 5);
  r.run(pid);
  const auto before = list_tree(nas / "Photos");
  REQUIRE(before.size() == 9);

  fs::create_directories(moved.parent_path());
  fs::rename(nas, moved);
  r.unmount(nas);
  r.mount(moved, "fsid:00000002", kSmbOther, true);
  r.restart();
  r.wrap.copies = 0;
  r.wrap.hashes = 0;
  auto [again, plan2] = r.plan(r.card(), r.preset_to(moved / "Photos", numbered));
  REQUIRE(totals(plan2, "selected_units") == 0);
  REQUIRE(totals(plan2, "duplicates") == 5);
  REQUIRE(r.wrap.hashes == 0);
  r.run(again);
  REQUIRE(r.wrap.copies == 0);
  REQUIRE(list_tree(moved / "Photos") == before);

  // {seq} carries on from the count made under the old path.
  write_bytes(r.card() / "DCIM/100CANON/IMG_0006.JPG", pattern(77777, 60));
  r.dates["IMG_0006.JPG"] = kSat;
  auto [pid2, pl2] = r.plan(r.card(), r.preset_to(moved / "Photos", numbered));
  r.run(pid2);
  const auto more = list_tree(moved / "Photos");
  REQUIRE(std::find(more.begin(), more.end(), "2026/2026-09-21/2026-09-21_0005.JPG") != more.end());
}

TEST_CASE("a share has one identity however it was mounted", "[import][engine][remount]") {
  using mv::import::share_identity;
  CHECK(share_identity(kSmbUser) == share_identity(kSmbOther));
  CHECK(share_identity("dev://me@nas.local/photos/") == "dev://me@nas.local/photos");
  CHECK(share_identity("dev://nas.local/photos") != share_identity("dev://nas.local/backup"));
  CHECK(share_identity("dev://alice@nas.local/home") != share_identity("dev://bob@nas.local/home"));
  CHECK(share_identity("dev:nas:/Export") != share_identity("dev:nas:/export"));  // NFS
  // Windows' form, from a drive letter or a UNC root alike.
  CHECK(share_identity("net:\\\\NAS\\Media") == "net:\\\\nas\\media");
}

TEST_CASE("a local disk that comes back under another path is the same destination",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path disk = r.dir / "Disk";
  const fs::path moved = r.dir / "Disk 1";
  r.mount(disk, "uuid:DISK-1", "bsd:disk4", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(disk / "Photos"));
  r.run(pid);
  fs::rename(disk, moved);
  r.unmount(disk);
  r.mount(moved, "uuid:DISK-1", "bsd:disk7", false);  // the BSD name moves too
  r.restart();
  r.wrap.hashes = 0;
  auto [again, plan2] = r.plan(r.card(), r.preset_to(moved / "Photos", R"(,"selection":"all")"));
  REQUIRE(totals(plan2, "selected_units") == 0);
  REQUIRE(totals(plan2, "duplicates") == 5);
  REQUIRE(r.wrap.hashes == 0);  // the ledger answered; the old path's rows were not lost
}

TEST_CASE("a resized Windows volume is the same destination", "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path disk = r.dir / "Disk";
  r.mount(disk, "vsn:1A2B3C4D:1000", "disk:2", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(disk / "Photos"));
  r.run(pid);
  r.mount(disk, "vsn:1A2B3C4D:2000", "disk:2", false);  // the same serial, a bigger partition
  r.restart();
  r.wrap.hashes = 0;
  auto [again, plan2] = r.plan(r.card(), r.preset_to(disk / "Photos", R"(,"selection":"all")"));
  REQUIRE(totals(plan2, "selected_units") == 0);
  REQUIRE(totals(plan2, "duplicates") == 5);
  REQUIRE(r.wrap.hashes == 0);
}

TEST_CASE("a UNC destination is checked once per plan, answered or not",
          "[import][engine][remount]") {
  rig r;
  r.wrap.share_root = "\\\\server\\share";
  const std::string key = std::string("vol:net:\\\\server\\share") + '\x1f' + "Photos";
  const mv::import::host h(&r.api);
  const mv::import::dest_locator loc(h);
  std::optional<std::string> first;
  SECTION("online") {
    r.wrap.share_up = true;
    first = loc.live_root(key);
    REQUIRE(first == mv::import::join_native(r.wrap.share_root, "Photos"));
  }
  SECTION("offline, where each attempt would wait on the network") {
    first = loc.live_root(key);
    REQUIRE_FALSE(first);
  }
  for (int i = 0; i < 5; ++i) REQUIRE(loc.live_root(key) == first);
  REQUIRE(r.wrap.share_stats == 1);
}

TEST_CASE("an interrupted job resumes on a destination that remounted elsewhere",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path nas = r.dir / "NAS";
  const fs::path away = r.dir / "offline";
  const fs::path moved = r.dir / "Remounted/NAS-1";
  r.mount(nas, "fsid:00000001", kSmbUser, true);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(nas / "Photos"));
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(nas, away);  // the share drops mid-job
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  r.wrap.before_copy = nullptr;
  r.unmount(nas);
  const auto partial = list_tree(away / "Photos");
  REQUIRE_FALSE(partial.empty());
  r.restart();

  // Still offline: Resume changes nothing and creates nothing.
  const auto journal_before = r.open_index()->journal(job);
  r.wrap.copies = 0;
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  REQUIRE(r.wrap.copies == 0);
  REQUIRE_FALSE(fs::exists(nas));
  const auto journal_after = r.open_index()->journal(job);
  REQUIRE(journal_after.size() == journal_before.size());
  for (std::size_t i = 0; i < journal_after.size(); ++i) {
    REQUIRE(journal_after[i].state == journal_before[i].state);
    REQUIRE(journal_after[i].targets == journal_before[i].targets);
    REQUIRE(journal_after[i].target_roots == journal_before[i].target_roots);
  }
  const auto unfinished = mv::json::parse(r.eng->unfinished_json());
  REQUIRE(unfinished);
  REQUIRE(unfinished->a.size() == 1);

  // Back, somewhere else: only what was not verified is copied, and there.
  fs::create_directories(moved.parent_path());
  fs::rename(away, moved);
  r.mount(moved, "fsid:00000002", kSmbOther, true);
  r.restart();
  r.wrap.copies = 0;
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  REQUIRE(r.wrap.copies == 9 - static_cast<int>(partial.size()));
  REQUIRE(list_tree(moved / "Photos").size() == 9);
  REQUIRE_FALSE(fs::exists(nas));

  // What the resumed job indexed dedupes too.
  auto [again, plan2] = r.plan(r.card(), r.preset_to(moved / "Photos", R"(,"selection":"all")"));
  REQUIRE(totals(plan2, "selected_units") == 0);
}

TEST_CASE("an interrupted job resumes from a card that came back under another path",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path away = r.dir / "pulled";
  const fs::path moved = r.dir / "Cards/EOS_DIGITAL 1";
  auto [pid, pl] = r.plan(r.card(), r.preset());
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(r.card(), away);
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  r.wrap.before_copy = nullptr;
  const auto partial = list_tree(r.dest());
  fs::create_directories(moved.parent_path());
  fs::rename(away, moved);
  r.wrap.cards.erase(utf8(r.card()));
  r.wrap.cards[utf8(moved)] = "uuid:CARD-A";
  r.wrap.disks[utf8(moved)] = wrap_state::fake_disk{"uuid:CARD-A", "card:uuid:CARD-A", false};
  r.restart();
  r.wrap.copies = 0;
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  REQUIRE(r.wrap.copies == 9 - static_cast<int>(partial.size()));
  REQUIRE(list_tree(r.dest()).size() == 9);
}

TEST_CASE("a library-wide plan leaves the rows of an offline destination alone",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path nas = r.dir / "NAS";
  const fs::path away = r.dir / "offline";
  r.mount(nas, "uuid:NAS-1", "bsd:disk4", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(nas / "Photos"));
  r.run(pid);
  const auto before = r.open_index()->all_rows();
  REQUIRE(before.size() == 9);
  fs::rename(nas, away);
  r.unmount(nas);
  // Its rows can't be confirmed, so nothing counts as a duplicate, and nothing is forgotten.
  auto [again, plan2] = r.plan(r.card(), r.preset_to(r.dir / "Other", R"(,"selection":"all","scope":"library")"));
  REQUIRE(totals(plan2, "selected_units") == 5);
  REQUIRE(r.open_index()->all_rows().size() == before.size());
}

TEST_CASE("a per-user share resumes on that user's mount", "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path alice = r.dir / "A-home";
  const fs::path bob = r.dir / "B-home";
  const fs::path away = r.dir / "offline";
  const fs::path moved = r.dir / "B-home-1";
  r.mount(alice, "fsid:0000000a", "dev://alice@nas.local/home", true);
  r.mount(bob, "fsid:0000000b", "dev://bob@nas.local/home", true);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(bob / "Photos"));
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(bob, away);
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  r.wrap.before_copy = nullptr;
  r.unmount(bob);
  fs::rename(away, moved);
  r.mount(moved, "fsid:0000000c", "dev://bob@nas.local/home", true);
  r.restart();
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  REQUIRE(list_tree(moved / "Photos").size() == 9);
  REQUIRE(list_tree(alice).empty());
}

TEST_CASE("two disks with one id leave the job interrupted", "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path disk = r.dir / "Disk";
  const fs::path away = r.dir / "offline";
  const fs::path moved = r.dir / "Disk 1";
  const fs::path clone = r.dir / "Clone";
  r.mount(disk, "uuid:DISK-1", "bsd:disk4", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(disk / "Photos"));
  r.wrap.before_copy = [&](int n) {
    if (n == 3) fs::rename(disk, away);
  };
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  r.wrap.before_copy = nullptr;
  r.unmount(disk);
  fs::rename(away, moved);
  r.mount(moved, "uuid:DISK-1", "bsd:disk5", false);
  r.mount(clone, "uuid:DISK-1", "bsd:disk6", false);
  r.restart();
  r.wrap.copies = 0;
  REQUIRE(r.eng->resume(job));
  r.eng->wait_idle();
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_INTERRUPTED);
  REQUIRE(r.wrap.copies == 0);
  REQUIRE(list_tree(clone).empty());
}

TEST_CASE("Retry failed does not write to another disk now at the old path",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path nas = r.dir / "NAS";
  const fs::path away = r.dir / "offline";
  r.mount(nas, "uuid:NAS-1", "bsd:disk4", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(nas / "Photos"));
  r.wrap.fault_times = 1000;  // every copy fails, so the job ends with failures
  const auto job = r.run(pid);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_FAILED);
  r.wrap.fault_times = 0;
  fs::rename(nas, away);
  r.unmount(nas);
  fs::create_directories(nas);  // something else answers at the old path, with no key
  r.wrap.copies = 0;
  auto retry = r.eng->retry_failed(job);
  REQUIRE(retry);
  r.eng->wait_idle();
  REQUIRE(r.progress(*retry).state == MV_IMPORT_JOB_INTERRUPTED);
  REQUIRE(r.wrap.copies == 0);
  REQUIRE(list_tree(nas).empty());
}

TEST_CASE("a volume with no id that lasts keeps its absolute path", "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path disk = r.dir / "Disk";
  std::string id = "uuid:unused";
  std::string device = "bsd:disk4";
  bool network = false;
  SECTION("an fsid") { id = "fsid:00000001:1000"; }
  SECTION("a serial of zero") { id = "vsn:00000000:1000"; }
  SECTION("a share Windows could not name") {
    id = "vsn:12345678:1000";
    device = "net:E:\\";
    network = true;
  }
  r.mount(disk, id, device, network);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(disk / "Photos"));
  r.run(pid);
  const auto rows = r.open_index()->all_rows();
  REQUIRE(rows.size() == 9);
  for (const auto& row : rows) REQUIRE(row.root == utf8(disk / "Photos"));
}

TEST_CASE("migration samples across the root, not its oldest folders",
          "[import][engine][remount]") {
  rig r;
  const fs::path nas = r.dir / "NAS";
  const std::string root = utf8(nas / "Photos");
  r.mount(nas, "uuid:NAS-1", "bsd:disk4", false);
  // Every folder but the last has gone to an archive.
  write_bytes(nas / "Photos/r096.jpg", pattern(1234, 1));
  mv::import::digest h{};
  {
    auto idx = r.open_index();
    for (int i = 0; i < 100; ++i) {
      char rel[16];
      std::snprintf(rel, sizeof rel, "r%03d.jpg", i);
      idx->upsert({root, rel, 1234, 1, h});
    }
  }
  r.restart();
  r.eng->wait_idle();
  const auto rows = r.open_index()->all_rows();
  REQUIRE(rows.size() == 100);
  REQUIRE(rows.front().root.rfind("vol:", 0) == 0);
}

TEST_CASE("rows that could not be adopted still dedupe and still count {seq}",
          "[import][engine][remount]") {
  rig r;
  r.make_card();
  const fs::path nas = r.dir / "NAS";
  const std::string root = utf8(nas / "Photos");
  r.mount(nas, "uuid:NAS-1", "bsd:disk4", false);
  auto [pid, pl] = r.plan(r.card(), r.preset_to(nas / "Photos"));
  r.run(pid);
  {
    // An import.db from before keys, as absolute rows. Rows for archived files
    // surround the real ones, so the even sample (every 12th) finds none here.
    auto idx = r.open_index();
    const auto rows = idx->all_rows();
    REQUIRE(rows.size() == 9);
    mv::import::digest h{};
    idx->upsert({root, "0-archived.jpg", 5, 1, h});
    for (const auto& row : rows) {
      idx->upsert({root, row.rel, row.size, row.mtime, row.hash});
      idx->drop(row.root, row.rel);
    }
    for (int i = 0; i < 350; ++i) {
      idx->upsert({root, "3-archived-" + std::to_string(1000 + i) + ".jpg", 5, 1, h});
    }
    idx->commit_seq(root, "2026-09-21", 7);
  }
  r.restart();
  r.wrap.copies = 0;
  r.wrap.hashes = 0;
  const std::string numbered = R"(,"rename":"{date}_{seq}","selection":"all")";
  auto [again, plan2] = r.plan(r.card(), r.preset_to(nas / "Photos", numbered));
  REQUIRE(totals(plan2, "selected_units") == 0);
  REQUIRE(totals(plan2, "duplicates") == 5);
  REQUIRE(r.wrap.hashes == 0);
  REQUIRE(r.open_index()->all_rows().front().root == root);  // not adopted

  write_bytes(r.card() / "DCIM/100CANON/IMG_0006.JPG", pattern(77777, 60));
  r.dates["IMG_0006.JPG"] = kSat;
  auto [pid2, pl2] = r.plan(r.card(), r.preset_to(nas / "Photos", numbered));
  r.run(pid2);
  const auto files = list_tree(nas / "Photos");
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-21/2026-09-21_0008.JPG") != files.end());
}

TEST_CASE("an existing import.db migrates in place and loses nothing",
          "[import][engine][remount]") {
  rig r;
  const fs::path nas = r.dir / "NAS";
  const fs::path moved = r.dir / "Remounted/NAS-1";
  const fs::path stranger = r.dir / "Stranger";
  const fs::path missing = r.dir / "Unplugged/Photos";
  const std::string online = utf8(nas / "Photos");
  const std::string offline = utf8(missing);
  const std::string other = utf8(stranger / "Photos");
  write_bytes(nas / "Photos/2026/a.jpg", pattern(1000, 1));
  write_bytes(nas / "Photos/2026/job-done.jpg", pattern(1100, 2));
  write_bytes(stranger / "Photos/unrelated.jpg", pattern(50, 3));
  r.mount(nas, "fsid:00000001", kSmbUser, true);
  r.mount(stranger, "uuid:STRANGER", "bsd:disk9", false);  // another disk at the recorded path
  mv::import::digest h{};
  h[0] = 7;
  {
    auto idx = r.open_index();
    idx->upsert({online, "2026/a.jpg", 1000, 111, h});
    idx->upsert({offline, "2026/b.jpg", 2000, 222, h});
    idx->upsert({other, "2026/c.jpg", 3000, 333, h});
    idx->commit_seq(online, "2026-09-21", 7);
    idx->commit_seq(offline, "2026-09-21", 9);
    mv::import::job_row job;
    job.id = 41;
    job.created = 1;
    job.state = MV_IMPORT_JOB_INTERRUPTED;
    REQUIRE(idx->create_job(job) == 41);
    job.id = 42;
    REQUIRE(idx->create_job(job) == 42);
    mv::import::journal_row a;
    a.job = 41;
    a.src = "/nowhere/job.jpg";
    a.size = 1100;
    a.state = mv::import::member_state::done;
    a.targets = {join(online, "2026/job-done.jpg")};
    a.target_roots = {online};
    mv::import::journal_row b = a;
    b.job = 42;
    b.targets = {join(offline, "2026/job.jpg")};
    b.target_roots = {offline};
    REQUIRE(idx->journal_add({a, b}));
  }
  r.restart();
  r.eng->wait_idle();

  auto idx = r.open_index();
  const auto rows = idx->all_rows();
  REQUIRE(rows.size() == 3);
  std::string key;
  for (const auto& row : rows) {
    if (row.rel == "2026/a.jpg") key = row.root;
    if (row.rel == "2026/b.jpg") REQUIRE(row.root == offline);
    if (row.rel == "2026/c.jpg") REQUIRE(row.root == other);  // none of its files are here
  }
  REQUIRE(key.rfind("vol:", 0) == 0);
  REQUIRE(idx->last_seq(key, "2026-09-21") == 7);
  REQUIRE(idx->last_seq(offline, "2026-09-21") == 9);
  const auto adopted = idx->journal(41);
  REQUIRE(adopted.size() == 1);
  REQUIRE(adopted[0].target_roots == std::vector<std::string>{key});
  REQUIRE(adopted[0].targets == std::vector<std::string>{"2026/job-done.jpg"});
  const auto left = idx->journal(42);
  REQUIRE(left[0].target_roots == std::vector<std::string>{offline});
  REQUIRE(left[0].targets == std::vector<std::string>{join(offline, "2026/job.jpg")});

  // A second open finds nothing more to do, and the migrated rows follow a remount.
  r.restart();
  r.eng->wait_idle();
  REQUIRE(r.open_index()->all_rows().size() == 3);
  fs::create_directories(moved.parent_path());
  fs::rename(nas, moved);
  r.unmount(nas);
  r.mount(moved, "fsid:00000002", kSmbOther, true);
  r.restart();
  r.eng->wait_idle();
  const auto later = r.open_index()->all_rows();
  REQUIRE(std::count_if(later.begin(), later.end(), [&](const auto& x) { return x.root == key; }) == 1);
}

TEST_CASE("Retry failed on an unknown job fails rather than reporting success",
          "[import][engine]") {
  rig r;
  auto retry = r.eng->retry_failed(424242);
  REQUIRE(retry);
  r.eng->wait_idle();
  REQUIRE(r.progress(*retry).state == MV_IMPORT_JOB_FAILED);
}

TEST_CASE("a journal write that fails lands none of its rows", "[import][engine]") {
  scratch_dir dir("journal");
  auto idx = mv::import::library_index::open(utf8(dir / "import.db"));
  REQUIRE(idx);
  mv::import::job_row job;
  job.id = 7;
  job.created = 1;
  REQUIRE((*idx)->create_job(job) == 7);

  auto row = [](std::uint64_t job_id, std::uint32_t unit) {
    mv::import::journal_row r;
    r.job = job_id;
    r.unit = unit;
    r.src = "card/" + std::to_string(unit);
    r.rel = std::to_string(unit);
    return r;
  };
  REQUIRE((*idx)->journal_add({row(7, 0), row(7, 1)}));
  REQUIRE((*idx)->journal(7).size() == 2);

  // The last row names no job (a foreign-key failure, as a full disk would
  // be partway through): the rows before it roll back with it.
  REQUIRE_FALSE((*idx)->journal_add({row(7, 2), row(7, 3), row(99, 0)}));
  REQUIRE((*idx)->journal(7).size() == 2);
  REQUIRE((*idx)->journal(99).empty());

  // The connection is usable afterwards: no transaction was left open.
  REQUIRE((*idx)->journal_add({row(7, 2)}));
  REQUIRE((*idx)->journal(7).size() == 3);
  REQUIRE((*idx)->seen_store({{"/a/x.jpg", 1, 2, {}}}, nullptr));
  REQUIRE((*idx)->seen_under("/a/").size() == 1);
}

TEST_CASE("rename templates number per day and survive re-imports", "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"rename":"{date}_{seq}")"));
  r.run(pid);
  const auto files = list_tree(r.dest());
  // In capture order: the dateless HEIC's file time is a minute earlier.
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-21/2026-09-21_0001.HEIC") != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-21/2026-09-21_0002.CR3") != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-21/2026-09-21_0002.JPG") != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-21/2026-09-21_0002.xmp") != files.end());
  REQUIRE(std::find(files.begin(), files.end(), "2026/2026-09-22/2026-09-22_0001.MP4") != files.end());
  // A new file the same day continues the count.
  write_bytes(r.card() / "DCIM/100CANON/IMG_0006.JPG", pattern(77777, 60));
  r.dates["IMG_0006.JPG"] = kSat;
  auto [pid2, pl2] = r.plan(r.card(), r.preset(R"(,"rename":"{date}_{seq}")"));
  r.run(pid2);
  const auto more = list_tree(r.dest());
  REQUIRE(std::find(more.begin(), more.end(), "2026/2026-09-21/2026-09-21_0005.JPG") != more.end());
}

TEST_CASE("library-wide duplicate scope skips files kept in other folders", "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset());
  r.run(pid);
  const std::string other = json_path(r.dir / "Other");
  const std::string to_other = R"({"name":"T2","destination":")" + other + R"(","eject_after":false,"selection":"all")";
  auto [p1, a] = r.plan(r.card(), to_other + R"(,"scope":"library"})");
  REQUIRE(totals(a, "selected_units") == 0);
  auto [p2, b] = r.plan(r.card(), to_other + "}");
  REQUIRE(totals(b, "selected_units") == 5);
}

TEST_CASE("verify-a-folder flags a one-bit flip and passes the rest", "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset());
  r.run(pid);
  const fs::path victim = r.dest() / "2026/2026-09-21/IMG_0002.JPG";
  const auto mtime = fs::last_write_time(victim);
  auto bytes = read_bytes(victim);
  bytes[4242] ^= 0x01;
  write_bytes(victim, bytes);
  fs::last_write_time(victim, mtime);  // silent corruption: same size, same time

  auto job = r.eng->verify_folder(utf8(r.dest()));
  REQUIRE(job);
  r.eng->wait_idle();
  const auto s = r.summary(*job);
  REQUIRE(*s.integer("checked") == 9);
  REQUIRE(*s.integer("ok") == 8);
  REQUIRE(s.find("problems")->a.size() == 1);
  REQUIRE(*s.find("problems")->a[0].str("problem") == "corrupt");
  REQUIRE(s.find("problems")->a[0].str("path")->find("IMG_0002.JPG") != std::string::npos);

  const auto history = mv::json::parse(r.eng->history_json());
  REQUIRE(history);
  REQUIRE(history->a.size() == 2);  // the import and the verify
}

TEST_CASE("auto-import fires only for the card it was enabled on", "[import][engine]") {
  rig r;
  r.make_card();
  REQUIRE(r.eng->save_preset(r.preset()));
  const auto arrival = [&](const fs::path& root) {
    {
      std::lock_guard lock(r.events_m);
      r.events.clear();
    }
    r.eng->on_volume(0, utf8(root));
    r.eng->wait_idle();
    std::lock_guard lock(r.events_m);
    const auto e = std::find_if(r.events.begin(), r.events.end(), [](const mv_addon_event& x) {
      return x.kind == MV_ADDON_EVENT_VOLUME_ARRIVED;
    });
    REQUIRE(e != r.events.end());
    return *e;
  };
  const auto plain = arrival(r.card());
  REQUIRE(plain.payload == MV_IMPORT_ARRIVAL_OPEN);
  REQUIRE(list_tree(r.dest()).empty());

  REQUIRE(r.eng->bind_card("uuid:CARD-A", "Test", true));
  const auto bound = arrival(r.card());
  REQUIRE(bound.payload == MV_IMPORT_ARRIVAL_AUTO);
  REQUIRE(r.progress(bound.id).state == MV_IMPORT_JOB_DONE);
  REQUIRE(list_tree(r.dest()).size() == 9);
  REQUIRE(list_tree(r.card()).size() == 10);  // and never deletes

  // Another card, not bound: the window opens, nothing imports.
  write_bytes(r.dir / "SD2/DCIM/100MSDCF/DSC00001.JPG", pattern(1000, 70));
  r.wrap.cards[utf8(r.dir / "SD2")] = "uuid:CARD-B";
  const auto other = arrival(r.dir / "SD2");
  REQUIRE(other.payload == MV_IMPORT_ARRIVAL_OPEN);
}

TEST_CASE("the viewer's marks import their whole units", "[import][engine]") {
  rig r;
  r.make_card();
  const std::string marks =
      "[\"" + json_path(r.card() / "DCIM/100CANON/IMG_0001.JPG") + "\"]";
  // Ctrl+Shift+F7: the last preset.
  REQUIRE(r.eng->save_preset(r.preset()));
  auto scan = r.eng->scan_files({utf8(r.card() / "DCIM/100CANON/IMG_0001.JPG")});
  REQUIRE(scan);
  const std::string p = r.preset();
  auto plan = r.eng->plan(*scan, &p, nullptr);
  REQUIRE(plan);
  r.eng->wait_idle();
  const auto v = r.plan_of(*plan);
  REQUIRE(totals(v, "selected_units") == 1);
  REQUIRE(totals(v, "selected_files") == 3);  // the JPEG, its RAW, its .xmp
  (void)marks;
}

TEST_CASE("selection changes rename on the control thread and post PLAN_READY",
          "[import][engine]") {
  rig r;
  r.make_card();
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"rename":"{date}_{seq}")"));
  const std::string day = "2026-09-21";
  REQUIRE(r.eng->select(pid, -1, &day, false));
  r.eng->wait_idle();
  auto v = r.plan_of(pid);
  REQUIRE(totals(v, "selected_units") == 1);  // only Sunday's clip
  REQUIRE(r.eng->select(pid, 0, nullptr, true));
  r.eng->wait_idle();
  v = r.plan_of(pid);
  REQUIRE(totals(v, "selected_units") == 2);
  const auto& first = v.find("units")->a[0];
  REQUIRE(first.find("names")->a[0].s.rfind("2026-09-21_0001", 0) == 0);
}

// ---- find duplicates (PR 54) ------------------------------------------------

namespace {

// A tree with one three-way group under different names and folders, a pair
// of same-size files with different bytes, empty files, and a lone file.
fs::path make_dup_tree(rig& r) {
  const fs::path t = r.dir / "Library";
  write_bytes(t / "2024/IMG_0001.JPG", pattern(250000, 11));
  write_bytes(t / "Backup/old copy.jpg", pattern(250000, 11));
  write_bytes(t / "Desktop/export/renamed.bin", pattern(250000, 11));
  write_bytes(t / "2024/IMG_0002.JPG", pattern(250000, 12));  // same size, other bytes
  write_bytes(t / "2025/clip.mov", pattern(400000, 13));
  write_bytes(t / "2025/clip (1).mov", pattern(400000, 13));
  write_bytes(t / "2025/unique.txt", pattern(1234, 14));
  write_bytes(t / "empty-a", {});
  write_bytes(t / "empty-b", {});
  write_bytes(t / ".hidden/IMG_0001.JPG", pattern(250000, 11));  // hidden: never walked
  for (const auto& e : fs::recursive_directory_iterator(t)) {
    if (e.is_regular_file()) set_mtime(e.path(), kSat);
  }
  return t;
}

std::uint64_t find_dups(rig& r, const fs::path& dir) {
  auto job = r.eng->find_duplicates(utf8(dir));
  REQUIRE(job);
  r.eng->wait_idle();
  return *job;
}

std::vector<std::string> group_names(const mv::json::value& g) {
  std::vector<std::string> out;
  for (const auto& f : g.find("files")->a) out.push_back(utf8(from_utf8(*f.str("path")).filename()));
  return out;
}

const mv::json::value* file_in(const mv::json::value& s, const fs::path& p) {
  for (const auto& g : s.find("groups")->a) {
    for (const auto& f : g.find("files")->a) {
      if (from_utf8(*f.str("path")) == p) return &f;  // either separator
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("find duplicates groups identical bytes, never names", "[import][engine][duplicates]") {
  rig r;
  const fs::path t = make_dup_tree(r);
  const auto job = find_dups(r, t);
  REQUIRE(r.progress(job).state == MV_IMPORT_JOB_DONE);
  const auto s = r.summary(job);
  REQUIRE(*s.str("kind") == "duplicates");
  REQUIRE(*s.integer("files") == 7);      // empty and hidden files aside
  REQUIRE(*s.integer("compared") == 6);   // unique.txt shares no size
  REQUIRE(*s.integer("hashed") == 6);
  const auto& groups = s.find("groups")->a;
  REQUIRE(groups.size() == 2);
  // Largest waste first: three 250 kB copies (500 kB) before two 400 kB (400 kB).
  REQUIRE(*groups[0].integer("size") == 250000);
  REQUIRE(group_names(groups[0]) ==
          std::vector<std::string>{"IMG_0001.JPG", "old copy.jpg", "renamed.bin"});
  REQUIRE(group_names(groups[1]) == std::vector<std::string>{"clip (1).mov", "clip.mov"});
  REQUIRE(*s.integer("duplicate_files") == 3);
  REQUIRE(*s.integer("wasted_bytes") == 2 * 250000 + 400000);
  REQUIRE(*s.boolean("can_trash"));
  // Same size, different bytes: not a duplicate.
  REQUIRE(!file_in(s, t / "2024/IMG_0002.JPG"));
  // A scan removes nothing.
  REQUIRE(list_tree(t).size() == 10);

  // History keeps the counts, not every path.
  const auto history = mv::json::parse(r.eng->history_json());
  REQUIRE(history);
  REQUIRE(*history->a[0].str("kind") == "duplicates");
  REQUIRE(*history->a[0].find("summary")->integer("group_count") == 2);
  REQUIRE(!history->a[0].find("summary")->find("groups"));
}

TEST_CASE("a second duplicate scan reads only what changed", "[import][engine][duplicates]") {
  rig r;
  const fs::path t = make_dup_tree(r);
  (void)find_dups(r, t);
  REQUIRE(r.wrap.hashes == 6);

  r.restart();  // remembered in import.db, not in memory
  const auto again = find_dups(r, t);
  REQUIRE(r.wrap.hashes == 6);
  REQUIRE(*r.summary(again).integer("hashed") == 0);
  REQUIRE(r.summary(again).find("groups")->a.size() == 2);

  // One copy edited: only it is read again, and it leaves its group.
  write_bytes(t / "Backup/old copy.jpg", pattern(250000, 99));
  set_mtime(t / "Backup/old copy.jpg", kSun);
  const auto third = find_dups(r, t);
  REQUIRE(r.wrap.hashes == 7);
  const auto s = r.summary(third);
  REQUIRE(group_names(s.find("groups")->a[1]) ==
          std::vector<std::string>{"IMG_0001.JPG", "renamed.bin"});
}

TEST_CASE("trashing a duplicate never empties its group", "[import][engine][duplicates]") {
  rig r;
  const fs::path t = make_dup_tree(r);
  const auto job = find_dups(r, t);
  const fs::path a = t / "2024/IMG_0001.JPG";
  const fs::path b = t / "Backup/old copy.jpg";
  const fs::path c = t / "Desktop/export/renamed.bin";

  // All three asked for: two go to the bin, the last is refused.
  REQUIRE(r.eng->trash_duplicate(job, utf8(b)));
  REQUIRE(r.eng->trash_duplicate(job, utf8(c)));
  REQUIRE(r.eng->trash_duplicate(job, utf8(a)));
  r.eng->wait_idle();
  const auto s = r.summary(job);
  REQUIRE(*file_in(s, b)->str("state") == "trashed");
  REQUIRE(*file_in(s, c)->str("state") == "trashed");
  REQUIRE(*file_in(s, a)->str("state") == "refused");
  REQUIRE(*file_in(s, a)->str("reason") == "last_copy");
  REQUIRE(fs::exists(a));
  REQUIRE(!fs::exists(b));
  REQUIRE(!fs::exists(c));
  REQUIRE(r.binned == 2);
  REQUIRE(*s.integer("wasted_bytes") == 400000);  // only the clip pair left
  // Not a file of this job: refused at once.
  REQUIRE(!r.eng->trash_duplicate(job, utf8(t / "2025/unique.txt")));
}

TEST_CASE("trash re-checks both copies before it moves anything", "[import][engine][duplicates]") {
  rig r;
  const fs::path t = make_dup_tree(r);
  const auto job = find_dups(r, t);
  const fs::path clip = t / "2025/clip.mov";
  const fs::path copy = t / "2025/clip (1).mov";

  // The other copy rotted since the scan (same size and time, other bytes):
  // it is no longer a copy, so this one stays.
  auto bytes = read_bytes(copy);
  bytes[777] ^= 0x01;
  write_bytes(copy, bytes);
  set_mtime(copy, kSat);
  REQUIRE(r.eng->trash_duplicate(job, utf8(clip)));
  r.eng->wait_idle();
  REQUIRE(*file_in(r.summary(job), clip)->str("reason") == "last_copy");
  REQUIRE(fs::exists(clip));

  // Edited since the scan: a different file now, not a duplicate.
  set_mtime(copy, kSun);
  REQUIRE(r.eng->trash_duplicate(job, utf8(copy)));
  r.eng->wait_idle();
  REQUIRE(*file_in(r.summary(job), copy)->str("reason") == "changed");
  REQUIRE(fs::exists(copy));
  REQUIRE(r.binned == 0);
}

TEST_CASE("no bin, no delete", "[import][engine][duplicates]") {
  rig r;
  const fs::path t = make_dup_tree(r);
  const fs::path b = t / "Backup/old copy.jpg";

  // A location without a bin (a network share): refused, the file stays.
  r.bin_refuses = true;
  const auto job = find_dups(r, t);
  REQUIRE(r.eng->trash_duplicate(job, utf8(b)));
  r.eng->wait_idle();
  REQUIRE(*file_in(r.summary(job), b)->str("reason") == "no_bin");
  REQUIRE(fs::exists(b));

  // An app without the bin call (an older host table): no delete offered.
  r.api.recycle_file = nullptr;
  r.restart();
  const auto old = find_dups(r, t);
  REQUIRE(!*r.summary(old).boolean("can_trash"));
  const auto refused = r.eng->trash_duplicate(old, utf8(b));
  REQUIRE(!refused);
  REQUIRE(refused.error() == mv::status::unsupported_format);
  REQUIRE(fs::exists(b));
}

// Timing, not part of the normal run:
//
//     mv_import_tests "[.perf-bench]"
//
// A synthetic tree (2,000 files of 100-600 kB, 400 of them copied again
// under other names), or MV_DUP_BENCH_DIR=folder for a real one: the folder
// is only read, and the hash cache lives in the rig's scratch import.db,
// removed with it. Warm page cache either way: the first scan's figure is
// hashing speed, not the disk's.
TEST_CASE("perf: find duplicates, first scan and rescan", "[.perf-bench]") {
  rig r;
  fs::path tree;
  if (const char* env = std::getenv("MV_DUP_BENCH_DIR")) {
    tree = from_utf8(env);
  } else {
    tree = r.dir / "Bench";
    std::uint32_t seed = 7;
    for (int i = 0; i < 2000; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const std::size_t size = 100000 + (seed >> 8) % 500000;
      const auto bytes = pattern(size, static_cast<std::uint32_t>(i) + 1000);
      write_bytes(tree / ("DCIM/" + std::to_string(i / 200)) / ("IMG_" + std::to_string(i) + ".JPG"), bytes);
      if (i % 5 == 0) write_bytes(tree / "Backup" / ("copy of " + std::to_string(i) + ".jpg"), bytes);
    }
  }
  const auto timed = [&] {
    const auto t0 = std::chrono::steady_clock::now();
    const auto job = find_dups(r, tree);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return std::pair{job, ms};
  };
  const auto [first, first_ms] = timed();
  const auto s1 = r.summary(first);
  const int hashes_first = r.wrap.hashes.load();
  const auto [second, second_ms] = timed();
  const auto s2 = r.summary(second);
  std::printf("find duplicates: %lld files, %lld MB, %lld compared, %lld groups\n",
              static_cast<long long>(*s1.integer("files")),
              static_cast<long long>(*s1.integer("bytes") / 1000000),
              static_cast<long long>(*s1.integer("compared")),
              static_cast<long long>(*s1.integer("group_count")));
  std::printf("  first scan  %8.1f ms  (%d files hashed)\n", first_ms, hashes_first);
  std::printf("  rescan      %8.1f ms  (%lld files hashed)\n", second_ms,
              static_cast<long long>(*s2.integer("hashed")));
  REQUIRE(*s2.integer("hashed") == 0);
  REQUIRE(*s2.integer("group_count") == *s1.integer("group_count"));
}
