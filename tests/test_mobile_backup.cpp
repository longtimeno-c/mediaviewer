// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup, 1.1 (docs/plans/mobile.md §10b, M11), over loopback against a
// real `driftwire serve` and against the sample library's on-phone folder. M11's
// verify line, as far as loopback can show it, at a smaller scale: mixed photos,
// Live Photos, RAWs and videos backed up while the run is stopped at random
// points; every file on the "NAS" matches its hash, none is duplicated, and the
// count equals the files there; delete some, exactly those are copied again;
// "reinstall" (an empty ledger), nothing is copied again; the scratch space never
// holds more than one file; a write past the storage limit is refused.
// Filter with "[mobile-link]"; nothing here leaves 127.0.0.1.
#include <sodium.h>

#include "catch_compat.h"

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "wire/grants.h"
#include "wire/known.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "wire/serve_backup.h"
#include "wire/net.h"
#include "mobile/backup.h"
#include "mobile/link.h"
#include "emulated_path.h"

namespace fs = std::filesystem;
namespace mob = mv::mobile;
namespace bk = mv::mobile::backup;
namespace serve = mv::driftwire::serve;
namespace dw = mv::driftwire;

namespace {

using key32 = std::array<std::uint8_t, 32>;

key32 random_key() {
  key32 k;
  randombytes_buf(k.data(), k.size());
  return k;
}

void write_file(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct temp_root {
  fs::path root;
  temp_root() {
    root = fs::temp_directory_path() / ("mv_backup_" + std::to_string(randombytes_random()));
    fs::create_directories(root);
  }
  ~temp_root() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

const std::string kBackupRoot = "b0b0b0b0b0b0b0b0";
const std::string kSharedRoot = "a1a1a1a1a1a1a1a1";  // an ordinary shared folder (changing files)
const std::string kLibrary = "library-1";

// A "camera roll": files on disk standing in for PhotoKit's resources.
struct camera_roll final : bk::source {
  fs::path dir;
  fs::path scratch;
  std::map<std::string, std::string> files;  // key/index -> bytes
  std::uint64_t largest = 0;
  std::atomic<std::uint64_t> peak_scratch{0};
  std::vector<bk::item> items;
  bool date_from_file = false;  // the file says it was taken on 31 December 2019

  camera_roll(fs::path d, fs::path s, int count, std::uint32_t seed) : dir(std::move(d)), scratch(std::move(s)) {
    std::mt19937 rng(seed);
    for (int i = 0; i < count; ++i) {
      bk::item it;
      it.key = (i % 3 == 0) ? "local:" + std::to_string(i) : "cloud-" + std::to_string(i);
      it.local_id = std::to_string(i);
      it.year = 2020 + i % 7;
      it.month = 1 + i % 12;
      it.day = 1 + i % 28;
      const int kind = i % 4;  // photo, Live Photo, RAW + JPEG, video
      const std::string base = "IMG_" + std::to_string(1000 + i);
      it.resources.push_back({kind == 3 ? "video" : "photo", base + (kind == 3 ? ".MOV" : ".HEIC")});
      if (kind == 1) it.resources.push_back({"live_video", base + ".MOV"});
      if (kind == 2) it.resources.push_back({"raw", base + ".DNG"});
      for (std::size_t r = 0; r < it.resources.size(); ++r) {
        const std::size_t size = 1000 + rng() % (kind == 3 ? 900000 : 200000);
        std::string bytes(size, '\0');
        for (auto& c : bytes) c = static_cast<char>(rng());
        largest = std::max<std::uint64_t>(largest, size);
        files[it.key + "/" + std::to_string(r)] = std::move(bytes);
      }
      items.push_back(std::move(it));
    }
  }

  mv::expected export_resource(const bk::item& it, std::size_t index, const std::string& path,
                               const mob::cancel_flag* cancel, int ymd[3]) override {
    if (date_from_file) {
      ymd[0] = 2019;
      ymd[1] = 12;
      ymd[2] = 31;
    }
    if (cancel && cancel->stopped()) return mv::err(mv::status::cancelled);
    const auto f = files.find(it.key + "/" + std::to_string(index));
    if (f == files.end()) return mv::err(mv::status::not_found);
    write_file(path, f->second);
    // Peak extra storage: everything in the scratch folder now.
    std::uint64_t now = 0;
    for (const auto& e : fs::recursive_directory_iterator(scratch)) {
      if (e.is_regular_file()) now += e.file_size();
    }
    if (now > peak_scratch) peak_scratch = now;
    return {};
  }

  [[nodiscard]] std::size_t resource_count() const { return files.size(); }
};

// The service, with one backup folder.
struct nas {
  temp_root tmp;
  fs::path data = tmp.root / "data", backup = tmp.root / "backup";
  std::unique_ptr<dw::secrets::store> store;
  key32 access{}, identity{};
  std::unique_ptr<serve::server> srv;
  std::uint64_t limit = 1ull << 34;
  std::function<bool(const key32&)> may_write;
  fs::path shared = tmp.root / "shared";
  bool with_shared = false;  // a second, ordinary folder: "Photos"

  nas() {
    REQUIRE(sodium_init() >= 0);
    fs::create_directories(data);
    fs::create_directories(backup);
    REQUIRE(dw::secrets::owner_only_folder((data / "secrets").string()));
    store = dw::secrets::file_store((data / "secrets").string());
    REQUIRE(store);
    bool made = false;
    REQUIRE(serve::set_up(*store, identity, access, made));
  }
  void start() {
    srv.reset();
    serve::server::options o;
    o.store = store.get();
    o.loopback_only = true;
    o.per_minute = 10000;
    o.per_source_pending = 8;
    o.max_pending = 32;
    o.cfg.port = 0;
    o.cfg.name = "NAS";
    serve::root_config b{kBackupRoot, "Phone backup", backup.string()};
    b.backup = true;
    b.limit = limit;
    b.reserve = 0;
    o.cfg.roots.push_back(b);
    if (with_shared) {
      fs::create_directories(shared);
      o.cfg.roots.push_back(serve::root_config{kSharedRoot, "Photos", shared.string()});
    }
    o.free_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
    o.may_write = may_write;
    o.make_listener = [] { auto l = dw::net::listen_internet(0, true, 32); REQUIRE(l); return std::move(*l); };
    auto s = serve::server::start(o);
    REQUIRE(s);
    srv = std::move(*s);
  }
  void pair(const key32& device) {
    key32 pub;
    crypto_scalarmult_base(pub.data(), device.data());
    auto devices = dw::known::load(*store);
    devices.push_back({"Phone", pub});
    REQUIRE(dw::known::save(*store, devices));
  }
  mob::service_config config(const key32& device) const {
    mob::service_config c;
    c.local = "127.0.0.1:" + std::to_string(srv->port());
    c.public_name = "localhost:" + std::to_string(srv->port());
    c.identity = identity;
    c.access_key = access;
    c.device_key = device;
    return c;
  }
};

// Every backed-up file under `root` (no meta folder, no unfinished copies).
std::map<std::string, std::string> files_under(const fs::path& root) {
  std::map<std::string, std::string> out;
  for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
    const std::string name = it->path().filename().string();
    if (it->is_directory() && name == serve::kBackupMetaFolder) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file()) continue;
    out[fs::relative(it->path(), root).generic_string()] = read_all(it->path());
  }
  return out;
}

bk::options options_for(const fs::path& scratch, const std::string& root = kBackupRoot) {
  bk::options o;
  o.library_id = kLibrary;
  o.root = root;
  o.scratch_dir = scratch.string();
  o.ledger_copy_every = 5;
  return o;
}

// One run, stopped when `stop_after` files have been reported done (0: never).
bk::status_report run_once(bk::target& t, camera_roll& roll, bk::ledger& l, const fs::path& scratch, int stop_after,
                           double stop_fraction = 2.0, const std::string& root = kBackupRoot,
                           const std::function<void(bk::options&)>& tune = {}) {
  auto o = options_for(scratch, root);
  if (tune) tune(o);
  bk::engine e(o, t, roll, l);
  e.set_items(roll.items);
  mob::cancel_flag cancel;
  bk::status_report last;
  (void)e.run(&cancel, [&](const bk::status_report& s) {
    last = s;
    if (stop_after > 0 && s.copied >= static_cast<std::uint64_t>(stop_after) && s.current_fraction >= stop_fraction) {
      cancel.stop = true;
    }
    if (stop_after > 0 && s.copied >= static_cast<std::uint64_t>(stop_after) && stop_fraction > 1.0) cancel.stop = true;
  });
  return last;
}

}  // namespace

TEST_CASE("backup: the layouts file by the photo's own date", "[mobile-link]") {
  CHECK(bk::subdir(2026, 9, 21, bk::layout::year_month) == "2026/09");
  CHECK(bk::subdir(2026, 9, 21, bk::layout::year) == "2026");
  CHECK(bk::subdir(2026, 9, 21, bk::layout::year_month_day) == "2026/09/21");
  CHECK(bk::subdir(2026, 9, 21, bk::layout::flat).empty());
  CHECK(bk::valid_layout(5));
  CHECK_FALSE(bk::valid_layout(0));  // the desktop's year_day is not a phone layout
}

