// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #291: every tag searchable — the shared fold, the search-string builder, the
// match and its "why", meta_cache.db keying and invalidation, and the folder
// pass (tag_index) that fills it once, ever.
//   mv_tests "[search]"            the unit tests
//   mv_tests "[.perf-search]"      what one bounded search read costs against
//                                  meta::read and read_date_taken
#include <catch2/catch_test_macros.hpp>

#include <exiv2/exiv2.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "core/fold.h"
#include "core/job_system.h"
#include "fixtures.h"
#include "io/sort_order.h"
#include "meta/meta_cache.h"
#include "meta/search.h"
#include "meta/tag_index.h"

namespace {

namespace fs = std::filesystem;
namespace io = mv::io;
namespace meta = mv::meta;
using namespace std::chrono_literals;

io::dir_entry entry(const std::string& dir, std::string name, std::int64_t mtime = 1, std::uint64_t size = 10) {
  io::dir_entry e;
  e.name_utf8 = name;
  e.path_utf8 = dir + "/" + name;
  e.mtime_unix = mtime;
  e.size = size;
  return e;
}

struct temp_dir {
  fs::path path;
  explicit temp_dir(const char* name) : path(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  [[nodiscard]] std::string str() const { return path.string(); }
};

meta::property prop(const char* raw_tag, std::string value) {
  meta::property p;
  p.raw_tag = raw_tag;
  p.raw = p.value = std::move(value);
  return p;
}

// What a fake reader says about a file, by name.
mv::result<meta::search_facts> fake_facts(std::string_view path) {
  meta::search_facts f;
  if (path.find("r5") != std::string_view::npos) {
    f.camera = "Canon EOS R5";
    f.fields = {"Canon EOS R5", "RF24-70mm F2.8 L IS USM"};
    f.date_taken_key = 2000;
    f.where = meta::geo_point{38.7223, -9.1393, std::nullopt};
  } else if (path.find("gpt") != std::string_view::npos) {
    f.made_with = "ChatGPT";
    f.ai_generated = true;
    f.fields = {"ChatGPT"};
    f.date_taken_key = 1000;
  }
  return f;
}

bool wait_until(const std::function<bool()>& done) {
  for (int i = 0; i < 500; ++i) {
    if (done()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return done();
}

}  // namespace

// ---- the fold ---------------------------------------------------------------------

TEST_CASE("the shared fold is the hosts' name fold", "[search][fold]") {
  using mv::text::fold;
  CHECK(fold("Portugália") == "portugalia");
  CHECK(fold("PORTUGALIA") == "portugalia");
  CHECK(fold("Portuga\xCC\x81lia") == fold("Portugália"));  // decomposed (a Mac file name)
  CHECK(fold("José ÉLAN") == "jose elan");
  CHECK(fold("Straße") == "straße");  // no decomposition: stays, as on the hosts
  CHECK(fold("Łódź") == "łodz");
  CHECK(fold("Ærø") == "ærø");
  CHECK(fold("ΑΘΗΝΑ") == "αθηνα");
  CHECK(fold("Москва") == "москва");
  CHECK(fold("東京") == "東京");
  CHECK(fold(std::string("a\xFF" "B")) == std::string("a\xFF" "b"));  // invalid bytes pass through
}

// ---- the builder ------------------------------------------------------------------

TEST_CASE("the search fields come from the summary and the tags, once each", "[search]") {
  meta::metadata m;
  m.s.camera = "Canon EOS R5";
  m.s.lens = "RF24-70mm F2.8 L IS USM";
  m.s.made_with = "Adobe Firefly";
  m.s.ai_generated = true;
  m.s.date_taken_key = 1234;
  m.s.where = meta::geo_point{38.7, -9.1, 12.0};
  m.s.comment = "first light";
  m.properties = {
      prop("Exif.Image.Make", "Canon"),  // already in the camera line
      prop("Exif.Image.Software", "Firmware Version 1.8.1"),
      prop("Xmp.xmp.CreatorTool", "Adobe Lightroom"),
      prop("Xmp.dc.title", "lang=\"x-default\" Sintra at dusk"),
      prop("Xmp.dc.description", "lang=\"x-default\" The palace, from the hill"),
      prop("Xmp.dc.subject", "holiday, Pena, palace"),
      prop("Iptc.Application2.Keywords", "holiday"),  // a repeat
      prop("Iptc.Application2.Keywords", "family"),
      prop("Exif.Image.Artist", "Tristan"),
      prop("Exif.Photo.ExposureTime", "1/250"),  // not a search field
  };
  const meta::search_facts f = meta::search_facts_of(m);
  const std::vector<std::string> want = {"Canon EOS R5",   "RF24-70mm F2.8 L IS USM",
                                         "Adobe Firefly",  "Firmware Version 1.8.1",
                                         "Adobe Lightroom", "Sintra at dusk",
                                         "The palace, from the hill", "first light",
                                         "holiday",        "Pena",
                                         "family",         "Tristan"};
  CHECK(f.fields == want);  // "palace" is inside the caption already
  CHECK(f.camera == "Canon EOS R5");
  CHECK(f.made_with == "Adobe Firefly");
  CHECK(f.ai_generated);
  CHECK(f.date_taken_key == 1234);
  REQUIRE(f.where);
  CHECK(f.where->lat == 38.7);
}

TEST_CASE("a clip's container tags are search fields", "[search]") {
  meta::metadata m;
  m.is_clip = true;
  m.s.camera = "Apple iPhone 15 Pro";
  m.properties = {prop("Container.com.apple.quicktime.make", "Apple"),
                  prop("Container.com.apple.quicktime.software", "17.4"),
                  prop("Container.title", "Ferry to Cacilhas"),
                  prop("Container.encoder", "Lavf60.3.100")};  // not a field
  const meta::search_facts f = meta::search_facts_of(m);
  CHECK(f.fields == std::vector<std::string>{"Apple iPhone 15 Pro", "17.4", "Ferry to Cacilhas"});
}

TEST_CASE("camera placeholders and empty tags make no field", "[search]") {
  meta::metadata m;
  m.properties = {prop("Exif.Image.ImageDescription", "OLYMPUS DIGITAL CAMERA"),
                  prop("Exif.Image.Artist", "   "), prop("Xmp.dc.title", "lang=\"x-default\" ")};
  CHECK(meta::search_facts_of(m).fields.empty());
}

// ---- match and why ----------------------------------------------------------------

TEST_CASE("every word must match; the why is the first word's field", "[search]") {
  const std::vector<std::string> fields = {"Canon EOS R5", "RF24-70mm", "ChatGPT", "Sintra at dusk"};
  const std::string folded = meta::folded_text(fields);
  CHECK(folded == "canon eos r5\x1frf24-70mm\x1f" "chatgpt\x1fsintra at dusk");
  const auto at = [&](std::string_view q) { return meta::match_fields(folded, meta::search_words(q)); };
  CHECK(at("R5") == 0);
  CHECK(at("chatgpt") == 2);
  CHECK(at("CHATGPT") == 2);
  CHECK(at("dusk canon") == 3);  // both, in any field; why = the first word's
  CHECK(at("canon nikon") == -1);
  CHECK(at("r5\x1frf") == 0);  // a separator splits words: "r5" and "rf", each in a field
  CHECK(at("   ") == -1);
  CHECK(meta::why_of(fields, 0) == "Canon EOS R5");
  CHECK(meta::why_of(fields, 9).empty());
  const std::vector<std::string> longer = {std::string(60, 'x')};
  CHECK(meta::why_of(longer, 0, 10) == std::string(10, 'x') + "\xE2\x80\xA6");
}

TEST_CASE("accents and case fold the same on both sides", "[search]") {
  const std::vector<std::string> fields = {"Café Lisboa"};
  const std::string folded = meta::folded_text(fields);
  CHECK(meta::match_fields(folded, meta::search_words("CAFE")) == 0);
  CHECK(meta::match_fields(folded, meta::search_words("café")) == 0);
}

// ---- meta_cache.db ------------------------------------------------------------------

TEST_CASE("meta_cache rows are keyed by path, size and mtime", "[search][cache]") {
  temp_dir tmp("mv_meta_cache_keying");
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  meta::cached_facts r;
  r.path = "/photos/a.jpg";
  r.size = 100;
  r.mtime = 50;
  r.facts.fields = {"Canon EOS R5", "holiday"};
  r.facts.camera = "Canon EOS R5";
  r.facts.ai_generated = true;
  r.facts.date_taken_key = 777;
  r.facts.where = meta::geo_point{38.7, -9.1, std::nullopt};
  r.folded = meta::folded_text(r.facts.fields);
  REQUIRE(cache.store(std::span<const meta::cached_facts>(&r, 1)));

  auto hit = cache.lookup("/photos/a.jpg", 100, 50);
  REQUIRE(hit);
  CHECK(hit->facts.fields == r.facts.fields);
  CHECK(hit->folded == r.folded);
  CHECK(hit->facts.camera == "Canon EOS R5");
  CHECK(hit->facts.ai_generated);
  CHECK(hit->facts.date_taken_key == 777);
  REQUIRE(hit->facts.where);
  CHECK(hit->facts.where->lon == -9.1);
  CHECK_FALSE(hit->facts.where->altitude);
  // Rewritten in place: another size or mtime is a miss.
  CHECK_FALSE(cache.lookup("/photos/a.jpg", 101, 50));
  CHECK_FALSE(cache.lookup("/photos/a.jpg", 100, 51));
  // The folder's rows, not a sub-folder's.
  meta::cached_facts sub = r;
  sub.path = "/photos/sub/b.jpg";
  REQUIRE(cache.store(std::span<const meta::cached_facts>(&sub, 1)));
  CHECK(cache.folder("/photos").size() == 1);
  CHECK(cache.folder("/photos/sub").size() == 1);
  // Forget (a write that landed in a sidecar).
  cache.forget("/photos/a.jpg");
  CHECK_FALSE(cache.lookup("/photos/a.jpg", 100, 50));
  CHECK(cache.rows() == 1);
}

TEST_CASE("meta_cache survives a reopen and remakes a damaged file", "[search][cache]") {
  temp_dir tmp("mv_meta_cache_reopen");
  {
    meta::meta_cache cache;
    REQUIRE(cache.open(tmp.str()));
    meta::cached_facts r;
    r.path = "/p/x.jpg";
    r.size = 1;
    r.mtime = 1;
    REQUIRE(cache.store(std::span<const meta::cached_facts>(&r, 1)));
  }
  {
    meta::meta_cache cache;
    REQUIRE(cache.open(tmp.str()));
    CHECK(cache.lookup("/p/x.jpg", 1, 1));
  }
  {
    std::ofstream junk(tmp.path / "meta_cache.db", std::ios::binary | std::ios::trunc);
    junk << "this is not a database, it is a cache that got damaged";
  }
  std::error_code ec;
  fs::remove(tmp.path / "meta_cache.db-wal", ec);
  fs::remove(tmp.path / "meta_cache.db-shm", ec);
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));  // made again, not a failure
  CHECK_FALSE(cache.lookup("/p/x.jpg", 1, 1));
  CHECK(cache.rows() == 0);
}

// #291 review: a second process holding the file (a second window) is busy,
// not damage. The open fails for now; the rows are not deleted under it.
TEST_CASE("meta_cache never deletes a cache another connection has locked", "[search][cache]") {
  temp_dir tmp("mv_meta_cache_busy");
  {
    meta::meta_cache cache;
    REQUIRE(cache.open(tmp.str()));
    meta::cached_facts r;
    r.path = "/p/x.jpg";
    r.size = 1;
    r.mtime = 1;
    REQUIRE(cache.store(std::span<const meta::cached_facts>(&r, 1)));
  }
  const std::string db = (tmp.path / "meta_cache.db").string();
  sqlite3* other = nullptr;
  REQUIRE(sqlite3_open(db.c_str(), &other) == SQLITE_OK);
  // An exclusive-mode WAL connection with a write open: no one else gets in.
  REQUIRE(sqlite3_exec(other, "PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
  {
    meta::meta_cache cache;
    CHECK_FALSE(cache.open(tmp.str()));
  }
  REQUIRE(sqlite3_exec(other, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(other);
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  CHECK(cache.lookup("/p/x.jpg", 1, 1));  // still there
}

// #291 review: files gone from a folder leave meta_cache.db when the folder is
// seen again, so revisiting a folder does not grow it; a file only left out
// of the listing (Settings hides its kind) keeps its row.
TEST_CASE("the folder pass drops the rows of files that are gone", "[search][pass][cache]") {
  temp_dir tmp("mv_tag_index_prune");
  const fs::path shoot = tmp.path / "shoot";
  fs::create_directories(shoot);
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  std::vector<io::dir_entry> files;
  for (const char* name : {"a.jpg", "b.jpg", "c.mp3"}) {
    std::ofstream(shoot / name) << "x";
    files.push_back(entry(shoot.string(), name));
  }
  {
    meta::tag_index index(&cache, fake_facts);
    index.scan(shoot.string(), files, jobs);
    REQUIRE(wait_until([&] { return !index.progress().reading; }));
  }
  CHECK(cache.folder(shoot.string()).size() == 3);
  fs::remove(shoot / "b.jpg");
  // b is deleted; c.mp3 is hidden by a setting but still on disk.
  std::vector<io::dir_entry> now{files[0]};
  meta::tag_index index(&cache, fake_facts);
  index.scan(shoot.string(), now, jobs);
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  std::set<std::string> left;
  for (const auto& r : cache.folder(shoot.string())) left.insert(r.path);
  CHECK(left == std::set<std::string>{files[0].path_utf8, files[2].path_utf8});
  CHECK(index.reads() == 0);
  // c shown again: served from memory, not read.
  index.scan(shoot.string(), {files[0], files[2]}, jobs);
  CHECK_FALSE(index.progress().reading);
  CHECK(index.reads() == 0);
  jobs.shutdown();
}

TEST_CASE("cached_or_read reads a file once, then serves the cache until it changes", "[search][cache]") {
  temp_dir tmp("mv_meta_cache_cor");
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  const std::string file = (tmp.path / "plain.png").string();
  {
    const std::vector<std::uint8_t> px(4 * 4 * 4, 200);
    const auto png = fixtures::png_rgba(4, 4, px.data());
    std::ofstream f(file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  }
  REQUIRE(meta::cached_or_read(cache, file));
  CHECK(cache.rows() == 1);
  auto again = meta::cached_or_read(cache, file);
  REQUIRE(again);
  CHECK(again->path == file);
}

// ---- the folder pass -----------------------------------------------------------------

TEST_CASE("the folder pass reads each file once, ever, and matches with a why", "[search][pass]") {
  temp_dir tmp("mv_tag_index_once");
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  const std::string dir = "/shoot";
  std::vector<io::dir_entry> files;
  for (int i = 0; i < 70; ++i) files.push_back(entry(dir, "IMG_" + std::to_string(i) + ".jpg"));
  files.push_back(entry(dir, "r5_portrait.jpg"));
  files.push_back(entry(dir, "gpt_render.png"));

  std::atomic<int> reads{0};
  const auto reader = [&](std::string_view p) {
    ++reads;
    return fake_facts(p);
  };
  {
    meta::tag_index index(&cache, reader);
    index.scan(dir, files, jobs);
    REQUIRE(wait_until([&] { return !index.progress().reading; }));
    CHECK(index.progress().done == files.size());
    CHECK(index.progress().total == files.size());
    CHECK(reads.load() == static_cast<int>(files.size()));

    auto hits = index.match("R5");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].path == dir + "/r5_portrait.jpg");
    CHECK(hits[0].why == "Canon EOS R5");
    hits = index.match("chatgpt");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].why == "ChatGPT");
    CHECK(index.match("r").empty());  // one letter matches nothing
    CHECK(index.match("leica").empty());

    // Date keys from the same pass: no second read.
    CHECK(index.date_key(files[70]) == 2000);
    CHECK(index.date_key(files[71]) == 1000);
    CHECK_FALSE(index.date_key(files[0]));
    CHECK(index.consume_changed());

    // Coordinates for locations_in.
    std::vector<std::string> located;
    index.each_location([&](const std::string& p, const meta::geo_point& w) {
      located.push_back(p);
      CHECK(w.lat == 38.7223);
      return true;
    });
    CHECK(located == std::vector<std::string>{dir + "/r5_portrait.jpg"});

    // The same folder again (a re-sort, a relist): nothing read.
    index.scan(dir, files, jobs);
    CHECK_FALSE(index.progress().reading);
    CHECK(reads.load() == static_cast<int>(files.size()));
  }
  // A new process: one SELECT, no file reads.
  meta::tag_index fresh(&cache, reader);
  fresh.scan(dir, files, jobs);
  REQUIRE(wait_until([&] { return !fresh.progress().reading; }));
  CHECK(reads.load() == static_cast<int>(files.size()));
  CHECK(fresh.reads() == 0);
  CHECK(fresh.match("R5").size() == 1);

  // One file rewritten in place: only it is read again.
  files[70].mtime_unix = 99;
  fresh.scan(dir, files, jobs);
  REQUIRE(wait_until([&] { return !fresh.progress().reading; }));
  CHECK(fresh.reads() == 1);
  CHECK(fresh.match("R5").size() == 1);

  // PR 12: a write that landed in a sidecar.
  fresh.invalidate(files[71].path_utf8);
  CHECK(fresh.match("chatgpt").empty());
  CHECK(fresh.progress().done == files.size() - 1);
  fresh.scan(dir, files, jobs);
  REQUIRE(wait_until([&] { return !fresh.progress().reading; }));
  CHECK(fresh.reads() == 2);
  CHECK(fresh.match("chatgpt").size() == 1);
  jobs.shutdown();
}

TEST_CASE("the folder pass waits while the viewer is busy and yields to a newer folder", "[search][pass]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  std::atomic<bool> busy{true};
  std::atomic<int> reads{0};
  meta::tag_index index(nullptr, [&](std::string_view p) {
    ++reads;
    return fake_facts(p);
  });
  std::vector<io::dir_entry> a;
  for (int i = 0; i < 100; ++i) a.push_back(entry("/a", "f" + std::to_string(i) + ".jpg"));
  index.scan("/a", a, jobs, {}, [&] { return busy.load(); });
  std::this_thread::sleep_for(100ms);
  CHECK(reads.load() == 0);  // nothing read while the viewer loads
  CHECK(index.progress().reading);
  busy = false;
  REQUIRE(wait_until([&] { return reads.load() > 0; }));

  // Another folder supersedes: /a stops at its next file.
  busy = true;
  const int at_switch = reads.load();
  std::vector<io::dir_entry> b{entry("/b", "r5.jpg")};
  index.scan("/b", b, jobs, {}, [] { return false; });
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  CHECK(index.folder() == "/b");
  CHECK(index.progress().total == 1);
  CHECK(index.match("r5").size() == 1);
  busy = false;
  std::this_thread::sleep_for(50ms);
  CHECK(reads.load() <= at_switch + 2);  // /a's chunk ended at its next file
  jobs.shutdown();
}

// #291 review: the pass steps aside for view-tied jobs, not only for the
// present loop: the photo's full decode and the +-2 prefetch run first.
TEST_CASE("the folder pass waits for view jobs and gives a queued one its worker", "[search][pass]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(1)));  // one worker: the pass must hand it over
  std::atomic<int> reads{0};
  meta::tag_index index(nullptr, [&](std::string_view p) {
    ++reads;
    std::this_thread::sleep_for(2ms);
    return fake_facts(p);
  });
  std::vector<io::dir_entry> a;
  for (int i = 0; i < 200; ++i) a.push_back(entry("/a", "f" + std::to_string(i) + ".jpg"));
  index.scan("/a", a, jobs, {}, [] { return false; });
  REQUIRE(wait_until([&] { return reads.load() > 0; }));
  // A view job arrives while the pass holds the only worker: it runs within
  // a file's read, not after the chunk or the folder.
  std::atomic<bool> ran{false};
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<std::int64_t> waited_ms{-1};
  (void)jobs.submit([&](const mv::job_context&) {
    waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ran = true;
    return mv::status::ok;
  });
  REQUIRE(wait_until([&] { return ran.load(); }));
  CHECK(waited_ms.load() < 40);
  CHECK(index.progress().reading);  // the pass is not done, it stepped aside
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  CHECK(reads.load() == 200);  // and resumed where it stopped: each file once
  CHECK(jobs.foreground_queued() == 0);
  CHECK(jobs.foreground_running() == 0);
  jobs.shutdown();
}

TEST_CASE("the folder pass does not read while a view job runs on another worker", "[search][pass]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  std::atomic<int> reads{0};
  meta::tag_index index(nullptr, [&](std::string_view p) {
    ++reads;
    return fake_facts(p);
  });
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  (void)jobs.submit([&](const mv::job_context&) {
    started = true;
    while (!release) std::this_thread::sleep_for(1ms);
    return mv::status::ok;
  });
  REQUIRE(wait_until([&] { return started.load(); }));
  std::vector<io::dir_entry> a;
  for (int i = 0; i < 50; ++i) a.push_back(entry("/a", "f" + std::to_string(i) + ".jpg"));
  index.scan("/a", a, jobs, {}, [] { return false; });
  std::this_thread::sleep_for(80ms);
  CHECK(reads.load() == 0);  // the second worker is free, but the view is decoding
  release = true;
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  CHECK(reads.load() == 50);
  jobs.shutdown();
}

TEST_CASE("the pass's date keys sort by date taken", "[search][pass][sort]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(1)));
  meta::tag_index index(nullptr, fake_facts);
  std::vector<io::dir_entry> v{entry("/d", "r5.jpg", 5), entry("/d", "gpt.png", 9), entry("/d", "none.jpg", 7)};
  index.scan("/d", v, jobs);
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  io::sort_entries(v, {io::sort_key::date_taken, false}, [&](const io::dir_entry& e) { return index.date_key(e); });
  CHECK(v[0].name_utf8 == "none.jpg");  // mtime 7 < 1000 < 2000
  CHECK(v[1].name_utf8 == "gpt.png");
  CHECK(v[2].name_utf8 == "r5.jpg");
  jobs.shutdown();
}

// ---- the bounded read on real bytes --------------------------------------------------

namespace {

std::vector<std::uint8_t> tagged_jpeg(int w, int h, bool chatgpt) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
  std::uint32_t s = 7;
  for (auto& b : rgb) {
    s = s * 1664525u + 1013904223u;
    b = static_cast<std::uint8_t>(s >> 24);
  }
  auto jpeg = fixtures::jpeg_rgb(w, h, rgb.data());
  auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
  Exiv2::ExifData exif;
  Exiv2::XmpData xmp;
  if (chatgpt) {
    xmp["Xmp.xmp.CreatorTool"] = "ChatGPT";
    xmp["Xmp.Iptc4xmpExt.DigitalSourceType"] =
        "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia";
  } else {
    exif["Exif.Image.Make"] = "Canon";
    exif["Exif.Image.Model"] = "Canon EOS R5";
    exif["Exif.Photo.LensModel"] = "RF24-70mm F2.8 L IS USM";
    exif["Exif.Photo.DateTimeOriginal"] = "2024:05:01 14:03:22";
    exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
    exif["Exif.GPSInfo.GPSLatitude"] = "38/1 47/1 4200/100";
    exif["Exif.GPSInfo.GPSLongitudeRef"] = "W";
    exif["Exif.GPSInfo.GPSLongitude"] = "9/1 23/1 1200/100";
    xmp["Xmp.dc.subject"] = "holiday";
    xmp["Xmp.dc.title"] = "lang=x-default Pena palace";
  }
  image->setExifData(exif);
  image->setXmpData(xmp);
  image->writeMetadata();
  Exiv2::BasicIo& bio = image->io();
  bio.open();
  Exiv2::DataBuf buf = bio.read(bio.size());
  return {buf.c_data(), buf.c_data() + buf.size()};
}

std::string write_file(const fs::path& p, const std::vector<std::uint8_t>& b) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return p.string();
}

}  // namespace

