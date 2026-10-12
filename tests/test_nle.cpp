// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/23's portable half: the search agent's wire format (every length
// checked), the FCPXML a drag or an export writes, and the viewer's thumbnail
// cache read by another process without a write. The session over the AI
// pack's table is in test_ai_engine.cpp, beside the engine it compares with.
#include "catch_compat.h"

#include <sqlite3.h>

#include <mediaviewer/mediaviewer_ai.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nle/fcpxml.h"
#include "nle/search_wire.h"
#include "nle/thumb_reader.h"
#include "image/thumb.h"
#include "import_fixture.h"
#include "io/file_port.h"

using namespace mv::test;
using namespace mv::nle;

namespace {

reply sample() {
  reply r;
  r.correlation_id = 42;
  row clip;
  clip.path = "/Volumes/Card/DCIM/Birthday Cake & Candles.mp4";
  clip.thumb = "/Users/x/Library/Caches/MediaViewer/thumbs/00ff.jpg";
  clip.pts_ms = 10000;
  clip.duration_ms = 60000;
  clip.score = 0.31f;
  clip.kind = MV_AI_KIND_VIDEOS;
  clip.more = 2;
  clip.match = MV_AI_MATCH_PICTURE;
  clip.moments = {{9000, 0.28f}, {10000, 0.31f}, {40000, 0.25f}};
  row still;
  still.path = "C:\\Photos\\Été\\cake.jpg";
  still.kind = MV_AI_KIND_PHOTOS;
  still.score = 0.29f;
  r.rows = {clip, still};
  return r;
}

}  // namespace

TEST_CASE("the wire carries a reply exactly and rejects any buffer it cannot trust", "[nle][search-agent]") {
  const reply in = sample();
  const std::vector<std::uint8_t> bytes = encode(in);
  auto out = decode(bytes);
  REQUIRE(out);
  CHECK(out->correlation_id == 42);
  CHECK(out->code == mv::status::ok);
  REQUIRE(out->rows.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(out->rows[i].path == in.rows[i].path);
    CHECK(out->rows[i].thumb == in.rows[i].thumb);
    CHECK(out->rows[i].pts_ms == in.rows[i].pts_ms);
    CHECK(out->rows[i].duration_ms == in.rows[i].duration_ms);
    CHECK(out->rows[i].score == in.rows[i].score);
    CHECK(out->rows[i].kind == in.rows[i].kind);
    CHECK(out->rows[i].more == in.rows[i].more);
    CHECK(out->rows[i].match == in.rows[i].match);
    REQUIRE(out->rows[i].moments.size() == in.rows[i].moments.size());
  }
  CHECK(out->rows[0].moments[2].pts_ms == 40000);

  // Every truncation is refused, never read past.
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    CHECK_FALSE(decode(std::span<const std::uint8_t>(bytes.data(), n)));
  }
  // A path that runs past the blob (row 0's path_len: header 32 + 36).
  std::vector<std::uint8_t> bad = bytes;
  bad[32 + 36] = 0xFF;
  bad[32 + 39] = 0x7F;
  CHECK_FALSE(decode(bad));
  // Another version says so rather than guessing.
  std::vector<std::uint8_t> v2 = bytes;
  v2[4] = 2;
  auto future = decode(v2);
  REQUIRE_FALSE(future);
  CHECK(future.error() == mv::status::unsupported_format);
  // A request's fixed half.
  request q;
  q.kind = request_kind::similar;
  q.correlation_id = 9;
  q.pts_ms = 1234;
  q.max_results = 999999;
  auto rq = decode_request(encode(q));
  REQUIRE(rq);
  CHECK(rq->kind == request_kind::similar);
  CHECK(rq->correlation_id == 9);
  CHECK(rq->pts_ms == 1234);
  CHECK(rq->max_results == kMaxRows);  // clamped, never trusted
  auto qb = encode(q);
  qb[16] = 7;  // no such kind
  CHECK_FALSE(decode_request(qb));
  CHECK_FALSE(decode_request(std::span<const std::uint8_t>(qb.data(), qb.size() - 1)));
  // An empty reply with a reason.
  reply none;
  none.code = mv::status::not_found;
  auto back = decode(encode(none));
  REQUIRE(back);
  CHECK(back->rows.empty());
  CHECK(back->code == mv::status::not_found);
}

