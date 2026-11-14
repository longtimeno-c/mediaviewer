// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Check library (docs/plans/check-library.md), end to end over the real host
// table and import.db: a folder checked against a library folder (Import's
// Tools, both platforms), the index making a second check free, the offline
// rule, library scope, and the Mac's Photos backup over the same engine
// through mv.import.1 (shell/photos_backup_library.h) with a fake Photos
// source: planted duplicates, renamed copies, files in other folders, and
// files deleted or moved since the backup.
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_import.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addons/import/engine.h"
#include "addons/import/paths.h"
#include "core/json.h"
#include "import_fixture.h"
#include "io/content_hash.h"
#include "io/file_port.h"
#include "shell/photos_backup.h"
#include "shell/photos_backup_library.h"

using namespace mv::test;
namespace bk = mv::shell::backup;

namespace {

std::atomic<int> g_hashes{0};
const mv_host_api* g_real = nullptr;

mv_status MV_CALL counting_hash(void* host, const char* path, uint32_t uncached, int32_t(MV_CALL* c)(void*),
                                void(MV_CALL* y)(void*), void* u, uint8_t out[32]) {
  ++g_hashes;
  return g_real->hash_file(host, path, uncached, c, y, u, out);
}

// The real host table (io ports), with hashing counted, and the engine.
struct rig {
  scratch_dir dir{"checklib"};
  std::unique_ptr<mv::addon::host_table> table;
  mv_host_api api{};
  std::unique_ptr<mv::import::engine> eng;
  mv_import_api import{};

  rig() {
    mv::addon::host_services svc;
    svc.data_dir = utf8(dir / "data");
    svc.default_library = utf8(dir / "Pictures/MediaViewer");
    table = std::make_unique<mv::addon::host_table>(std::move(svc));
    g_real = table->api();
    api = *table->api();
    api.hash_file = &counting_hash;
    api.watch_volumes = nullptr;
    g_hashes = 0;
    restart();
  }
  ~rig() {
    eng.reset();
    g_real = nullptr;
  }
  void restart() {
    eng.reset();
    eng = std::make_unique<mv::import::engine>(&api);
    REQUIRE(eng->start(utf8(dir / "data/import.db")));
    make_table();
  }

  // mv.import.1's Check library calls (and the job calls the backup uses),
  // straight onto this engine: what addon_entry.cpp does, without a package.
  void make_table() {
    import = mv_import_api{};
    import.struct_size = sizeof(mv_import_api);
    import.ctx = eng.get();
    import.check_library = [](void* ctx, const char* req, uint64_t* out) -> mv_status {
      auto r = static_cast<mv::import::engine*>(ctx)->check_library(req);
      if (!r) return static_cast<mv_status>(r.error());
      *out = *r;
      return MV_OK;
    };
    import.progress = [](void* ctx, uint64_t job, mv_import_progress* out) -> mv_status {
      return static_cast<mv::import::engine*>(ctx)->progress(job, *out) ? MV_OK : MV_ERR_INVALID_ARG;
    };
    import.cancel = [](void* ctx, uint64_t job) -> mv_status {
      return static_cast<mv::import::engine*>(ctx)->cancel(job) ? MV_OK : MV_ERR_INVALID_ARG;
    };
    import.summary_json = [](void* ctx, uint64_t job, char* out, uint32_t cap, uint32_t* needed) -> mv_status {
      auto r = static_cast<mv::import::engine*>(ctx)->summary_json(job);
      if (!r) return static_cast<mv_status>(r.error());
      return copy_out(*r, out, cap, needed);
    };
    import.library_find = [](void* ctx, const char* req, char* out, uint32_t cap, uint32_t* needed) -> mv_status {
      auto r = static_cast<mv::import::engine*>(ctx)->library_find(req);
      if (!r) return static_cast<mv_status>(r.error());
      return copy_out(*r, out, cap, needed);
    };
    import.library_record = [](void* ctx, const char* req) -> mv_status {
      auto r = static_cast<mv::import::engine*>(ctx)->library_record(req);
      return r ? MV_OK : static_cast<mv_status>(r.error());
    };
  }
  static mv_status copy_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
    *needed = static_cast<uint32_t>(text.size() + 1);
    if (!out || cap < *needed) return MV_ERR_INVALID_ARG;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return MV_OK;
  }