TEST_CASE("read_search_facts finds a camera, a lens, keywords, a place and an AI generator", "[search][read]") {
  temp_dir tmp("mv_search_read");
  const std::string camera = write_file(tmp.path / "camera.jpg", tagged_jpeg(640, 480, false));
  const std::string ai = write_file(tmp.path / "ai.jpg", tagged_jpeg(64, 64, true));

  auto c = meta::read_search_facts(camera);
  REQUIRE(c);
  CHECK(c->camera == "Canon EOS R5");
  CHECK(c->lens == "RF24-70mm F2.8 L IS USM");
  CHECK(c->date_taken_key != 0);
  REQUIRE(c->where);
  CHECK(c->where->lat > 38.79);
  CHECK(c->where->lon < -9.38);
  CHECK_FALSE(c->ai_generated);
  const std::string folded = meta::folded_text(c->fields);
  CHECK(meta::match_fields(folded, meta::search_words("R5")) == 0);
  CHECK(meta::match_fields(folded, meta::search_words("holiday")) >= 0);
  CHECK(meta::match_fields(folded, meta::search_words("pena")) >= 0);
  // The full read and the bounded read agree on what search needs.
  auto full = meta::read(camera);
  REQUIRE(full);
  CHECK(meta::search_facts_of(*full).fields == c->fields);

  auto a = meta::read_search_facts(ai);
  REQUIRE(a);
  CHECK(a->ai_generated);
  CHECK(a->made_with == "ChatGPT");
  CHECK(meta::match_fields(meta::folded_text(a->fields), meta::search_words("chatgpt")) >= 0);

  CHECK_FALSE(meta::read_search_facts((tmp.path / "missing.jpg").string()));
}