TEST_CASE("backup: stopped at random points, every file arrives once and whole; deletes and reinstalls",
          "[mobile-link]") {
  nas n;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 40, 1234);
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);

  // Killed again and again at random points, each time with a new link (a new launch).
  std::mt19937 rng(99);
  bk::status_report last;
  for (int launch = 0; launch < 60; ++launch) {
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    const int stop_after = 1 + static_cast<int>(rng() % 6);
    const double fraction = (rng() % 2) ? 0.5 : 2.0;  // sometimes in the middle of a file
    last = run_once(*target, roll, **ledger, scratch, stop_after, fraction);
    if (last.now == bk::state::done) break;
    REQUIRE(last.now == bk::state::idle);
  }
  REQUIRE(last.now == bk::state::done);
  CHECK(last.backed_up == roll.items.size());

  auto on_nas = files_under(n.backup);
  // Every file there once, under its own name, matching what the phone has.
  CHECK(on_nas.size() == roll.resource_count());
  std::set<std::string> contents;
  for (const auto& [rel, bytes] : on_nas) {
    CHECK(rel.find(" (") == std::string::npos);
    CHECK(rel.find(".mvpart") == std::string::npos);
    contents.insert(bytes);
  }
  for (const auto& [k, bytes] : roll.files) CHECK(contents.count(bytes) == 1);
  CHECK((*ledger)->size(kBackupRoot) == on_nas.size());
  // Peak extra storage on the phone: one file, and the others sending within parallel_bytes.
  CHECK(roll.peak_scratch <= roll.largest + static_cast<std::uint64_t>(bk::options{}.parallel_bytes));

  // Delete 5 from the NAS: exactly those are copied again.
  int removed = 0;
  for (const auto& [rel, bytes] : on_nas) {
    if (removed == 5) break;
    fs::remove(n.backup / rel);
    ++removed;
  }
  {
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    last = run_once(*target, roll, **ledger, scratch, 0);
  }
  CHECK(last.now == bk::state::done);
  CHECK(last.copied == 5);
  CHECK(files_under(n.backup) == on_nas);

  // Reinstall: an empty ledger on the phone, the same device key... and a new phone's key too.
  for (const bool new_phone : {false, true}) {
    ledger->reset();
    fs::remove(phone.root / "ledger.db");
    fs::remove(phone.root / "ledger.db-wal");
    fs::remove(phone.root / "ledger.db-shm");
    ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    REQUIRE((*ledger)->size(kBackupRoot) == 0);
    const key32 key = new_phone ? random_key() : device;
    if (new_phone) n.pair(key);
    mob::link_session link(n.config(key));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    last = run_once(*target, roll, **ledger, scratch, 0);
    CHECK(last.now == bk::state::done);
    CHECK(last.copied == 0);
    CHECK(last.backed_up == roll.items.size());
  }
  CHECK(files_under(n.backup) == on_nas);
}