TEST_CASE("FCPXML: a moment is a range around the match with a marker, a still a clip, the query a keyword",
          "[nle]") {
  const reply r = sample();
  fcpxml_options o;
  o.event_name = "MediaViewer: cake";
  o.keyword = "MV: cake";
  const std::string doc = fcpxml(r.rows, o);
  CHECK(doc.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE fcpxml>\n<fcpxml version=\"1.10\">", 0) == 0);
  // Escaped names, percent-encoded URLs (a space, '&', a Windows drive, UTF-8).
  CHECK(doc.find("name=\"Birthday Cake &amp; Candles.mp4\"") != std::string::npos);
  CHECK(doc.find("src=\"file:///Volumes/Card/DCIM/Birthday%20Cake%20%26%20Candles.mp4\"") != std::string::npos);
  CHECK(doc.find("src=\"file:///C:/Photos/%C3%89t%C3%A9/cake.jpg\"") != std::string::npos);
  // 10 s match: 8 s .. 13 s, marker at 10 s, and the 9 s match inside it; 40 s is outside.
  CHECK(doc.find("start=\"8000/1000s\" duration=\"5000/1000s\"") != std::string::npos);
  CHECK(doc.find("<marker start=\"10000/1000s\"") != std::string::npos);
  CHECK(doc.find("<marker start=\"9000/1000s\"") != std::string::npos);
  CHECK(doc.find("<marker start=\"40000/1000s\"") == std::string::npos);
  CHECK(doc.find("<keyword start=\"8000/1000s\" duration=\"5000/1000s\" value=\"MV: cake\"/>") != std::string::npos);
  CHECK(doc.find("duration=\"60000/1000s\" hasVideo=\"1\" hasAudio=\"1\"") != std::string::npos);
  // The still: a 5 s clip with the keyword over it.
  CHECK(doc.find("<keyword start=\"0s\" duration=\"5000/1000s\" value=\"MV: cake\"/>") != std::string::npos);
  CHECK(doc.find("<event name=\"MediaViewer: cake\">") != std::string::npos);

  // No keyword collection when the editor does not want one (open question 8).
  o.keyword.clear();
  CHECK(fcpxml(r.rows, o).find("<keyword") == std::string::npos);

  // One asset for two moments of the same file.
  reply two = r;
  two.rows.push_back(r.rows[0]);
  two.rows.back().pts_ms = 40000;
  const std::string d2 = fcpxml(two.rows, o);
  std::size_t assets = 0;
  for (std::size_t at = d2.find("<asset "); at != std::string::npos; at = d2.find("<asset ", at + 1)) ++assets;
  CHECK(assets == 2);
}

TEST_CASE("FCPXML: a range stays inside the clip", "[nle]") {
  fcpxml_options o;
  CHECK(range_around(500, 60000, o).start_ms == 0);
  CHECK(range_around(500, 60000, o).duration_ms == 3500);
  CHECK(range_around(59000, 60000, o).start_ms == 57000);
  CHECK(range_around(59000, 60000, o).duration_ms == 3000);
  CHECK(range_around(10000, 0, o).duration_ms == 5000);  // length unknown: the full window
  CHECK(file_url("//server/share/a b.mov") == "file://server/share/a%20b.mov");
}

TEST_CASE("the viewer's thumbnails are read by another process without a write", "[nle][search-agent]") {
  scratch_dir dir{"nle-thumbs"};
  const fs::path media = dir / "clip.mp4";
  write_bytes(media, pattern(64, 3));
  auto st = mv::io::stat_path(utf8(media));
  REQUIRE(st);
  const fs::path cache = dir / "thumbs";
  fs::create_directories(cache);

  thumb_reader none(utf8(cache));
  auto miss = none.lookup(utf8(media), 4000);
  REQUIRE(miss);
  CHECK(miss->empty());  // no cache yet: a placeholder, and nothing created
  CHECK_FALSE(fs::exists(cache / "thumbs.sqlite"));

  // The app's cache, as image/thumb.h writes it: a still and one moment.
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(utf8(cache / "thumbs.sqlite").c_str(), &db) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db,
                       "CREATE TABLE thumbs(path TEXT NOT NULL, mtime INTEGER NOT NULL, size INTEGER NOT NULL,"
                       " spec TEXT NOT NULL, file TEXT NOT NULL, PRIMARY KEY(path, mtime, size, spec));",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  const auto put = [&](const mv::image::thumb_key& k, const char* file) {
    sqlite3_stmt* s = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "INSERT INTO thumbs VALUES(?,?,?,?,?)", -1, &s, nullptr) == SQLITE_OK);
    sqlite3_bind_text(s, 1, k.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, k.mtime_unix);
    sqlite3_bind_int64(s, 3, static_cast<sqlite3_int64>(k.size));
    sqlite3_bind_text(s, 4, mv::image::kThumbSpec, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 5, file, -1, SQLITE_STATIC);
    REQUIRE(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
  };
  put(mv::image::thumb_key{utf8(media), st->mtime_unix, st->size}, "poster.jpg");
  put(mv::image::moment_thumb_key(utf8(media), 4000, st->mtime_unix, st->size), "moment.jpg");
  put(mv::image::moment_thumb_key(utf8(media), 8000, st->mtime_unix, st->size), "gone.jpg");
  sqlite3_close(db);
  write_bytes(cache / "poster.jpg", pattern(8, 1));
  write_bytes(cache / "moment.jpg", pattern(8, 2));
  const std::string before = [&] {
    std::ifstream in(cache / "thumbs.sqlite", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();

  thumb_reader r(utf8(cache));
  CHECK(fs::path(*r.lookup(utf8(media), -1)).filename() == "poster.jpg");
  CHECK(fs::path(*r.lookup(utf8(media), 4000)).filename() == "moment.jpg");
  CHECK(r.lookup(utf8(media), 8000)->empty());   // its file went: a miss, the row stays
  CHECK(r.lookup(utf8(media), 12000)->empty());  // never made
  // The reader that started before the cache existed picks it up now.
  CHECK(fs::path(*none.lookup(utf8(media), -1)).filename() == "poster.jpg");
  const std::string after = [&] {
    std::ifstream in(cache / "thumbs.sqlite", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();
  CHECK(after == before);
}