TEST_CASE("perf: one search read against a full read and a date read", "[.perf-search]") {
  const int rounds = std::getenv("MV_BENCH_ROUNDS") ? std::atoi(std::getenv("MV_BENCH_ROUNDS")) : 40;
  temp_dir tmp("mv_search_bench");
  const std::string file = write_file(tmp.path / "camera.jpg", tagged_jpeg(3000, 2000, false));
  const auto median = [&](const std::function<void()>& fn) {
    fn();
    std::vector<double> t;
    for (int i = 0; i < rounds; ++i) {
      const auto a = std::chrono::steady_clock::now();
      fn();
      t.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
  };
  const double full = median([&] { (void)meta::read(file); });
  const double search = median([&] { (void)meta::read_search_facts(file); });
  const double date = median([&] { (void)meta::read_date_taken(file); });
  std::printf("[perf-search] camera JPEG 6 MP: meta::read %.1f us, read_search_facts %.1f us, read_date_taken %.1f us\n",
              full, search, date);
}

// #291 review: fuzz_meta_search found a damaged AVIF-ish ISO-BMFF file that
// libavformat opens as a clip whose stream names a colour matrix FFmpeg has no
// name for; av_color_space_name() returned NULL and std::string(NULL)
// crashed. The folder pass reads every file of every folder shown, so one
// such file crashed the app without being opened.
TEST_CASE("a clip naming an unknown colour matrix does not crash the search read", "[search][read]") {
  static constexpr std::uint8_t kBytes[] = {
      0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x61, 0x70, 0x76, 0x69, 0x66, 0x00, 0x00, 0x00, 0x00,
      0x61, 0x76, 0x69, 0x66, 0x6d, 0x69, 0x66, 0x31, 0x6d, 0x69, 0x61, 0x66, 0x4d, 0x41, 0x31, 0x42,
      0x00, 0x00, 0x00, 0xeb, 0x6d, 0x65, 0x74, 0x61, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x21,
      0x68, 0x64, 0x6c, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x69, 0x63, 0x74,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0e, 0x70, 0x69, 0x74, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1e, 0x69,
      0x6c, 0x6f, 0x63, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x01, 0x13, 0x00, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0x28, 0x69, 0x69, 0x6e,
      0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x69, 0x6e, 0x66, 0x65, 0x02,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x61, 0x76, 0x30, 0x31, 0x43, 0x6f, 0x6c, 0x6f, 0x72,
      0x00, 0x00, 0x00, 0x00, 0x6a, 0x69, 0x26, 0x72, 0x70, 0x00, 0x00, 0x00, 0x4b, 0x69, 0x70, 0x63,
      0x6f, 0x00, 0x00, 0x00, 0x14, 0x69, 0x73, 0x70, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x30, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x70, 0x69, 0x78, 0x69, 0x00, 0x00, 0x00,
      0x00, 0x03, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x0c, 0x61, 0x76, 0x31, 0x43, 0x81, 0x00, 0x0c,
      0x00, 0x00, 0x00, 0x00, 0x13, 0x63, 0x6f, 0x6c, 0x72, 0x6e, 0x63, 0x6c, 0x78, 0x00, 0x01, 0x00,
      0x0d, 0x00, 0x06, 0x80, 0x00, 0x00, 0x00, 0x17, 0x69, 0x70, 0x6d, 0x61, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x02, 0x83, 0x04, 0x00, 0x00, 0x01, 0x02, 0x6d,
      0x64, 0x61, 0x74, 0x12, 0x00, 0x0a, 0x09, 0x18, 0x15, 0x2f, 0xfb, 0x44, 0x04, 0x33, 0x1a, 0x10,
      0x32, 0xea, 0x01, 0x19, 0x47, 0x87, 0x86, 0x21, 0x87, 0x9e, 0x79, 0xe6, 0x82, 0x40, 0x00, 0xf1,
      0x97, 0xe7, 0x97, 0xcc, 0x8e, 0xee, 0xd7, 0x8e, 0x8e, 0x46, 0xe7, 0x1f, 0xf2, 0x57, 0x0c, 0xd3,
      0xfb, 0xea, 0xce, 0x21, 0x42, 0xb4, 0xc6, 0x85, 0x2b, 0x9a, 0x96, 0x97, 0x7f, 0x44, 0x89, 0xec,
      0x1e, 0xb9, 0xf5, 0x32, 0x13, 0x70, 0x05, 0x02, 0x67, 0x1e, 0x4a, 0x4f, 0x9e, 0x8c, 0x7b, 0x43,
      0x94, 0xa2, 0x5f, 0x05, 0xfd, 0x15, 0x21, 0x7c, 0x1d, 0x8d, 0x55, 0xe1, 0xed, 0x9b, 0x17, 0x90,
      0x04, 0x85, 0xcc, 0x66, 0x71, 0x9e, 0x5c, 0x7c, 0x74, 0xfb, 0x81, 0x65, 0x87, 0x11, 0x53, 0x22,
      0xb0, 0x81, 0x51, 0xd5, 0xe5, 0x77, 0xbe, 0x80, 0x0e, 0xe6, 0x39, 0x3e, 0xf2, 0x42, 0xe9, 0x29,
      0x97, 0x9d, 0xab, 0xbb, 0x9d, 0x45, 0xa7, 0x13, 0x30, 0xde, 0xb4, 0x6f, 0x85, 0x1a, 0x7f, 0x47,
      0xac, 0xef, 0xc9, 0x19, 0x42, 0x4d, 0xe4, 0x59, 0xe6, 0xd0, 0x9f, 0xcf, 0xf0, 0x42, 0x66, 0xb4,
      0x83, 0xb6, 0x33, 0xe6, 0x1c, 0x28, 0xc6, 0x8d, 0xc8, 0x03, 0x2c, 0x19, 0x48, 0x4f, 0x77, 0xea,
      0x87, 0x07, 0x51, 0x09, 0x94, 0xec, 0x74, 0xc1, 0xd1, 0x8e, 0x6c, 0xd9, 0xeb, 0xf2, 0xc0, 0x88,
      0x12, 0xbf, 0x87, 0x7b, 0x40, 0x06, 0x70, 0xc2, 0x8f, 0x8e, 0xaa, 0xd8, 0x21, 0x5b, 0xca, 0xf2,
      0x33, 0xbe, 0xb5, 0x0f, 0x5b, 0x2d, 0x59, 0x4f, 0xa9, 0x7b, 0x88, 0xa7, 0xef, 0x44, 0x13, 0xf2,
      0x33, 0xcc, 0xde, 0x44, 0x48, 0x41, 0xe5, 0xf3, 0xbf, 0xc6, 0x34, 0xef, 0x49, 0x03, 0x57, 0x83,
      0x51, 0x50, 0x2a, 0x17, 0x16, 0x91, 0x31, 0x61, 0x19, 0x76, 0x32, 0x3b, 0x08};
  temp_dir tmp("mv_search_unknown_matrix");
  const std::string file = write_file(tmp.path / "damaged.avif",
                                      std::vector<std::uint8_t>(std::begin(kBytes), std::end(kBytes)));
  (void)meta::read_search_facts(file);
  (void)meta::read(file);
}

// #291 review: what a whole folder costs to sort by date taken the first time.
// MV_BENCH_DIR=<folder> mv_tests "[.perf-pass]": the old date scan (one
// read_date_taken per file, one background job) against the tag pass (cold:
// no meta_cache.db; warm: the second visit, one SELECT). Wall time to the last
// key; run it twice so the page cache is warm for both.
TEST_CASE("perf: a folder's date keys, the date scan against the tag pass", "[.perf-pass]") {
  const char* dir = std::getenv("MV_BENCH_DIR");
  REQUIRE(dir);
  std::vector<io::dir_entry> files;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    io::dir_entry d;
    d.name_utf8 = e.path().filename().string();
    d.path_utf8 = e.path().string();
    d.size = static_cast<std::uint64_t>(e.file_size());
    d.mtime_unix = std::chrono::duration_cast<std::chrono::seconds>(e.last_write_time().time_since_epoch()).count();
    files.push_back(std::move(d));
  }
  const auto secs = [](auto a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
  };
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(0)));
  // The date scan ran as one background job (meta_store::resolve_date_keys),
  // at the pool's background QoS, as the tag pass does.
  auto t = std::chrono::steady_clock::now();
  std::atomic<std::size_t> dated{0};
  std::atomic<bool> done{false};
  (void)jobs.submit_at(mv::background_generation, [&](const mv::job_context&) {
    for (const auto& e : files) dated += meta::read_date_taken(e.path_utf8) ? 1 : 0;
    done = true;
    return mv::status::ok;
  });
  while (!done) std::this_thread::sleep_for(1ms);
  const double date_scan = secs(t);
  temp_dir tmp("mv_perf_pass");
  meta::meta_cache cache;
  REQUIRE(cache.open(tmp.str()));
  double cold = 0, warm = 0;
  std::size_t keyed = 0;
  {
    meta::tag_index index(&cache);
    t = std::chrono::steady_clock::now();
    index.scan(dir, files, jobs);
    while (index.progress().reading) std::this_thread::sleep_for(1ms);
    cold = secs(t);
    for (const auto& e : files) keyed += index.date_key(e) ? 1 : 0;
  }
  {
    meta::tag_index index(&cache);
    t = std::chrono::steady_clock::now();
    index.scan(dir, files, jobs);
    while (index.progress().reading) std::this_thread::sleep_for(1ms);
    warm = secs(t);
    CHECK(index.reads() == 0);
  }
  jobs.shutdown();
  std::printf("[perf-pass] %zu files: date scan %.2f s (%.3f ms/file, %zu dated); tag pass cold %.2f s "
              "(%.3f ms/file, %zu dated), warm %.3f s\n",
              files.size(), date_scan, date_scan * 1000 / files.size(), dated.load(), cold, cold * 1000 / files.size(),
              keyed, warm);
}