  fs::path lib() const { return dir / "Family"; }
  fs::path card() const { return dir / "Card"; }

  // Runs a check to the end; the summary.
  mv::json::value check(const std::string& request) {
    auto job = eng->check_library(request);
    REQUIRE(job);
    eng->wait_idle();
    auto text = eng->summary_json(*job);
    REQUIRE(text);
    auto v = mv::json::parse(*text);
    REQUIRE(v);
    return *v;
  }
  std::string folder_request(const char* scope = "destination") const {
    mv::json::writer w;
    w.begin_object();
    w.key("destination").string(utf8(lib()));
    w.key("scope").string(scope);
    w.key("source_folder").string(utf8(card()));
    w.end_object();
    return w.take();
  }
};

std::vector<std::string> strings(const mv::json::value* v) {
  std::vector<std::string> out;
  REQUIRE(v);
  for (const auto& s : v->a) out.push_back(s.s);
  return out;
}

std::string name_of(const std::string& path) { return utf8(fs::path(path).filename()); }

// The family library: photos in dated folders, a renamed copy of one, two
// identical files in different folders, and files of the card's sizes with
// other bytes.
void make_family(rig& r) {
  write_bytes(r.lib() / "2024/2024-05-17/IMG_0001.HEIC", pattern(300'000, 1));
  write_bytes(r.lib() / "From Mum/holiday 3.heic", pattern(310'000, 2));  // the card's IMG_0002, renamed
  write_bytes(r.lib() / "2023/beach.jpg", pattern(250'000, 7));
  write_bytes(r.lib() / "Old backup/beach copy.jpg", pattern(250'000, 7));   // a duplicate inside the library
  write_bytes(r.lib() / "2022/other.mov", pattern(400'000, 8));             // the card's clip size, other bytes
  write_bytes(r.lib() / "2022/notes.txt", pattern(1'000, 9));
}
void make_card(rig& r) {
  write_bytes(r.card() / "DCIM/IMG_0001.HEIC", pattern(300'000, 1));  // in the library, same name
  write_bytes(r.card() / "DCIM/IMG_0002.HEIC", pattern(310'000, 2));  // in the library, renamed
  write_bytes(r.card() / "DCIM/MVI_0003.MOV", pattern(400'000, 3));   // same size, other bytes: new
  write_bytes(r.card() / "DCIM/IMG_0004.JPG", pattern(123'456, 4));   // no size match: new, never read
}

}  // namespace

TEST_CASE("check library finds a source's files by content, renamed or not, and lists duplicates",
          "[import][check_library]") {
  rig r;
  make_family(r);
  make_card(r);
  const auto s = r.check(r.folder_request());
  REQUIRE(*s.str("kind") == "check");
  REQUIRE(*s.boolean("reachable"));
  REQUIRE(*s.boolean("walk_complete"));
  REQUIRE(*s.integer("library_files") == 6);
  REQUIRE(*s.integer("source_files") == 4);
  REQUIRE(*s.integer("found_count") == 2);
  const auto& found = s.find("found")->a;
  std::vector<std::string> found_names;
  for (const auto& f : found) found_names.push_back(name_of(*f.str("path")));
  std::sort(found_names.begin(), found_names.end());
  REQUIRE(found_names == std::vector<std::string>{"IMG_0001.HEIC", "IMG_0002.HEIC"});
  for (const auto& f : found) {
    const auto where = strings(f.find("where"));
    REQUIRE(where.size() == 1);
    if (name_of(*f.str("path")) == "IMG_0002.HEIC") REQUIRE(name_of(where[0]) == "holiday 3.heic");
  }
  const auto missing = strings(s.find("not_found"));
  REQUIRE(missing.size() == 2);
  REQUIRE(*s.integer("not_found_bytes") == 400'000 + 123'456);
  // Duplicates already in the library: beach.jpg and its copy.
  REQUIRE(*s.integer("duplicate_groups") == 1);
  const auto group = strings(s.find("duplicates")->a[0].find("paths"));
  REQUIRE(group.size() == 2);
  // Only sizes that matter were read: the library's 300k, 310k, 400k and the
  // two 250k files (a repeated size); the card's three size matches. Never
  // notes.txt or IMG_0004.
  REQUIRE(*s.integer("hashed") == 5);
  REQUIRE(g_hashes == 5 + 3);
  // Report only: nothing written to either side.
  REQUIRE(list_tree(r.lib()).size() == 6);
  REQUIRE(list_tree(r.card()).size() == 4);
}

TEST_CASE("a second check of an unchanged library reads no file", "[import][check_library]") {
  rig r;
  make_family(r);
  make_card(r);
  (void)r.check(r.folder_request());
  r.restart();  // remembered in import.db, not in memory
  g_hashes = 0;
  const auto s = r.check(r.folder_request());
  REQUIRE(g_hashes == 0);
  REQUIRE(*s.integer("hashed") == 0);
  REQUIRE(*s.integer("remembered") == 5);
  REQUIRE(*s.integer("found_count") == 2);

  // A file added to the library: only it is read.
  write_bytes(r.lib() / "2025/IMG_0004 from the card.jpg", pattern(123'456, 4));
  g_hashes = 0;
  const auto t = r.check(r.folder_request());
  REQUIRE(*t.integer("hashed") == 1);
  REQUIRE(*t.integer("found_count") == 3);
  REQUIRE(g_hashes == 2);  // that file, and IMG_0004 on the card (now a size match)
}

TEST_CASE("an offline library says nothing and forgets nothing", "[import][check_library]") {
  rig r;
  make_family(r);
  make_card(r);
  (void)r.check(r.folder_request());
  // The share goes away: its folder no longer answers.
  fs::rename(r.lib(), r.dir / "Family (away)");
  const auto s = r.check(r.folder_request());
  REQUIRE_FALSE(*s.boolean("reachable"));
  REQUIRE(*s.integer("found_count") == 0);
  REQUIRE(*s.integer("dropped") == 0);
  // Back: every hash is still known.
  fs::rename(r.dir / "Family (away)", r.lib());
  g_hashes = 0;
  const auto t = r.check(r.folder_request());
  REQUIRE(*t.integer("remembered") == 5);
  REQUIRE(*t.integer("hashed") == 0);
}

TEST_CASE("a file deleted from the library leaves the index only when its folder answers",
          "[import][check_library]") {
  rig r;
  make_family(r);
  make_card(r);
  (void)r.check(r.folder_request());
  fs::remove(r.lib() / "From Mum/holiday 3.heic");  // deleted; the folder stays
  fs::remove_all(r.lib() / "2022");                  // a whole folder gone: rows kept
  const auto s = r.check(r.folder_request());
  REQUIRE(*s.integer("dropped") == 1);
  REQUIRE(*s.integer("found_count") == 1);  // only IMG_0001 now
}

TEST_CASE("library scope finds a file in another destination Import knows", "[import][check_library]") {
  rig r;
  make_card(r);
  write_bytes(r.lib() / "a.jpg", pattern(5'000, 50));
  // Another library folder, already indexed by a check of its own.
  const fs::path other = r.dir / "Archive";
  write_bytes(other / "old/IMG_0002 (archive).HEIC", pattern(310'000, 2));
  {
    mv::json::writer w;
    w.begin_object();
    w.key("destination").string(utf8(other));
    w.key("source_sizes").begin_array().integer(310'000).end_array();
    w.end_object();
    (void)r.check(w.take());
  }
  const auto here = r.check(r.folder_request("destination"));
  REQUIRE(*here.integer("found_count") == 0);
  const auto everywhere = r.check(r.folder_request("library"));
  REQUIRE(*everywhere.integer("found_count") == 1);
  REQUIRE(name_of(strings(everywhere.find("found")->a[0].find("where"))[0]) == "IMG_0002 (archive).HEIC");
}

TEST_CASE("library_find counts by size and confirms by hash; library_record teaches the index",
          "[import][check_library]") {
  rig r;
  make_family(r);
  {
    mv::json::writer w;
    w.begin_object();
    w.key("destination").string(utf8(r.lib()));
    w.key("source_sizes").begin_array().integer(310'000).end_array();
    w.end_object();
    const auto s = r.check(w.take());
    REQUIRE(strings(s.find("matched_sizes")).empty() == false);
  }
  const auto ask = [&](std::uint64_t size, const std::string& hash) {
    mv::json::writer w;
    w.begin_object();
    w.key("destination").string(utf8(r.lib()));
    w.key("size").integer(static_cast<std::int64_t>(size));
    if (!hash.empty()) w.key("hash").string(hash);
    w.end_object();
    auto text = r.eng->library_find(w.take());
    REQUIRE(text);
    auto v = mv::json::parse(*text);
    REQUIRE(v);
    return *v;
  };
  const std::string right = mv::io::hash_bytes(pattern(310'000, 2)).hex();
  const std::string wrong = mv::io::hash_bytes(pattern(310'000, 3)).hex();
  REQUIRE(*ask(310'000, "").integer("candidates") == 1);
  REQUIRE(strings(ask(310'000, right).find("paths")).size() == 1);
  REQUIRE(strings(ask(310'000, wrong).find("paths")).empty());

  // A file written by another tool: recorded, then found without a read.
  const fs::path added = r.lib() / "2025/new.heic";
  write_bytes(added, pattern(77'000, 77));
  const auto st = mv::io::stat_path(utf8(added));
  REQUIRE(st);
  mv::json::writer w;
  w.begin_object();
  w.key("destination").string(utf8(r.lib()));
  w.key("path").string(utf8(added));
  w.key("size").integer(77'000);
  w.key("mtime").integer(st->mtime_unix);
  w.key("hash").string(mv::io::hash_bytes(pattern(77'000, 77)).hex());
  w.end_object();
  REQUIRE(r.eng->library_record(w.take()));
  g_hashes = 0;
  REQUIRE(strings(ask(77'000, mv::io::hash_bytes(pattern(77'000, 77)).hex()).find("paths")).size() == 1);
  REQUIRE(g_hashes == 0);
  // A record that does not describe the file now is refused.
  mv::json::writer bad;
  bad.begin_object();
  bad.key("destination").string(utf8(r.lib()));
  bad.key("path").string(utf8(added));
  bad.key("size").integer(1);
  bad.key("mtime").integer(st->mtime_unix);
  bad.key("hash").string(mv::io::hash_bytes(pattern(77'000, 77)).hex());
  bad.end_object();
  REQUIRE_FALSE(r.eng->library_record(bad.take()));
}

// ---- the Mac's Photos backup over the same engine --------------------------

namespace {

struct photo {
  bk::asset_file meta;
  std::vector<std::uint8_t> bytes;
  bool local = true;
  std::string path;
};

class fake_photos final : public bk::source {
 public:
  std::vector<photo> files;
  std::atomic<int> fetches{0};
  mv::result<std::vector<bk::asset_file>> enumerate(const std::atomic<bool>*) override {
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
  mv::expected fetch(const bk::asset_file& file, const std::string& tmp, const std::atomic<bool>*) override {
    ++fetches;
    for (const auto& f : files) {
      if (f.meta.id == file.id && f.meta.kind == file.kind) {
        write_bytes(fs::path(tmp), f.bytes);
        return {};
      }
    }
    return mv::err(mv::status::not_found);
  }
};

constexpr std::int64_t kMay = 1715940000;  // 2024-05-17 10:00 UTC

photo make_photo(const fs::path& dir, const char* id, const char* name, std::size_t size, std::uint32_t seed,
                 bool local) {
  photo p;
  p.meta.id = id;
  p.meta.filename = name;
  p.meta.created_unix = kMay;
  p.meta.size = size;
  p.bytes = pattern(size, seed);
  p.local = local;
  p.path = utf8(dir / (std::string(id) + "-" + name));
  write_bytes(fs::path(p.path), p.bytes);
  return p;
}

// The owner's case: an iCloud library, some of it already in the family
// library (imported from a card, sent by the phone, copied by hand).
std::unique_ptr<fake_photos> photos_of(const fs::path& lib_dir) {
  auto src = std::make_unique<fake_photos>();
  src->files.push_back(make_photo(lib_dir, "A", "IMG_0001.HEIC", 300'000, 1, true));   // in the family library
  src->files.push_back(make_photo(lib_dir, "B", "IMG_0002.HEIC", 310'000, 2, false));  // there renamed; iCloud only
  src->files.push_back(make_photo(lib_dir, "C", "IMG_0010.MOV", 400'000, 30, true));   // a size match, other bytes
  src->files.push_back(make_photo(lib_dir, "D", "IMG_0011.JPG", 66'000, 31, false));   // new
  return src;
}

bk::options backup_options(rig& r, std::shared_ptr<bk::library> index, bool check_only = false) {
  bk::options o;
  o.destination = utf8(r.lib());
  o.scratch = utf8(r.dir / "scratch");
  o.index = std::move(index);
  o.check_only = check_only;
  return o;
}

bk::progress run(bk::engine& e) {
  e.join();
  return e.snapshot();
}

}  // namespace

TEST_CASE("Check library for Photos reports what is there, what would be copied, and writes nothing",
          "[import][check_library][photos_backup]") {
  rig r;
  make_family(r);
  scratch_dir photos_dir("checklib_photos");
  auto index = bk::make_import_library(&r.import);
  REQUIRE(index);
  auto src = photos_of(photos_dir.root());
  fake_photos* raw = src.get();
  bk::engine e;
  REQUIRE(e.start(std::move(src), backup_options(r, index, true)));
  const bk::progress p = run(e);
  REQUIRE(p.state == bk::run_state::done);
  const bk::check_report rep = e.last_check();
  REQUIRE(rep.index);
  REQUIRE(rep.originals == 4);
  REQUIRE(rep.in_library.size() == 1);  // A, read locally and matched
  REQUIRE(rep.in_library[0].name == "IMG_0001.HEIC");
  REQUIRE(rep.unconfirmed == 1);  // B: iCloud only, its size matches; never downloaded to check
  REQUIRE(rep.to_copy == 2);      // C (same size, other bytes) and D
  REQUIRE(rep.duplicates.size() == 1);
  REQUIRE(raw->fetches == 0);
  // Nothing written to the library, not even the backup's own folder.
  REQUIRE(list_tree(r.lib()).size() == 6);
  REQUIRE_FALSE(fs::exists(fs::path(bk::housekeeping_dir(utf8(r.lib())))));
  const auto json = mv::json::parse(bk::check_report_json(rep));
  REQUIRE(json);
  REQUIRE(*json->integer("to_copy") == 2);
}

TEST_CASE("Back Up Now skips photos already in the library by content and records them as found",
          "[import][check_library][photos_backup]") {
  rig r;
  make_family(r);
  scratch_dir photos_dir("checklib_photos2");
  auto index = bk::make_import_library(&r.import);
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.state == bk::run_state::done);
    REQUIRE(p.found == 2);  // A in place, B renamed under "From Mum"
    REQUIRE(p.done == 2);   // C and D copied
    REQUIRE(p.failed == 0);
  }
  // No second copy of A or B anywhere.
  const auto tree = list_tree(r.lib());
  REQUIRE(std::count_if(tree.begin(), tree.end(), [](const std::string& s) { return s.find("IMG_0001") != std::string::npos; }) == 1);
  REQUIRE(std::count_if(tree.begin(), tree.end(), [](const std::string& s) { return s.find("IMG_0002") != std::string::npos; }) == 0);

  // The copies joined Import's index: a card holding D is skipped by Import too.
  g_hashes = 0;
  {
    auto again = photos_of(photos_dir.root());
    fake_photos* raw = again.get();
    bk::engine e;
    REQUIRE(e.start(std::move(again), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.done == 0);
    REQUIRE(p.found == 0);
    REQUIRE(p.skipped == 4);  // copied or found before; found ones confirmed through the index
    REQUIRE(raw->fetches == 0);
  }
  REQUIRE(g_hashes == 0);  // the second run read no library file

  // Someone reorganises: B's file moves to another folder. Still found, not copied.
  fs::create_directories(r.lib() / "Sorted");
  fs::rename(r.lib() / "From Mum/holiday 3.heic", r.lib() / "Sorted/holiday 3.heic");
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.done == 0);
    REQUIRE(p.skipped == 4);
  }
}

TEST_CASE("a backed-up file moved inside the library is found where it went, not copied again",
          "[import][check_library][photos_backup]") {
  rig r;
  fs::create_directories(r.lib());
  scratch_dir photos_dir("checklib_photos3");
  auto index = bk::make_import_library(&r.import);
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    REQUIRE(run(e).done == 4);
  }
  // Someone files D under an album folder with a new name.
  const fs::path day = fs::path(bk::layout_dir(utf8(r.lib()), kMay));
  fs::create_directories(r.lib() / "Albums/Spring");
  fs::rename(day / "IMG_0011.JPG", r.lib() / "Albums/Spring/the garden.jpg");
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index, true)));
    REQUIRE(run(e).state == bk::run_state::done);
    const bk::check_report rep = e.last_check();
    REQUIRE(rep.gone.size() == 1);
    REQUIRE(name_of(rep.gone[0].moved_to) == "the garden.jpg");
  }
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.done == 0);
    REQUIRE(p.found == 1);
  }
  REQUIRE_FALSE(fs::exists(day / "IMG_0011.JPG"));

  // The found file is edited in place (another size): copied again, as a
  // copied file would be, since its bytes are nowhere in the library now.
  write_bytes(r.lib() / "Albums/Spring/the garden.jpg", pattern(70'000, 99));
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.done == 1);
    REQUIRE(p.held == 0);
  }
}

TEST_CASE("without Import the backup is as before", "[import][check_library][photos_backup]") {
  rig r;
  make_family(r);
  scratch_dir photos_dir("checklib_photos4");
  bk::engine e;
  REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, nullptr)));
  const bk::progress p = run(e);
  REQUIRE(p.done == 4);
  REQUIRE(p.found == 0);
  REQUIRE(bk::make_import_library(nullptr) == nullptr);
  mv_import_api old{};
  old.struct_size = static_cast<uint32_t>(offsetof(mv_import_api, check_library));
  REQUIRE(bk::make_import_library(&old) == nullptr);
}

// The cost of a first check (every matching size hashed) and of a second one
// (walk + lookups only) on a large synthetic library. Hidden: it writes a few
// GB. MV_CHECK_BENCH_FILES sets the count (default 20,000 files, 64-192 kB
// each, in 200 dated folders); the source holds every library size, the
// worst case for a first check. Prints one JSON line.
TEST_CASE("check library cost, first and second check", "[.perf-bench][check_library]") {
  rig r;
  std::size_t n = 20'000;
  if (const char* e = std::getenv("MV_CHECK_BENCH_FILES")) n = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
  std::vector<std::uint64_t> sizes;
  sizes.reserve(n);
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t size = 64'000 + (i * 7919) % 128'000;
    char folder[32];
    std::snprintf(folder, sizeof folder, "%04zu/%04zu-%02zu", 2000 + i % 20, 2000 + i % 20, i % 10);
    write_bytes(r.lib() / folder / ("IMG_" + std::to_string(i) + ".JPG"), pattern(size, static_cast<std::uint32_t>(i)));
    sizes.push_back(size);
    total += size;
  }
  mv::json::writer w;
  w.begin_object();
  w.key("destination").string(utf8(r.lib()));
  w.key("source_sizes").begin_array();
  for (const auto s : sizes) w.integer(static_cast<std::int64_t>(s));
  w.end_array();
  w.key("history").boolean(false);
  w.end_object();
  const std::string request = w.take();
  const auto timed = [&] {
    const auto t0 = std::chrono::steady_clock::now();
    const auto s = r.check(request);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return std::make_pair(ms, *s.integer("hashed"));
  };
  g_hashes = 0;
  const auto first = timed();
  r.restart();
  g_hashes = 0;
  const auto second = timed();
  REQUIRE(second.second == 0);
  REQUIRE(g_hashes == 0);
  r.restart();
  const auto third = timed();
  std::printf("{\"bench\":\"check_library\",\"files\":%zu,\"bytes\":%llu,\"first_ms\":%lld,\"first_hashed\":%lld,"
              "\"second_ms\":%lld,\"second_hashed\":%lld,\"third_ms\":%lld}\n",
              n, static_cast<unsigned long long>(total), static_cast<long long>(first.first),
              static_cast<long long>(first.second), static_cast<long long>(second.first),
              static_cast<long long>(second.second), static_cast<long long>(third.first));
}

TEST_CASE("Check library on an existing backup changes nothing in the destination, the manifest included",
          "[import][check_library][photos_backup]") {
  rig r;
  make_family(r);
  scratch_dir photos_dir("checklib_photos5");
  auto index = bk::make_import_library(&r.import);
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    REQUIRE(run(e).state == bk::run_state::done);
  }
  const fs::path keep(bk::housekeeping_dir(utf8(r.lib())));
  const auto before_bytes = read_bytes(keep / "manifest.sqlite");
  const auto before_list = list_tree(keep);
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index, true)));
    REQUIRE(run(e).state == bk::run_state::done);
  }
  REQUIRE(read_bytes(keep / "manifest.sqlite") == before_bytes);
  REQUIRE(list_tree(keep) == before_list);  // no -journal, no report
}

TEST_CASE("a photo found in another library stays found when the scope narrows, never held",
          "[import][check_library][photos_backup]") {
  rig r;
  fs::create_directories(r.lib());
  // B's bytes live in another library Import knows.
  const fs::path other = r.dir / "Archive";
  write_bytes(other / "IMG_0002 (archive).HEIC", pattern(310'000, 2));
  write_bytes(other / "notes.txt", pattern(500, 3));
  {
    mv::json::writer w;
    w.begin_object();
    w.key("destination").string(utf8(other));
    w.key("source_sizes").begin_array().integer(310'000).end_array();
    w.end_object();
    (void)r.check(w.take());
  }
  scratch_dir photos_dir("checklib_photos6");
  auto index = bk::make_import_library(&r.import);
  {
    bk::options o = backup_options(r, index);
    o.scope = bk::library_scope::library;
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), std::move(o)));
    const bk::progress p = run(e);
    REQUIRE(p.found == 1);
    REQUIRE(p.done == 3);
  }
  // "This folder" cannot see the archive: B is still found, not held.
  {
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
    const bk::progress p = run(e);
    REQUIRE(p.held == 0);
    REQUIRE(p.skipped == 4);
    REQUIRE(p.done == 0);
  }
  // The archive goes offline (its folder does not answer): still found.
  fs::rename(other, r.dir / "Archive (away)");
  {
    bk::options o = backup_options(r, index);
    o.scope = bk::library_scope::library;
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), std::move(o)));
    const bk::progress p = run(e);
    REQUIRE(p.held == 0);
    REQUIRE(p.skipped == 4);
  }
  // Offline with its mount point left behind empty: still found, never held.
  fs::create_directories(other);
  {
    bk::options o = backup_options(r, index);
    o.scope = bk::library_scope::library;
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), std::move(o)));
    const bk::progress p = run(e);
    REQUIRE(p.held == 0);
  }
  fs::remove(other);
  // Back, and deleted for real (its folder answers and holds other files): held.
  fs::rename(r.dir / "Archive (away)", other);
  fs::remove(other / "IMG_0002 (archive).HEIC");
  {
    bk::options o = backup_options(r, index);
    o.scope = bk::library_scope::library;
    bk::engine e;
    REQUIRE(e.start(photos_of(photos_dir.root()), std::move(o)));
    const bk::progress p = run(e);
    REQUIRE(p.held == 1);
    REQUIRE(p.done == 0);
  }
}

TEST_CASE("once the gate closes the backup stops calling Import and goes on without it",
          "[import][check_library][photos_backup]") {
  rig r;
  make_family(r);
  scratch_dir photos_dir("checklib_photos7");
  auto index = bk::make_import_library(&r.import);
  REQUIRE(bk::close_import_library(1.0));
  bk::engine e;
  REQUIRE(e.start(photos_of(photos_dir.root()), backup_options(r, index)));
  const bk::progress p = run(e);
  REQUIRE(p.state == bk::run_state::cancelled);  // a closed gate reads as a cancel: the run stops
  (void)bk::make_import_library(&r.import);       // reopens for the next tests
}