TEST_CASE("backup: the storage limit, a folder this phone may not write, and a different photo of the same name",
          "[mobile-link]") {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  const key32 device = random_key();

  SECTION("past the limit the run stops, and says so") {
    nas n;
    n.limit = 600 * 1024;
    n.start();
    n.pair(device);
    camera_roll roll(phone.root / "roll", scratch, 12, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    const auto last = run_once(*target, roll, **ledger, scratch, 0);
    CHECK(last.now == bk::state::limit);
    CHECK(last.backed_up < roll.items.size());
    std::uint64_t total = 0;
    for (const auto& [rel, bytes] : files_under(n.backup)) total += bytes.size();
    CHECK(total <= 600 * 1024);
  }
  SECTION("a phone the service does not let write") {
    nas n;
    n.may_write = [](const key32&) { return false; };
    n.start();
    n.pair(device);
    camera_roll roll(phone.root / "roll", scratch, 3, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    CHECK(run_once(*target, roll, **ledger, scratch, 0).now == bk::state::not_allowed);
    CHECK(files_under(n.backup).empty());
  }
  SECTION("same name, different photo: kept beside it, never over it") {
    nas n;
    n.start();
    n.pair(device);
    camera_roll roll(phone.root / "roll", scratch, 1, 7);
    const auto& it = roll.items.front();
    const std::string rel = bk::subdir(it.year, it.month, it.day, bk::layout::year_month) + "/" + it.resources[0].name;
    write_file(n.backup / rel, "someone else's photo");
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    CHECK(run_once(*target, roll, **ledger, scratch, 0).now == bk::state::done);
    CHECK(read_all(n.backup / rel) == "someone else's photo");
    const auto row = (*ledger)->find(kBackupRoot, it.key, it.resources[0].kind, it.resources[0].name);
    REQUIRE(row);
    CHECK(row->rel.find(" (2)") != std::string::npos);
  }
}

TEST_CASE("backup: the folders offered follow the server while the phone is connected", "[mobile-link]") {
  const key32 device = random_key();
  key32 pub;
  crypto_scalarmult_base(pub.data(), device.data());
  nas n;
  n.with_shared = true;
  n.start();
  const auto ids = [](const std::vector<bk::folder>& fs) {
    std::vector<std::string> out;
    for (const auto& f : fs) out.push_back(f.id + (f.writable ? "+w" : ""));
    return out;
  };
  const auto cfg_with = [&](bool backup) {
    serve::config c;
    c.name = "NAS";
    for (const auto& r : n.srv->roots()) c.roots.push_back(r);
    for (auto& r : c.roots) {
      if (r.id == kBackupRoot) r.backup = backup;
    }
    return c;
  };

  SECTION("a folder made a backup folder after the phone connected") {
    n.pair(device);
    REQUIRE(n.srv->replace_config(cfg_with(false)));
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    auto before = target->folders();
    REQUIRE(before);
    CHECK(before->empty());
    REQUIRE(n.srv->replace_config(cfg_with(true)));
    auto after = target->folders();
    REQUIRE(after);
    CHECK(ids(*after) == std::vector<std::string>{kBackupRoot + "+w"});
  }
  SECTION("a phone limited to other folders, with backup on") {
    serve::grants::grant g;
    g.name = "Phone";
    g.key = pub;
    g.roots = std::vector<std::string>{kSharedRoot};
    g.backup = true;
    REQUIRE(serve::grants::save(*n.store, {g}));
    n.srv->recheck_devices();
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    // The service tells it only its own folders: none, and the phone knows why.
    auto folders = target->folders();
    REQUIRE(folders);
    CHECK(folders->empty());
    CHECK(target->limited_to_some_folders());
    // A run into the backup folder it chose before it was limited: "choose another" (whose list
    // then says none may be used), never "can't reach".
    temp_root phone;
    const fs::path scratch = phone.root / "scratch";
    fs::create_directories(scratch);
    camera_roll roll(phone.root / "roll", scratch, 2, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    CHECK(run_once(*target, roll, **ledger, scratch, 0).now == bk::state::no_folder);
    CHECK(files_under(n.backup).empty());
  }
  SECTION("a phone whose pairing ended is never told \"not allowed\"") {
    n.pair(device);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    REQUIRE(target->folders());
    REQUIRE(dw::known::save(*n.store, {}));  // removed on the desktop
    n.srv->recheck_devices();
    CHECK_FALSE(target->limited_to_some_folders());
    temp_root phone;
    const fs::path scratch = phone.root / "scratch";
    fs::create_directories(scratch);
    camera_roll roll(phone.root / "roll", scratch, 2, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    const auto now = run_once(*target, roll, **ledger, scratch, 0).now;
    CHECK(now != bk::state::not_allowed);
    CHECK(now != bk::state::no_folder);
    CHECK(files_under(n.backup).empty());
  }
  SECTION("a phone the owner has not let back up sees the folder, not writable") {
    serve::grants::grant g;
    g.name = "Phone";
    g.key = pub;
    g.backup = false;
    REQUIRE(serve::grants::save(*n.store, {g}));
    n.srv->recheck_devices();
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    auto folders = target->folders();
    REQUIRE(folders);
    CHECK(ids(*folders) == std::vector<std::string>{kBackupRoot});
  }
}

namespace {

// One run with Photos' deletions: the last report and the keys it finished with.
std::pair<bk::status_report, std::vector<std::string>> run_deleting(bk::target& t, camera_roll& roll, std::vector<bk::item> items,
                                                                     std::vector<std::string> deleted, bk::ledger& l,
                                                                     const fs::path& scratch, const std::string& root) {
  bk::engine e(options_for(scratch, root), t, roll, l);
  e.set_items(std::move(items));
  e.set_deleted(std::move(deleted));
  mob::cancel_flag cancel;
  bk::status_report last;
  (void)e.run(&cancel, [&](const bk::status_report& s) { last = s; });
  return {last, e.deleted_done()};
}

std::size_t trashed_under(const fs::path& root) {
  std::size_t n = 0;
  const fs::path trash = root / serve::kTrashFolder;
  if (!fs::exists(trash)) return 0;
  for (auto it = fs::recursive_directory_iterator(trash); it != fs::recursive_directory_iterator(); ++it) {
    if (it->is_regular_file()) ++n;
  }
  return n;
}

// The real target, but the folder's trash out of reach (offline, an older service).
struct no_trash final : bk::target {
  bk::target& t;
  explicit no_trash(bk::target& real) : t(real) {}
  mv::result<std::vector<bk::folder>> folders() override { return t.folders(); }
  mv::result<serve::stat_info> space(const std::string& r) override { return t.space(r); }
  mv::result<std::vector<serve::list_entry>> list(const std::string& r, const std::string& rel) override { return t.list(r, rel); }
  mv::result<serve::client::write_done> write(const std::string& r, const std::string& rel, std::uint64_t size,
                                              const std::string& hash, std::uint64_t at, const serve::client::read_fn& read,
                                              const std::function<void(std::uint64_t)>& progress, const mob::cancel_flag* cancel,
                                              serve::client::write_refused* why, bool meta) override {
    return t.write(r, rel, size, hash, at, read, progress, cancel, why, meta);
  }
  mv::result<std::vector<std::string>> ledger_ids(const std::string& r) override { return t.ledger_ids(r); }
  mv::result<std::string> read_ledger(const std::string& r, const std::string& id) override { return t.read_ledger(r, id); }
  std::string device_id() const override { return t.device_id(); }
  mv::result<bool> trash(const std::string&, const std::string&) override { return mv::err(mv::status::not_found); }
};

void check_deletions(bk::target& target, const fs::path& folder, const std::string& root) {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 6, 11);
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);
  REQUIRE(run_deleting(target, roll, roll.items, {}, **ledger, scratch, root).first.now == bk::state::done);
  const std::size_t all = files_under(folder).size();
  REQUIRE(all == roll.resource_count());

  const bk::item gone = roll.items[1];    // deleted in Photos: a Live Photo, two files
  const bk::item hidden = roll.items[2];  // hidden in Photos: left out of the items, not deleted
  const bk::item back = roll.items[4];    // deleted, then put back from Recently Deleted
  std::vector<bk::item> now;
  for (const auto& it : roll.items) {
    if (it.key != gone.key && it.key != hidden.key) now.push_back(it);
  }
  const std::vector<std::string> deleted{gone.key, back.key, "cloud-never-backed-up"};

  // The trash out of reach: nothing moves, nothing is forgotten, the key waits.
  no_trash blocked(target);
  auto [stuck, stuck_done] = run_deleting(blocked, roll, now, deleted, **ledger, scratch, root);
  CHECK(stuck.trashed == 0);
  CHECK(std::find(stuck_done.begin(), stuck_done.end(), gone.key) == stuck_done.end());
  CHECK((*ledger)->find(root, gone.key, gone.resources[0].kind, gone.resources[0].name));

  auto [report, done] = run_deleting(target, roll, now, deleted, **ledger, scratch, root);
  CHECK(report.now == bk::state::done);
  CHECK(report.trashed == gone.resources.size());
  std::sort(done.begin(), done.end());
  std::vector<std::string> want = deleted;
  std::sort(want.begin(), want.end());
  CHECK(done == want);
  // Moved, never deleted: in the trash, out of the folder and the ledger; the rest as they were.
  CHECK(trashed_under(folder) == gone.resources.size());
  CHECK(files_under(folder).size() == all);  // (files_under counts the trash too)
  for (const auto& r : gone.resources) CHECK_FALSE((*ledger)->find(root, gone.key, r.kind, r.name));
  for (const auto& r : hidden.resources) CHECK((*ledger)->find(root, hidden.key, r.kind, r.name));
  for (const auto& r : back.resources) CHECK((*ledger)->find(root, back.key, r.kind, r.name));

  // Asked again (the app had not heard it was done): nothing more moves.
  auto [again, again_done] = run_deleting(target, roll, now, deleted, **ledger, scratch, root);
  CHECK(again.trashed == 0);
  CHECK(again_done.size() == deleted.size());
  CHECK(trashed_under(folder) == gone.resources.size());
}

}  // namespace

TEST_CASE("backup: a photo deleted from Photos goes into the folder's trash, and nothing else does", "[mobile-link]") {
  SECTION("on the server") {
    nas n;
    n.start();
    const key32 device = random_key();
    n.pair(device);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    check_deletions(*target, n.backup, kBackupRoot);
  }
  SECTION("in the sample library's folder on the phone") {
    temp_root phone;
    const fs::path nas = phone.root / "sample-nas";
    auto target = bk::local_target(nas.string(), "0123456789abcdef", bk::kSampleLimit,
                                   [](const std::string&) { return std::uint64_t{1} << 40; });
    check_deletions(*target, nas, bk::kSampleFolderId);
  }
  SECTION("in a person's subfolder, on the server and on the phone") {
    nas n;
    n.start();
    const key32 device = random_key();
    n.pair(device);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    check_deletions(*target, n.backup, kBackupRoot + "/Family/Tristan");
    CHECK(fs::exists(n.backup / serve::kTrashFolder));  // the backup folder's own trash
    temp_root phone;
    const fs::path sample = phone.root / "sample-nas";
    auto local = bk::local_target(sample.string(), "0123456789abcdef", bk::kSampleLimit,
                                  [](const std::string&) { return std::uint64_t{1} << 40; });
    check_deletions(*local, sample, std::string(bk::kSampleFolderId) + "/Tristan");
  }
}

// The family NAS: one backup folder, a subfolder for each person's phone.
TEST_CASE("backup: each person backs up into their own subfolder of one backup folder", "[mobile-link]") {
  CHECK(bk::parse_destination(kBackupRoot)->sub.empty());
  CHECK(bk::parse_destination(kBackupRoot + "/iPhone/Tristan")->sub == "iPhone/Tristan");
  CHECK(bk::parse_destination(kBackupRoot + "/iPhone/Tristan")->root == kBackupRoot);
  for (const std::string& bad : std::vector<std::string>{"", "/Tristan", "/", kBackupRoot + "/", kBackupRoot + "/..", kBackupRoot + "/../x",
                                kBackupRoot + "/a/../../x", kBackupRoot + "/" + serve::kTrashFolder,
                                kBackupRoot + "/" + serve::kBackupMetaFolder + "/x", kBackupRoot + "/a//b"}) {
    CHECK_FALSE(bk::parse_destination(bad));
  }

  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  const key32 tristan = random_key(), sam = random_key();

  SECTION("on the server: made, listed, filled, each apart; the limit is the whole folder's") {
    nas n;
    n.start();
    n.pair(tristan);
    n.pair(sam);
    mob::link_session link_t(n.config(tristan)), link_s(n.config(sam));
    mob::route_report report;
    REQUIRE(link_t.connect(report));
    REQUIRE(link_s.connect(report));
    auto target_t = bk::link_target(link_t);
    auto target_s = bk::link_target(link_s);

    auto made = bk::make_subfolder(*target_t, kBackupRoot, "Tristan");
    REQUIRE(made);
    CHECK(*made == kBackupRoot + "/Tristan");
    CHECK(fs::is_directory(n.backup / "Tristan"));
    CHECK(bk::make_subfolder(*target_t, kBackupRoot, "Tristan"));  // there already is fine
    auto sams = bk::make_subfolder(*target_s, kBackupRoot, "Sam");
    REQUIRE(sams);
    for (const std::string bad : {"", "..", ".", ".hidden", "a/b", "a\\b", "trailing."}) {
      auto refused = bk::make_subfolder(*target_t, kBackupRoot, bad);
      REQUIRE_FALSE(refused);
      CHECK(refused.error() == mv::status::invalid_arg);
    }
    write_file(n.backup / "a file", "x");
    auto onto_file = bk::make_subfolder(*target_t, kBackupRoot, "a file");
    REQUIRE_FALSE(onto_file);
    CHECK(onto_file.error() == mv::status::invalid_arg);
    auto listed = bk::subfolders(*target_s, kBackupRoot);
    REQUIRE(listed);
    CHECK(*listed == std::vector<std::string>{"Sam", "Tristan"});  // no meta folder, no trash, no file
    auto deeper = bk::make_subfolder(*target_t, *made, "iPhone 16");
    REQUIRE(deeper);
    CHECK(*deeper == kBackupRoot + "/Tristan/iPhone 16");
    CHECK(*bk::subfolders(*target_t, *made) == std::vector<std::string>{"iPhone 16"});

    camera_roll roll_t(phone.root / "roll-t", scratch, 8, 3), roll_s(phone.root / "roll-s", scratch, 5, 4);
    auto ledger_t = bk::ledger::open((phone.root / "ledger-t.db").string());
    auto ledger_s = bk::ledger::open((phone.root / "ledger-s.db").string());
    REQUIRE(ledger_t);
    REQUIRE(ledger_s);
    CHECK(run_once(*target_t, roll_t, **ledger_t, scratch, 0, 2.0, *made).now == bk::state::done);
    CHECK(run_once(*target_s, roll_s, **ledger_s, scratch, 0, 2.0, *sams).now == bk::state::done);
    CHECK(files_under(n.backup / "Tristan").size() == roll_t.resource_count());
    CHECK(files_under(n.backup / "Sam").size() == roll_s.resource_count());
    for (const auto& r : (*ledger_t)->all(*made)) CHECK(r.rel.rfind("Tristan/", 0) == 0);
    // Rows are per destination: the folder itself, or another subfolder, is backed up afresh.
    CHECK((*ledger_t)->size(kBackupRoot) == 0);
    CHECK(run_once(*target_t, roll_t, **ledger_t, scratch, 0, 2.0, *deeper).now == bk::state::done);
    CHECK(files_under(n.backup / "Tristan" / "iPhone 16").size() == roll_t.resource_count());
    // A reinstall finds its rows in the backup folder's meta folder and copies nothing again.
    REQUIRE((*ledger_t)->clear());
    const auto again = run_once(*target_t, roll_t, **ledger_t, scratch, 0, 2.0, *deeper);
    CHECK(again.now == bk::state::done);
    CHECK(again.copied == 0);

    // A destination that would leave the folder is no folder; nothing is written anywhere.
    const auto before = files_under(n.tmp.root);
    CHECK(run_once(*target_t, roll_t, **ledger_t, scratch, 0, 2.0, kBackupRoot + "/../data").now == bk::state::no_folder);
    CHECK(run_once(*target_t, roll_t, **ledger_t, scratch, 0, 2.0, "ffffffffffffffff/Tristan").now == bk::state::no_folder);
    CHECK(files_under(n.tmp.root).size() == before.size());
  }
  SECTION("the backup folder's storage limit counts every person's subfolder") {
    nas n;
    n.limit = 600 * 1024;
    n.start();
    n.pair(tristan);
    mob::link_session link(n.config(tristan));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto target = bk::link_target(link);
    write_file(n.backup / "Sam" / "big.bin", std::string(400 * 1024, 's'));
    camera_roll roll(phone.root / "roll", scratch, 12, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    CHECK(run_once(*target, roll, **ledger, scratch, 0, 2.0, kBackupRoot + "/Tristan").now == bk::state::limit);
    std::uint64_t total = 0;
    for (const auto& [rel, bytes] : files_under(n.backup)) total += bytes.size();
    CHECK(total <= 600 * 1024);
  }
  SECTION("in the sample library's folder on the phone (the demo)") {
    auto target = bk::local_target((phone.root / "sample-nas").string(), "0123456789abcdef", bk::kSampleLimit,
                                   [](const std::string&) { return std::uint64_t{1} << 40; });
    auto made = bk::make_subfolder(*target, bk::kSampleFolderId, "Tristan");
    REQUIRE(made);
    CHECK(*bk::subfolders(*target, bk::kSampleFolderId) == std::vector<std::string>{"Tristan"});
    camera_roll roll(phone.root / "roll", scratch, 6, 9);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    CHECK(run_once(*target, roll, **ledger, scratch, 0, 2.0, *made).now == bk::state::done);
    CHECK(files_under(phone.root / "sample-nas" / "Tristan").size() == roll.resource_count());
  }
}

// From the review of trash-on-delete: what must never be moved.
TEST_CASE("backup: a file another photo relies on stays, a refused one does not hold the rest up, and many at once move nothing",
          "[mobile-link]") {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  const fs::path nas = phone.root / "sample-nas";
  fs::create_directories(scratch);
  auto target = bk::local_target(nas.string(), "0123456789abcdef", bk::kSampleLimit,
                                 [](const std::string&) { return std::uint64_t{1} << 40; });
  const std::string root = bk::kSampleFolderId;

  SECTION("the same picture twice in Photos: deleting one leaves the file for the other") {
    camera_roll roll(phone.root / "roll", scratch, 4, 7);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    REQUIRE(run_deleting(*target, roll, roll.items, {}, **ledger, scratch, root).first.now == bk::state::done);
    const bk::item first = roll.items[0];
    // A second photo recorded against the first one's files, as a duplicate in Photos is.
    for (const auto& r : first.resources) {
      auto row = (*ledger)->find(root, first.key, r.kind, r.name);
      REQUIRE(row);
      row->key = "twin-of-first";
      REQUIRE((*ledger)->add(*row));
    }
    std::vector<bk::item> now(roll.items.begin() + 1, roll.items.end());
    auto [report, done] = run_deleting(*target, roll, now, {first.key}, **ledger, scratch, root);
    CHECK(report.trashed == 0);
    CHECK(trashed_under(nas) == 0);
    CHECK(done == std::vector<std::string>{first.key});
    for (const auto& r : first.resources) {
      CHECK_FALSE((*ledger)->find(root, first.key, r.kind, r.name));  // this photo's row goes
      CHECK((*ledger)->find(root, "twin-of-first", r.kind, r.name));   // the other's stays, with its file
    }
    // The twin deleted too: now nothing relies on the file, and it goes.
    auto [after, after_done] = run_deleting(*target, roll, now, {"twin-of-first"}, **ledger, scratch, root);
    CHECK(after.trashed == first.resources.size());
    CHECK(trashed_under(nas) == first.resources.size());
  }

  SECTION("one file the service refuses to move does not hold the others up") {
    camera_roll roll(phone.root / "roll", scratch, 5, 9);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    REQUIRE(run_deleting(*target, roll, roll.items, {}, **ledger, scratch, root).first.now == bk::state::done);
    struct refuses_one final : bk::target {
      bk::target& t;
      std::string refused;
      explicit refuses_one(bk::target& real) : t(real) {}
      mv::result<std::vector<bk::folder>> folders() override { return t.folders(); }
      mv::result<serve::stat_info> space(const std::string& r) override { return t.space(r); }
      mv::result<std::vector<serve::list_entry>> list(const std::string& r, const std::string& rel) override { return t.list(r, rel); }
      mv::result<serve::client::write_done> write(const std::string& r, const std::string& rel, std::uint64_t size,
                                                  const std::string& hash, std::uint64_t at, const serve::client::read_fn& read,
                                                  const std::function<void(std::uint64_t)>& progress, const mob::cancel_flag* cancel,
                                                  serve::client::write_refused* why, bool meta) override {
        return t.write(r, rel, size, hash, at, read, progress, cancel, why, meta);
      }
      mv::result<std::vector<std::string>> ledger_ids(const std::string& r) override { return t.ledger_ids(r); }
      mv::result<std::string> read_ledger(const std::string& r, const std::string& id) override { return t.read_ledger(r, id); }
      std::string device_id() const override { return t.device_id(); }
      mv::result<bool> trash(const std::string& r, const std::string& rel) override {
        if (rel == refused) return mv::err(mv::status::permission_denied);
        return t.trash(r, rel);
      }
    } picky(*target);
    const bk::item stuck = roll.items[0], fine = roll.items[1];
    picky.refused = (*ledger)->find(root, stuck.key, stuck.resources[0].kind, stuck.resources[0].name)->rel;
    std::vector<bk::item> now(roll.items.begin() + 2, roll.items.end());
    auto [report, done] = run_deleting(picky, roll, now, {stuck.key, fine.key}, **ledger, scratch, root);
    CHECK(report.trashed == fine.resources.size());
    CHECK(done == std::vector<std::string>{fine.key});  // the refused one waits, the other is done
    CHECK((*ledger)->find(root, stuck.key, stuck.resources[0].kind, stuck.resources[0].name));
  }

  SECTION("more than the limit gone at once is a library removed from the phone: nothing moves") {
    const int count = static_cast<int>(bk::kMaxDeletedAtOnce) + 1;
    camera_roll roll(phone.root / "roll", scratch, count, 3);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    REQUIRE(run_deleting(*target, roll, roll.items, {}, **ledger, scratch, root).first.now == bk::state::done);
    const std::size_t rows = (*ledger)->size(root);
    std::vector<std::string> all;
    for (const auto& it : roll.items) all.push_back(it.key);
    auto [report, done] = run_deleting(*target, roll, {}, all, **ledger, scratch, root);
    CHECK(report.trashed == 0);
    CHECK(trashed_under(nas) == 0);
    CHECK(done.size() == all.size());         // done with: never mirrored on a later run either
    CHECK((*ledger)->size(root) == rows);     // and still recorded as backed up
    // One short of that is someone deleting photos.
    all.pop_back();
    std::vector<bk::item> kept{roll.items.back()};
    auto [some, some_done] = run_deleting(*target, roll, kept, all, **ledger, scratch, root);
    CHECK(some.trashed > 0);
    CHECK(trashed_under(nas) == some.trashed);
  }
}

TEST_CASE("backup: the sample library's folder on the phone runs the same backup and sends nothing", "[mobile-link]") {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  const fs::path sample_nas = phone.root / "sample-nas";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 16, 42);
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);
  auto target = bk::local_target(sample_nas.string(), "0123456789abcdef", bk::kSampleLimit,
                                 [](const std::string&) { return std::uint64_t{1} << 40; });
  auto folders = target->folders();
  REQUIRE(folders);
  REQUIRE(folders->size() == 1);
  CHECK(folders->front().label == bk::kSampleFolderLabel);
  bk::status_report last;
  for (int launch = 0; launch < 30 && last.now != bk::state::done; ++launch) {
    last = run_once(*target, roll, **ledger, scratch, 3, 0.5, bk::kSampleFolderId);
  }
  REQUIRE(last.now == bk::state::done);
  const auto files = files_under(sample_nas);
  CHECK(files.size() == roll.resource_count());
  for (const auto& [rel, bytes] : files) CHECK(rel.find(".mvpart") == std::string::npos);
  CHECK(fs::exists(sample_nas / serve::kBackupMetaFolder / "0123456789abcdef" / "ledger.json"));

  // Its own small limit shows the limit state.
  temp_root other;
  auto tight = bk::local_target((other.root / "nas").string(), "0123456789abcdef", 300 * 1024,
                                [](const std::string&) { return std::uint64_t{1} << 40; });
  auto ledger2 = bk::ledger::open((other.root / "ledger.db").string());
  REQUIRE(ledger2);
  CHECK(run_once(*tight, roll, **ledger2, scratch, 0, 2.0, bk::kSampleFolderId).now == bk::state::limit);
}

#include "mobile/mv_mobile_link.h"

TEST_CASE("backup: the C ABI says a phone limited to other folders has none it may use", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  key32 pub;
  crypto_scalarmult_base(pub.data(), device.data());
  serve::grants::grant g;
  g.name = "Phone";
  g.key = pub;
  g.roots = std::vector<std::string>{kSharedRoot};
  REQUIRE(serve::grants::save(*n.store, {g}));
  n.srv->recheck_devices();
  temp_root phone;
  const auto cfg = n.config(device);
  const std::string lib_dir = (phone.root / "library").string(), cache = (phone.root / "cache").string();
  mv_link_config lc{};
  lc.library_dir = lib_dir.c_str();
  lc.cache_dir = cache.c_str();
  lc.library_id = kLibrary.c_str();
  lc.local = cfg.local.c_str();
  lc.public_name = cfg.public_name.c_str();
  lc.device_name = "Test iPhone";
  lc.identity = cfg.identity.data();
  lc.access_key = cfg.access_key.data();
  lc.device_key = cfg.device_key.data();
  key32 publisher{};
  lc.publisher_key = publisher.data();
  mv_mobile_link* link = mv_link_open(&lc);
  REQUIRE(link);
  mv_route_report report{};
  REQUIRE(mv_link_connect(link, nullptr, &report) == MV_LINK_OK);
  const std::string ledger_path = (phone.root / "ledger.db").string(), scratch = (phone.root / "scratch").string();
  fs::create_directories(scratch);
  mv_backup_config bc{ledger_path.c_str(), scratch.c_str(), kLibrary.c_str(), nullptr};
  mv_backup* b = mv_backup_open(link, &bc);
  REQUIRE(b);
  int count = 0;
  CHECK(mv_backup_folders(b, [](void* ctx, const char*, const char*, int32_t) { ++*static_cast<int*>(ctx); }, &count) ==
        MV_LINK_NO_FOLDER);
  CHECK(count == 0);
  mv_backup_close(b);
  mv_link_close(link);
}

TEST_CASE("backup: the C ABI runs a backup the way the app does", "[mobile-link]") {
  nas n;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 6, 5);

  const auto cfg = n.config(device);
  const std::string lib_dir = (phone.root / "library").string(), cache = (phone.root / "cache").string();
  mv_link_config lc{};
  lc.library_dir = lib_dir.c_str();
  lc.cache_dir = cache.c_str();
  lc.library_id = kLibrary.c_str();
  lc.local = cfg.local.c_str();
  lc.public_name = cfg.public_name.c_str();
  lc.device_name = "Test iPhone";
  lc.identity = cfg.identity.data();
  lc.access_key = cfg.access_key.data();
  lc.device_key = cfg.device_key.data();
  key32 publisher{};
  lc.publisher_key = publisher.data();
  mv_mobile_link* link = mv_link_open(&lc);
  REQUIRE(link);
  mv_route_report report{};
  REQUIRE(mv_link_connect(link, nullptr, &report) == MV_LINK_OK);

  const std::string ledger_path = (phone.root / "ledger.db").string(), scratch_s = scratch.string();
  mv_backup_config bc{ledger_path.c_str(), scratch_s.c_str(), kLibrary.c_str(), nullptr};
  mv_backup* b = mv_backup_open(link, &bc);
  REQUIRE(b);

  std::vector<std::pair<std::string, int>> folders;
  REQUIRE(mv_backup_folders(b, [](void* ctx, const char* id, const char*, int32_t writable) {
            static_cast<std::vector<std::pair<std::string, int>>*>(ctx)->push_back({id, writable});
          }, &folders) == MV_LINK_OK);
  REQUIRE(folders.size() == 1);
  CHECK(folders[0].first == kBackupRoot);
  CHECK(folders[0].second == 1);

  for (const auto& it : roll.items) {
    std::string json = "[";
    for (std::size_t i = 0; i < it.resources.size(); ++i) {
      if (i) json += ",";
      json += "{\"kind\":\"" + it.resources[i].kind + "\",\"name\":\"" + it.resources[i].name + "\"}";
    }
    json += "]";
    REQUIRE(mv_backup_add_item(b, it.key.c_str(), it.local_id.c_str(), it.year, it.month, it.day, json.c_str()) == MV_LINK_OK);
  }
  // A name that is a path is never taken from the host.
  CHECK(mv_backup_add_item(b, "k", "1", 2026, 1, 1, R"([{"kind":"photo","name":"../x.jpg"}])") == MV_LINK_INVALID);

  struct ctx_t {
    camera_roll* roll;
    std::vector<mv_backup_status> seen;
  } ctx{&roll, {}};
  const auto exporter = [](void* c, const char* local_id, int32_t index, const char* path, mv_cancel* cancel, int32_t*) -> int32_t {
    if (mv_backup_cancelled(cancel)) return MV_LINK_CANCELLED;
    auto* source = static_cast<ctx_t*>(c)->roll;
    for (const auto& it : source->items) {
      if (it.local_id == local_id) {
        int ymd[3] = {it.year, it.month, it.day};
        return source->export_resource(it, static_cast<std::size_t>(index), path, nullptr, ymd) ? MV_LINK_OK : MV_LINK_INTERNAL;
      }
    }
    return MV_LINK_NOT_FOUND;
  };
  const auto status = [](void* c, const mv_backup_status* s) { static_cast<ctx_t*>(c)->seen.push_back(*s); };
  int32_t final_state = -1;
  CHECK(mv_backup_run(b, kBackupRoot.c_str(), 5, nullptr, exporter, status, &ctx, &final_state) == MV_LINK_OK);
  CHECK(final_state == MV_BACKUP_DONE);
  REQUIRE_FALSE(ctx.seen.empty());
  CHECK(ctx.seen.back().backed_up == roll.items.size());
  CHECK(files_under(n.backup).size() == roll.resource_count());
  // A bad layout is refused before anything runs.
  CHECK(mv_backup_run(b, kBackupRoot.c_str(), 0, nullptr, exporter, status, &ctx, &final_state) == MV_LINK_INVALID);
  CHECK(mv_backup_add_item(b, "k", "1", 2026, 1, 1, "[]") == MV_LINK_INVALID);

  mv_backup_space space{};
  CHECK(mv_backup_space_of(b, kBackupRoot.c_str(), &space) == MV_LINK_OK);
  CHECK(space.used > 0);

  // A person's own subfolder: made, listed, backed up to; never one outside the folder.
  const auto names = [](void* c, const char* name) { static_cast<std::vector<std::string>*>(c)->push_back(name); };
  std::vector<std::string> made;
  CHECK(mv_backup_make_folder(b, kBackupRoot.c_str(), "Tristan", names, &made) == MV_LINK_OK);
  REQUIRE(made == std::vector<std::string>{kBackupRoot + "/Tristan"});
  CHECK(mv_backup_make_folder(b, kBackupRoot.c_str(), "../Tristan", names, &made) == MV_LINK_INVALID);
  CHECK(mv_backup_make_folder(b, kBackupRoot.c_str(), ".hidden", names, &made) == MV_LINK_INVALID);
  std::vector<std::string> inside;
  CHECK(mv_backup_subfolders(b, kBackupRoot.c_str(), names, &inside) == MV_LINK_OK);
  CHECK(std::find(inside.begin(), inside.end(), "Tristan") != inside.end());
  CHECK(mv_backup_run(b, made[0].c_str(), 5, nullptr, exporter, status, &ctx, &final_state) == MV_LINK_OK);
  CHECK(final_state == MV_BACKUP_DONE);
  CHECK(files_under(n.backup / "Tristan").size() == roll.resource_count());
  CHECK(mv_backup_space_of(b, made[0].c_str(), &space) == MV_LINK_OK);
  const std::string escaping = kBackupRoot + "/../elsewhere";
  CHECK(mv_backup_run(b, escaping.c_str(), 5, nullptr, exporter, status, &ctx, &final_state) == MV_LINK_INVALID);
  CHECK(mv_backup_subfolders(b, escaping.c_str(), names, &inside) == MV_LINK_INVALID);
  CHECK(mv_backup_space_of(b, escaping.c_str(), &space) == MV_LINK_INVALID);
  mv_backup_close(b);
  mv_link_shut_down(link);
  mv_link_close(link);
}