// #291 review: the bounded search read must say what the full read says.
// MV_SEARCH_AGREE_DIR=<folder> mv_tests "[.search-agree]" compares
// read_search_facts with search_facts_of(meta::read) for every file in it and
// prints each difference (the repo's RAW / HEIC samples are the useful input).
TEST_CASE("the search read agrees with the full read", "[.search-agree]") {
  const char* dir = std::getenv("MV_SEARCH_AGREE_DIR");
  REQUIRE(dir);
  int files = 0, differ = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::string p = e.path().string();
    auto full = meta::read(p);
    auto quick = meta::read_search_facts(p);
    if (!full || !quick) continue;
    ++files;
    const meta::search_facts want = meta::search_facts_of(*full);
    const bool same = want.fields == quick->fields && want.camera == quick->camera && want.lens == quick->lens &&
                      want.made_with == quick->made_with && want.ai_generated == quick->ai_generated &&
                      want.date_taken_key == quick->date_taken_key &&
                      static_cast<bool>(want.where) == static_cast<bool>(quick->where);
    if (same) continue;
    ++differ;
    const auto join = [](const std::vector<std::string>& v) {
      std::string s;
      for (const auto& f : v) s += (s.empty() ? "" : " | ") + f;
      return s;
    };
    std::printf("[search-agree] %s (read_date_taken %lld)\n  full:  [%s] date %lld where %d\n  quick: [%s] date %lld where %d\n",
                e.path().filename().string().c_str(), static_cast<long long>(meta::read_date_taken(p).value_or(0)), join(want.fields).c_str(),
                static_cast<long long>(want.date_taken_key), want.where ? 1 : 0, join(quick->fields).c_str(),
                static_cast<long long>(quick->date_taken_key), quick->where ? 1 : 0);
  }
  std::printf("[search-agree] %d files, %d differ\n", files, differ);
  CHECK(differ == 0);
}

