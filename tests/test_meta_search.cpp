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