TEST_CASE("backup: the date read from the file files the whole photo", "[mobile-link]") {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 4, 3);
  roll.date_from_file = true;
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);
  auto target = bk::local_target((phone.root / "nas").string(), "0123456789abcdef", bk::kSampleLimit,
                                 [](const std::string&) { return std::uint64_t{1} << 40; });
  REQUIRE(run_once(*target, roll, **ledger, scratch, 0, 2.0, bk::kSampleFolderId).now == bk::state::done);
  for (const auto& [rel, bytes] : files_under(phone.root / "nas")) CHECK(rel.starts_with("2019/12/"));
}

TEST_CASE("backup: a hostile or garbled ledger copy on the NAS never blocks or skips a photo", "[mobile-link]") {
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  const fs::path nas_dir = phone.root / "nas";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, 5, 11);
  const auto& first = roll.items.front();
  const std::string hash(64, 'a');
  // Another "device" claims every photo is done, with paths that escape, a wrong name, another library.
  std::string rows;
  for (const auto& it : roll.items) {
    for (const auto& r : it.resources) {
      if (!rows.empty()) rows += ",";
      rows += "{\"k\":\"" + it.key + "\",\"r\":\"" + r.kind + "\",\"n\":\"" + r.name + "\",\"s\":1,\"h\":\"" + hash +
              "\",\"f\":\"" + bk::kSampleFolderId + "\",\"p\":\"../" + r.name + "\",\"t\":0}";
    }
  }
  rows += ",{\"k\":\"" + first.key + "\",\"r\":\"" + first.resources[0].kind + "\",\"n\":\"" + first.resources[0].name +
          "\",\"s\":1,\"h\":\"" + hash + "\",\"f\":\"" + bk::kSampleFolderId + "\",\"p\":\"other.jpg\",\"t\":0}";
  write_file(nas_dir / serve::kBackupMetaFolder / "ffffffffffffffff" / "ledger.json",
             "{\"v\":1,\"library_id\":\"" + kLibrary + "\",\"rows\":[" + rows + "]}");
  write_file(nas_dir / "other.jpg", "x");
  write_file(nas_dir / serve::kBackupMetaFolder / "eeeeeeeeeeeeeeee" / "ledger.json", "{not json");
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);
  auto target = bk::local_target(nas_dir.string(), "0123456789abcdef", bk::kSampleLimit,
                                 [](const std::string&) { return std::uint64_t{1} << 40; });
  const auto last = run_once(*target, roll, **ledger, scratch, 0, 2.0, bk::kSampleFolderId);
  CHECK(last.now == bk::state::done);
  CHECK(last.copied == roll.resource_count());
  CHECK(last.backed_up == roll.items.size());
}