// ---- the Places provider's input ------------------------------------------------------

TEST_CASE("a place provider answers `portugal` from the pass's locations, read once", "[search][places]") {
  // The Places add-on (docs/design/27) answers place: / in: and plain words
  // through PR 59's search-provider slot, from the host table's locations_in,
  // which the hosts serve from this pass (no file read). This fake stands in
  // for it: a country by bounding box, the why its label.
  struct country {
    const char* name;
    double lat0, lat1, lon0, lon1;
  };
  static constexpr country kCountries[] = {
      {"Portugal", 36.9, 42.2, -9.6, -6.2},    // the mainland
      {"Portugal", 32.4, 33.2, -17.3, -16.2},  // Madeira
      {"Portugal", 36.9, 39.8, -31.3, -24.9},  // the Azores
      {"Spain", 36.0, 43.8, -9.3, 3.4},
  };
  const auto provider = [](const meta::tag_index& index, std::string_view typed) {
    const std::string q = mv::text::fold(typed);
    std::vector<std::pair<std::string, std::string>> rows;
    index.each_location([&](const std::string& path, const meta::geo_point& w) {
      for (const country& c : kCountries) {
        if (w.lat >= c.lat0 && w.lat <= c.lat1 && w.lon >= c.lon0 && w.lon <= c.lon1) {
          if (mv::text::fold(c.name) == q) rows.emplace_back(path, c.name);
          break;  // the first box wins (Portugal before Spain)
        }
      }
      return true;
    });
    return rows;
  };
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(1)));
  std::atomic<int> reads{0};
  meta::tag_index index(nullptr, [&](std::string_view p) -> mv::result<meta::search_facts> {
    ++reads;
    meta::search_facts f;
    if (p.find("sintra") != std::string_view::npos) f.where = meta::geo_point{38.7975, -9.3883, std::nullopt};
    if (p.find("funchal") != std::string_view::npos) f.where = meta::geo_point{32.6669, -16.9241, std::nullopt};
    if (p.find("madrid") != std::string_view::npos) f.where = meta::geo_point{40.4168, -3.7038, std::nullopt};
    if (p.find("sea") != std::string_view::npos) f.where = meta::geo_point{45.0, -20.0, std::nullopt};
    return f;
  });
  std::vector<io::dir_entry> files{entry("/trip", "sintra.jpg"), entry("/trip", "funchal.mov"),
                                   entry("/trip", "madrid.heic"), entry("/trip", "sea.jpg"),
                                   entry("/trip", "nogps.jpg")};
  index.scan("/trip", files, jobs);
  REQUIRE(wait_until([&] { return !index.progress().reading; }));
  const int read_once = reads.load();
  auto rows = provider(index, "PORTUGAL");
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].first == "/trip/sintra.jpg");
  CHECK(rows[1].first == "/trip/funchal.mov");
  CHECK(rows[0].second == "Portugal");
  CHECK(provider(index, "spain").size() == 1);
  CHECK(provider(index, "atlantis").empty());
  CHECK(reads.load() == read_once);  // the provider read nothing
  // The app's own tag text has no place names (owner, 2026-10-11).
  CHECK(index.match("portugal").empty());
  jobs.shutdown();
}

