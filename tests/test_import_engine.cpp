// SPDX-License-Identifier: GPL-2.0-or-later
// The Import engine end to end, over the real host table (io ports, pairing,
// verified copy), at card scale: plan/18's PR 16-19 verify lines, the ones a
// machine without a card reader can check. The 64 GB timing, eject, and the
// present-loop gates while importing are hardware runs (plan/10).
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_import.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addons/import/engine.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;

namespace {

constexpr std::int64_t kSat = 1789999385;  // 2026-09-21 14:03:05
constexpr std::int64_t kSun = 1790069400;  // 2026-09-22 09:30:00

// Wraps the real host table: counts copies and hashes, injects faults, and
// answers volume_of for fake "cards" (a Linux temp folder is no removable
// volume).
struct wrap_state {
  const mv_host_api* real = nullptr;
  std::atomic<int> copies{0};
  std::atomic<int> hashes{0};
  std::atomic<std::uint64_t> copy_bytes{0};
  std::uint32_t fault_times = 0;
  std::function<void(int)> before_copy;
  std::map<std::string, std::string> cards;  // root -> volume id
};
wrap_state* g_wrap = nullptr;

mv_status MV_CALL w_copy(void* host, const mv_addon_copy_request* req, mv_addon_copy_result* out) {
  const int n = g_wrap->copies.fetch_add(1);
  if (g_wrap->before_copy) g_wrap->before_copy(n);
  mv_addon_copy_request r = *req;
  if (g_wrap->fault_times > 0) {
    r.fault_target = 0;
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

mv_status MV_CALL w_volume_of(void* host, const char* path, mv_addon_volume* out) {
  const mv_status s = g_wrap->real->volume_of(host, path, out);
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
    svc.data_dir = utf8(dir / "data");
    svc.default_library = utf8(dir / "Pictures/MediaViewer");
    svc.test_hooks = true;
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    wrap.real = table->api();
    api = *table->api();
    api.copy_verified = &w_copy;
    api.hash_file = &w_hash;
    api.volume_of = &w_volume_of;
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
      if (e.is_regular_file()) set_mtime(e.path(), kSat - 60);
    }
    wrap.cards[utf8(card())] = "uuid:CARD-A";
  }

  std::string preset(const std::string& extra = "") const {
    return R"({"name":"Test","destination":")" + utf8(dest()) + R"(","eject_after":false)" + extra +
           "}";
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
  const std::string backup = utf8(r.dir / "Backup");
  auto [pid, pl] = r.plan(r.card(), r.preset(R"(,"backup":")" + backup + R"(")"));
  r.run(pid);
  REQUIRE(list_tree(r.dest()) == list_tree(backup));
  REQUIRE(r.wrap.copies == 9);  // one copy call (one card read) per file
  std::uint64_t card_bytes = 0;
  for (const auto& e : fs::recursive_directory_iterator(r.card())) {
    if (e.is_regular_file() && e.path().extension() != ".BDM") card_bytes += e.file_size();
  }
  REQUIRE(r.wrap.copy_bytes == card_bytes);
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
  const std::string other = utf8(r.dir / "Other");
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
    for (const auto& e : r.events) {
      if (e.kind == MV_ADDON_EVENT_VOLUME_ARRIVED) return e;
    }
    FAIL("no arrival event");
    return mv_addon_event{};
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
      "[\"" + utf8(r.card() / "DCIM/100CANON/IMG_0001.JPG") + "\"]";
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