// ---- changing the shared folders (serve_client.h "changing the shared folders") ----------------

namespace {

// A "view" device: on the grants list with backup off.
void grant_view(nas& n, const key32& device) {
  key32 pub;
  crypto_scalarmult_base(pub.data(), device.data());
  std::vector<serve::grants::grant> list;
  REQUIRE(serve::grants::load(*n.store, list));
  serve::grants::grant g;
  g.name = "Viewer";
  g.key = pub;
  g.backup = false;
  g.created = 1;
  list.push_back(g);
  REQUIRE(serve::grants::save(*n.store, list));
}

std::string random_bytes(std::size_t n) {
  std::string s(n, '\0');
  randombytes_buf(s.data(), s.size());
  return s;
}

bool root_writable(mob::link_session& link, const std::string& id) {
  auto roots = link.roots();
  REQUIRE(roots);
  for (const auto& r : *roots) {
    if (r.id == id) return r.writable;
  }
  return false;
}

int leftovers(const fs::path& root) {
  int n = 0;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    const std::string name = e.path().filename().string();
    if (name.ends_with(".mvpart") || name.ends_with(".mvmove")) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("change: a change device adds, makes folders, renames, moves and deletes over the link", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  mob::link_session link(n.config(device));
  mob::route_report report;
  REQUIRE(link.connect(report));
  REQUIRE(link.changes_files());
  CHECK(*link.changes_files());
  CHECK(root_writable(link, kSharedRoot));

  const fs::path local = phone.root / "IMG_0001.jpg";
  write_file(local, "a photo from the phone");
  serve::client::change_refused why;
  auto added = link.add_file(kSharedRoot, "Trips/IMG_0001.jpg", local.string(), nullptr, nullptr, &why);
  REQUIRE(added);
  CHECK(added->rel == "Trips/IMG_0001.jpg");
  CHECK(read_all(n.shared / "Trips" / "IMG_0001.jpg") == "a photo from the phone");
  // The same name again, a different file: kept beside it, never over it.
  write_file(local, "another photo");
  added = link.add_file(kSharedRoot, "Trips/IMG_0001.jpg", local.string(), nullptr, nullptr, &why);
  REQUIRE(added);
  CHECK(added->rel == "Trips/IMG_0001 (2).jpg");
  CHECK(read_all(n.shared / "Trips" / "IMG_0001.jpg") == "a photo from the phone");

  REQUIRE(link.make_folder(kSharedRoot, "Trips/Lisbon", &why));
  CHECK(fs::is_directory(n.shared / "Trips" / "Lisbon"));
  // Rename, then move into the new folder.
  REQUIRE(link.move(kSharedRoot, "Trips/IMG_0001 (2).jpg", kSharedRoot, "Trips/Harbour.jpg", &why));
  REQUIRE(link.move(kSharedRoot, "Trips/Harbour.jpg", kSharedRoot, "Trips/Lisbon/Harbour.jpg", &why));
  CHECK(read_all(n.shared / "Trips" / "Lisbon" / "Harbour.jpg") == "another photo");
  // A taken destination is refused, and nothing is replaced.
  auto clash = link.move(kSharedRoot, "Trips/IMG_0001.jpg", kSharedRoot, "Trips/Lisbon/Harbour.jpg", &why);
  CHECK_FALSE(clash);
  CHECK(why.why == serve::client::change_why::exists);
  CHECK(read_all(n.shared / "Trips" / "Lisbon" / "Harbour.jpg") == "another photo");
  // Delete: into the folder's trash on the server, gone from every listing.
  auto removed = link.remove(kSharedRoot, "Trips/Lisbon", &why);
  REQUIRE(removed);
  CHECK_FALSE(fs::exists(n.shared / "Trips" / "Lisbon"));
  CHECK(removed->rel.ends_with("/Trips/Lisbon"));
  CHECK(fs::exists(n.shared / serve::kTrashFolder / removed->rel / "Harbour.jpg"));
  auto listed = link.list(kSharedRoot, "Trips");
  REQUIRE(listed);
  CHECK(listed->size() == 1);
  CHECK(leftovers(n.shared) == 0);
}

TEST_CASE("change: a view device sees no writable folder and is refused", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  const key32 device = random_key();
  n.start();
  grant_view(n, device);
  mob::link_session link(n.config(device));
  mob::route_report report;
  REQUIRE(link.connect(report));
  CHECK_FALSE(root_writable(link, kSharedRoot));
  serve::client::change_refused why;
  CHECK_FALSE(link.make_folder(kSharedRoot, "New folder", &why));
  CHECK(why.why == serve::client::change_why::not_allowed);
  CHECK_FALSE(fs::exists(n.shared / "New folder"));
}

TEST_CASE("change: an add carries on after a cancel and after a dropped link, and arrives whole", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  const fs::path local = phone.root / "clip.mov";
  const std::string bytes = random_bytes(24u << 20);
  write_file(local, bytes);
  mob::link_session link(n.config(device));
  mob::route_report report;
  REQUIRE(link.connect(report));
  serve::client::change_refused why;

  SECTION("cancelled part way, then again: picks up where the copy ended") {
    mob::cancel_flag cancel;
    auto first = link.add_file(kSharedRoot, "clip.mov", local.string(),
                               [&](std::uint64_t done, std::uint64_t) {
                                 if (done >= (4u << 20)) cancel.stop = true;
                               },
                               &cancel, &why);
    CHECK_FALSE(first);
    CHECK_FALSE(fs::exists(n.shared / "clip.mov"));
    std::uint64_t lowest = UINT64_MAX;
    auto again = link.add_file(kSharedRoot, "clip.mov", local.string(),
                               [&](std::uint64_t done, std::uint64_t) { lowest = std::min(lowest, done); }, nullptr, &why);
    REQUIRE(again);
    CHECK(again->rel == "clip.mov");
    CHECK(lowest >= (4u << 20));  // never from the start
  }
  SECTION("stopped while it is still being hashed: nothing is sent") {
    mob::cancel_flag cancel;
    cancel.stop = true;
    bool sent = false;
    auto stopped = link.add_file(kSharedRoot, "clip.mov", local.string(), [&](std::uint64_t, std::uint64_t) { sent = true; },
                                 &cancel, &why);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error() == mv::status::cancelled);
    CHECK_FALSE(sent);
    CHECK(fs::is_empty(n.shared));
    // Then sent whole, as the other sections end.
    REQUIRE(link.add_file(kSharedRoot, "clip.mov", local.string(), nullptr, nullptr, &why));
  }
  SECTION("the link cut mid-file: reconnected, and carried on") {
    std::atomic<bool> cut{false};
    std::thread cutter;
    auto done = link.add_file(kSharedRoot, "clip.mov", local.string(),
                              [&](std::uint64_t sent, std::uint64_t) {
                                if (sent >= (4u << 20) && !cut.exchange(true)) cutter = std::thread([&] { link.disconnect(); });
                              },
                              nullptr, &why);
    if (cutter.joinable()) cutter.join();
    CHECK(cut);
    INFO("status " << (done ? 0 : static_cast<int>(done.error())) << " why " << static_cast<int>(why.why) << " "
                   << why.word);
    REQUIRE(done);
    CHECK(done->rel == "clip.mov");
  }
  CHECK(read_all(n.shared / "clip.mov").size() == bytes.size());
  CHECK((read_all(n.shared / "clip.mov") == bytes));
  CHECK(leftovers(n.shared) == 0);
}

TEST_CASE("change: the C ABI the app calls", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  const auto cfg = n.config(device);
  const std::string lib_dir = (phone.root / "library").string(), cache = (phone.root / "cache").string();
  mv_link_config lc{};
  lc.library_dir = lib_dir.c_str();
  lc.cache_dir = cache.c_str();
  lc.library_id = kLibrary.c_str();
  lc.local = cfg.local.c_str();
  lc.public_name = cfg.public_name.c_str();
  lc.device_name = "Test iPhone";
  lc.identity = cfg.identity.data();
  lc.access_key = cfg.access_key.data();
  lc.device_key = cfg.device_key.data();
  key32 publisher{};
  lc.publisher_key = publisher.data();
  mv_mobile_link* link = mv_link_open(&lc);
  REQUIRE(link);
  mv_route_report report{};
  REQUIRE(mv_link_connect(link, nullptr, &report) == MV_LINK_OK);

  int32_t can = 0;
  REQUIRE(mv_files_can_change(link, &can) == MV_LINK_OK);
  CHECK(can == 1);
  std::map<std::string, int> writable;
  REQUIRE(mv_files_writable_roots(link, [](void* ctx, const char* id, int32_t w) {
            (*static_cast<std::map<std::string, int>*>(ctx))[id] = w;
          }, &writable) == MV_LINK_OK);
  CHECK(writable[kSharedRoot] == 1);

  const fs::path local = phone.root / "notes.txt";
  write_file(local, "notes");
  mv_change_result out{};
  std::uint64_t last = 0;
  REQUIRE(mv_files_add(link, kSharedRoot.c_str(), "notes.txt", local.string().c_str(), nullptr,
                       [](void* ctx, uint64_t done, uint64_t) { *static_cast<std::uint64_t*>(ctx) = done; }, &last,
                       &out) == MV_LINK_OK);
  CHECK(std::string(out.rel) == "notes.txt");
  CHECK(last == 5);
  REQUIRE(mv_files_mkdir(link, kSharedRoot.c_str(), "Docs", &out) == MV_LINK_OK);
  REQUIRE(mv_files_move(link, kSharedRoot.c_str(), "notes.txt", kSharedRoot.c_str(), "Docs/notes.txt", &out) == MV_LINK_OK);
  CHECK(std::string(out.rel) == "Docs/notes.txt");
  REQUIRE(mv_files_mkdir(link, kSharedRoot.c_str(), "Docs2", &out) == MV_LINK_OK);
  CHECK(mv_files_move(link, kSharedRoot.c_str(), "Docs2", kSharedRoot.c_str(), "Docs", &out) == MV_LINK_DENIED);
  CHECK(out.why == MV_CHANGE_EXISTS);
  REQUIRE(mv_files_delete(link, kSharedRoot.c_str(), "Docs", &out) == MV_LINK_OK);
  CHECK_FALSE(fs::exists(n.shared / "Docs"));
  CHECK(mv_files_delete(link, kSharedRoot.c_str(), "Docs", &out) == MV_LINK_NOT_FOUND);
  CHECK(out.why == MV_CHANGE_NOT_FOUND);
  CHECK(mv_files_mkdir(link, kSharedRoot.c_str(), "", &out) == MV_LINK_INVALID);
  // The phone's own file missing: said as the phone's, not the server's.
  CHECK(mv_files_add(link, kSharedRoot.c_str(), "x.txt", (phone.root / "missing.txt").string().c_str(), nullptr, nullptr,
                     nullptr, &out) == MV_LINK_INVALID);
  CHECK(out.why == MV_CHANGE_LOCAL);
  mv_link_close(link);
}

TEST_CASE("change: the phone's own file failing is told apart from the server and the link", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  mob::link_session link(n.config(device));
  mob::route_report report;
  REQUIRE(link.connect(report));
  serve::client::change_refused why;

  SECTION("not on the phone any more") {
    auto gone = link.add_file(kSharedRoot, "a.jpg", (phone.root / "missing.jpg").string(), nullptr, nullptr, &why);
    REQUIRE_FALSE(gone);
    CHECK(gone.error() == mv::status::invalid_arg);
    CHECK(link.connected());
  }
  SECTION("cut short while it is sent: the link stays, and nothing is left whole on the server") {
    const fs::path local = phone.root / "clip.mov";
    write_file(local, random_bytes(12u << 20));
    bool cut = false;
    auto short_read = link.add_file(kSharedRoot, "clip.mov", local.string(),
                                    [&](std::uint64_t sent, std::uint64_t) {
                                      if (sent >= (2u << 20) && !cut) {
                                        cut = true;
                                        fs::resize_file(local, 1u << 20);
                                      }
                                    },
                                    nullptr, &why);
    REQUIRE_FALSE(short_read);
    CHECK(short_read.error() == mv::status::invalid_arg);
    CHECK(link.connected());
    CHECK_FALSE(fs::exists(n.shared / "clip.mov"));
  }
}

TEST_CASE("change: what arrives is never kept unless it is what was hashed", "[mobile-link]") {
  nas n;
  n.with_shared = true;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  temp_root phone;
  mob::link_session link(n.config(device));
  mob::route_report report;
  REQUIRE(link.connect(report));
  serve::client::change_refused why;
  const fs::path local = phone.root / "clip.mov";

  SECTION("changed on the phone while it is sent: sent again once, then refused as damaged") {
    write_file(local, random_bytes(8u << 20));
    bool changed = false;
    auto done = link.add_file(kSharedRoot, "clip.mov", local.string(),
                              [&](std::uint64_t sent, std::uint64_t) {
                                if (sent >= (1u << 20) && !changed) {
                                  changed = true;
                                  write_file(local, random_bytes(8u << 20));  // same size, other bytes
                                }
                              },
                              nullptr, &why);
    REQUIRE_FALSE(done);
    CHECK(why.why == serve::client::change_why::hash);
    CHECK_FALSE(fs::exists(n.shared / "clip.mov"));
    CHECK(leftovers(n.shared) == 0);
  }
  SECTION("an unfinished copy of another file under the same name: the new one arrives whole") {
    write_file(local, random_bytes(16u << 20));
    mob::cancel_flag cancel;
    CHECK_FALSE(link.add_file(kSharedRoot, "clip.mov", local.string(),
                              [&](std::uint64_t sent, std::uint64_t) {
                                if (sent >= (4u << 20)) cancel.stop = true;
                              },
                              &cancel, &why));
    const std::string other = random_bytes(16u << 20);
    write_file(local, other);
    auto done = link.add_file(kSharedRoot, "clip.mov", local.string(), nullptr, nullptr, &why);
    REQUIRE(done);
    CHECK(done->rel == "clip.mov");
    CHECK((read_all(n.shared / "clip.mov") == other));
  }
}

// A link target that counts the writes on their way at once.
struct counting_target final : bk::target {
  bk::target& t;
  std::mutex m;
  std::condition_variable cv;
  int now = 0, most = 0;
  bool hold_first = false;  // the first write waits until another is on its way (10 s at most)
  int writes = 0;
  std::map<std::string, int> at_once_by_rel;  // the most writes of one name at once
  explicit counting_target(bk::target& x) : t(x) {}
  mv::result<std::vector<bk::folder>> folders() override { return t.folders(); }
  mv::result<serve::stat_info> space(const std::string& root) override { return t.space(root); }
  mv::result<std::vector<serve::list_entry>> list(const std::string& root, const std::string& rel) override {
    return t.list(root, rel);
  }
  mv::result<serve::client::write_done> write(const std::string& root, const std::string& rel, std::uint64_t size,
                                              const std::string& hash, std::uint64_t at,
                                              const serve::client::read_fn& read,
                                              const std::function<void(std::uint64_t)>& progress,
                                              const mob::cancel_flag* cancel, serve::client::write_refused* why,
                                              bool meta) override {
    {
      std::unique_lock lock(m);
      most = std::max(most, ++now);
      const int first = ++writes == 1;
      auto& here = at_once_by_rel[rel + "#" + std::to_string(meta)];
      ++here;
      at_once_by_rel["most:" + rel] = std::max(at_once_by_rel["most:" + rel], here);
      cv.notify_all();
      if (hold_first && first) cv.wait_for(lock, std::chrono::seconds(10), [&] { return now >= 2; });
    }
    auto r = t.write(root, rel, size, hash, at, read, progress, cancel, why, meta);
    std::lock_guard lock(m);
    --now;
    --at_once_by_rel[rel + "#" + std::to_string(meta)];
    return r;
  }
  mv::result<std::vector<std::string>> ledger_ids(const std::string& root) override { return t.ledger_ids(root); }
  mv::result<std::string> read_ledger(const std::string& root, const std::string& id) override {
    return t.read_ledger(root, id);
  }
  std::string device_id() const override { return t.device_id(); }
  bool limited_to_some_folders() const override { return t.limited_to_some_folders(); }
  bool writes_at_once() override { return t.writes_at_once() && !one_at_a_time; }
  bool one_at_a_time = false;  // as a service from before kWritesAtOnceOp says
  mv::result<bool> trash(const std::string& root, const std::string& rel) override { return t.trash(root, rel); }
  mv::expected make_folder(const std::string& root, const std::string& rel) override { return t.make_folder(root, rel); }
};

TEST_CASE("backup: several files are on their way at once, within the phone's scratch budget", "[mobile-link]") {
  // Before this, one file at a time: each waits a round trip for the service's first credit
  // and another for its confirmation, so small photos left the link idle.
  const auto run = [](const std::function<void(bk::options&)>& tune, int& most, std::uint64_t& peak,
                      std::uint64_t& largest, bool older_service = false) {
    nas n;
    n.start();
    const key32 device = random_key();
    n.pair(device);
    temp_root phone;
    const fs::path scratch = phone.root / "scratch";
    fs::create_directories(scratch);
    camera_roll roll(phone.root / "roll", scratch, 40, 4321);
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto inner = bk::link_target(link);
    counting_target counted(*inner);
    counted.one_at_a_time = older_service;
    const auto last = run_once(counted, roll, **ledger, scratch, 0, 2.0, kBackupRoot, tune);
    CHECK(last.now == bk::state::done);
    CHECK(last.backed_up == roll.items.size());
    CHECK(last.copied == roll.resource_count());
    // Every file once, whole.
    const auto on_nas = files_under(n.backup);
    CHECK(on_nas.size() == roll.resource_count());
    std::set<std::string> contents;
    for (const auto& [rel, bytes] : on_nas) contents.insert(bytes);
    for (const auto& [k, bytes] : roll.files) CHECK(contents.count(bytes) == 1);
    CHECK((*ledger)->size(kBackupRoot) == on_nas.size());
    most = counted.most;
    peak = roll.peak_scratch.load();
    largest = roll.largest;
  };
  int most = 0;
  std::uint64_t peak = 0, largest = 0;
  SECTION("by default, up to uploads_at_once (this service lists writes_at_once)") {
    run({}, most, peak, largest);
    CHECK(most > 1);
    CHECK(most <= bk::options{}.uploads_at_once);
  }
  SECTION("no budget: one file at a time, one file's storage") {
    run([](bk::options& o) { o.parallel_bytes = 0; }, most, peak, largest);
    CHECK(most == 1);
    CHECK(peak <= largest);
  }
  SECTION("a service that does not list writes_at_once: one file at a time") {
    run({}, most, peak, largest, true);
    CHECK(most == 1);
    CHECK(peak <= largest);
  }
  SECTION("a small budget: the storage stays within one file and it") {
    constexpr std::int64_t kBudget = 300 * 1000;
    run([](bk::options& o) { o.parallel_bytes = kBudget; }, most, peak, largest);
    CHECK(peak <= largest + static_cast<std::uint64_t>(kBudget));
  }
}

TEST_CASE("backup: two photos of one name in one folder never send at once, and both land as one at a time did",
          "[mobile-link]") {
  // The service holds a name until its write is done: a second of that name sent beside it was
  // refused (busy), and the run ended with a failure, where one at a time it was the same file
  // already there, or the service's " (2)".
  const auto run = [](bool same_bytes) {
    nas n;
    n.start();
    const key32 device = random_key();
    n.pair(device);
    temp_root phone;
    const fs::path scratch = phone.root / "scratch";
    fs::create_directories(scratch);
    camera_roll roll(phone.root / "roll", scratch, 8, 99);
    // Items 0 and 1: one name, one day (photo 0 kept; 1 made a plain photo of the same name).
    roll.items[1].resources = {roll.items[0].resources[0]};
    roll.items[1].year = roll.items[0].year;
    roll.items[1].month = roll.items[0].month;
    roll.items[1].day = roll.items[0].day;
    for (auto it = roll.files.begin(); it != roll.files.end();) {
      it = it->first.rfind(roll.items[1].key + "/", 0) == 0 ? roll.files.erase(it) : std::next(it);
    }
    roll.files[roll.items[1].key + "/0"] =
        same_bytes ? roll.files[roll.items[0].key + "/0"] : std::string(5000, 'z');
    auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
    REQUIRE(ledger);
    mob::link_session link(n.config(device));
    mob::route_report report;
    REQUIRE(link.connect(report));
    auto inner = bk::link_target(link);
    counting_target counted(*inner);
    counted.hold_first = true;  // the first is still on its way when the next would go
    const auto last = run_once(counted, roll, **ledger, scratch, 0);
    CHECK(last.now == bk::state::done);
    CHECK(last.failures.empty());
    CHECK(last.backed_up == roll.items.size());
    for (const auto& [k, v] : counted.at_once_by_rel) {
      if (k.rfind("most:", 0) == 0) CHECK(v == 1);
    }
    const auto on_nas = files_under(n.backup);
    const std::string name = roll.items[0].resources[0].name;
    const std::string stem = name.substr(0, name.rfind('.'));
    int of_name = 0;
    for (const auto& [rel, bytes] : on_nas) {
      if (fs::path(rel).filename().string().rfind(stem, 0) == 0) ++of_name;
    }
    CHECK(of_name == (same_bytes ? 1 : 2));  // the same file once; another as " (2)"
  };
  SECTION("the same photo twice") { run(true); }
  SECTION("another photo of the same name") { run(false); }
}

// The backup's pace over a phone's path to the NAS (tests/emulated_path.h), one JSON line:
//   mv_mobile_link_tests "[.backup-bench]"
//     MV_BACKUP_BENCH_RTT_MS  the round trip (default 40)
//     MV_BACKUP_BENCH_MBPS    the path's rate each way, Mbit/s (default 50)
//     MV_BACKUP_BENCH_ITEMS   photos in the roll (default 80; 1 to 200 KB, every fourth a video to 900 KB)
TEST_CASE("backup: the pace of many small photos over a phone's path", "[.backup-bench]") {
  const auto env = [](const char* name, double fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : fallback;
  };
  const int rtt = static_cast<int>(env("MV_BACKUP_BENCH_RTT_MS", 40));
  const double mbps = env("MV_BACKUP_BENCH_MBPS", 50);
  const int count = static_cast<int>(env("MV_BACKUP_BENCH_ITEMS", 80));
  nas n;
  n.start();
  const key32 device = random_key();
  n.pair(device);
  mv::test::path_proxy path({"127.0.0.1", n.srv->port()}, rtt, mbps);
  REQUIRE(path.ok());
  temp_root phone;
  const fs::path scratch = phone.root / "scratch";
  fs::create_directories(scratch);
  camera_roll roll(phone.root / "roll", scratch, count, 77);
  auto ledger = bk::ledger::open((phone.root / "ledger.db").string());
  REQUIRE(ledger);
  auto c = n.config(device);
  c.local = "127.0.0.1:" + std::to_string(path.port());
  c.public_name = c.local;
  mob::link_session link(c);
  mob::route_report report;
  REQUIRE(link.connect(report));
  auto target = bk::link_target(link);
  std::uint64_t bytes = 0;
  for (const auto& [k, b] : roll.files) bytes += b.size();
  const auto t0 = std::chrono::steady_clock::now();
  const auto last = run_once(*target, roll, **ledger, scratch, 0);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("{\"rtt_ms\":%d,\"mbps\":%.0f,\"files\":%zu,\"mb\":%.1f,\"seconds\":%.2f,\"files_per_s\":%.1f,"
              "\"mbit_per_s\":%.1f,\"peak_scratch_kb\":%.0f}\n",
              rtt, mbps, roll.resource_count(), bytes / 1e6, s, roll.resource_count() / s, bytes * 8 / 1e6 / s,
              roll.peak_scratch.load() / 1e3);
  std::fflush(stdout);
  CHECK(last.now == bk::state::done);
  CHECK(files_under(n.backup).size() == roll.resource_count());
}