// ---- fixtures for the verify runs (not a test) ------------------------------------------

// MV_SEARCH_FIXTURE_DIR=<dir> [MV_SEARCH_FIXTURE_COPIES=20000] mv_tests "[.make-search-fixture]"
// writes camera_r5.jpg (Canon EOS R5, Sintra GPS), chatgpt.jpg (ChatGPT, AI),
// plain.jpg (no tags) and nikon.jpg (another camera), then COPIES numbered
// copies cycling through the four: the 20 k-file folder the soak runs on.
TEST_CASE("make the search verify fixture", "[.make-search-fixture]") {
  const char* out = std::getenv("MV_SEARCH_FIXTURE_DIR");
  REQUIRE(out);
  const fs::path dir(out);
  fs::create_directories(dir);
  std::vector<std::vector<std::uint8_t>> kinds;
  kinds.push_back(tagged_jpeg(1200, 800, false));
  kinds.push_back(tagged_jpeg(1200, 800, true));
  {
    std::vector<std::uint8_t> rgb(1200u * 800u * 3u, 90);
    kinds.push_back(fixtures::jpeg_rgb(1200, 800, rgb.data()));
  }
  {
    auto jpeg = kinds[2];
    auto image = Exiv2::ImageFactory::open(jpeg.data(), jpeg.size());
    Exiv2::ExifData exif;
    exif["Exif.Image.Make"] = "NIKON CORPORATION";
    exif["Exif.Image.Model"] = "NIKON Z 6";
    image->setExifData(exif);
    image->writeMetadata();
    Exiv2::BasicIo& bio = image->io();
    bio.open();
    Exiv2::DataBuf buf = bio.read(bio.size());
    kinds.emplace_back(buf.c_data(), buf.c_data() + buf.size());
  }
  const char* names[] = {"camera_r5.jpg", "chatgpt.jpg", "plain.jpg", "nikon.jpg"};
  for (int i = 0; i < 4; ++i) (void)write_file(dir / names[i], kinds[static_cast<std::size_t>(i)]);
  const int copies = std::getenv("MV_SEARCH_FIXTURE_COPIES") ? std::atoi(std::getenv("MV_SEARCH_FIXTURE_COPIES")) : 0;
  char name[64];
  for (int i = 0; i < copies; ++i) {
    std::snprintf(name, sizeof(name), "IMG_%05d.jpg", i);
    (void)write_file(dir / name, kinds[static_cast<std::size_t>(i % 4)]);
  }
}
