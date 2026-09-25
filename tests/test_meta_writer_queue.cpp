// SPDX-License-Identifier: GPL-2.0-or-later
// PR 12: the host-shared write queue behind the rating keys and the comment
// field (shell/meta_writer.h): coalescing, one write at a time, optimistic
// state, and the job that runs on the pool.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "fixtures.h"
#include "meta/meta.h"
#include "shell/meta_writer.h"

namespace {

using mv::meta::change;
using mv::shell::meta_writer;
using mv::shell::rating_fields;

mv::shell::meta_outcome done(const mv::shell::meta_job& j, bool ok = true) {
  mv::shell::meta_outcome o;
  o.path = j.path;
  o.ok = ok;
  o.error = ok ? mv::status::ok : mv::status::io;
  return o;
}

}  // namespace

TEST_CASE("requests for one file coalesce into one write", "[meta][writer]") {
  meta_writer w;
  w.submit("/a.jpg", rating_fields(3));
  w.submit("/a.jpg", rating_fields(4));
  w.submit("/a.jpg", mv::shell::comment_fields("note"));

  CHECK(w.pending_rating("/a.jpg") == 4);
  CHECK(w.pending_comment("/a.jpg") == "note");
  CHECK_FALSE(w.pending_rating("/b.jpg"));

  const auto job = w.take_next();
  REQUIRE(job);
  CHECK(job->path == "/a.jpg");
  CHECK(job->fields.rating.value == 4);  // the latest, not the first
  CHECK(job->fields.comment.value == "note");
  CHECK_FALSE(w.take_next());  // nothing else queued
}

TEST_CASE("one write at a time, oldest file first", "[meta][writer]") {
  meta_writer w;
  w.submit("/a.jpg", rating_fields(1));
  w.submit("/b.jpg", rating_fields(2));

  const auto first = w.take_next();
  REQUIRE(first);
  CHECK(first->path == "/a.jpg");
  CHECK(w.in_flight());
  CHECK_FALSE(w.take_next());  // /b waits for /a to land

  // The rating just asked for is visible while it is in flight, and a newer
  // request for the same file queues behind it rather than joining it.
  CHECK(w.pending_rating("/a.jpg") == 1);
  w.submit("/a.jpg", rating_fields(5));
  CHECK(w.pending_rating("/a.jpg") == 5);
  CHECK(w.busy_for("/a.jpg"));

  w.finished(done(*first));
  const auto second = w.take_next();
  REQUIRE(second);
  CHECK(second->path == "/b.jpg");
  w.finished(done(*second));
  const auto third = w.take_next();
  REQUIRE(third);
  CHECK(third->path == "/a.jpg");
  CHECK(third->fields.rating.value == 5);
  w.finished(done(*third));
  CHECK_FALSE(w.busy_for("/a.jpg"));
  CHECK_FALSE(w.has_pending());
}

TEST_CASE("a cleared rating is 0 and stays visible until it lands", "[meta][writer]") {
  meta_writer w;
  w.submit("/a.jpg", rating_fields(0));
  CHECK(w.pending_rating("/a.jpg") == 0);
  const auto job = w.take_next();
  REQUIRE(job);
  CHECK(job->fields.rating.k == change<int>::kind::clear);
  w.finished(done(*job));
  CHECK_FALSE(w.pending_rating("/a.jpg"));
}

TEST_CASE("a failed write is reported once and not retried", "[meta][writer]") {
  meta_writer w;
  w.submit("/a.jpg", rating_fields(2));
  const auto job = w.take_next();
  REQUIRE(job);
  w.finished(done(*job, false));
  CHECK_FALSE(w.in_flight());
  CHECK_FALSE(w.has_pending());
  const auto failure = w.take_failure();
  REQUIRE(failure);
  CHECK(failure->path == "/a.jpg");
  CHECK_FALSE(w.take_failure());
}

TEST_CASE("empty requests are ignored and a revert outranks what was queued", "[meta][writer]") {
  meta_writer w;
  w.submit("/a.jpg", {});
  w.submit("", rating_fields(3));
  CHECK_FALSE(w.has_pending());

  w.submit("/a.jpg", rating_fields(3));
  w.submit_revert("/a.jpg");
  const auto job = w.take_next();
  REQUIRE(job);
  CHECK(job->revert);
  CHECK(job->fields.empty());
  w.finished(done(*job));

  w.submit_revert("/b.jpg");
  w.submit("/b.jpg", rating_fields(1));  // a fresh request after a queued revert wins
  const auto next = w.take_next();
  REQUIRE(next);
  CHECK_FALSE(next->revert);
}

TEST_CASE("the pool job writes through the snapshot store and reports where it landed", "[meta][writer]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "mv_writer_queue_job";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 70);
  const auto png = fixtures::png_rgba(16, 16, rgba.data());
  {
    std::ofstream f(dir / "IMG_1.png", std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  }
  const std::string path = (dir / "IMG_1.png").string();
  const std::string store = (dir / "snaps").string();

  mv::shell::meta_job job;
  job.path = path;
  job.fields = rating_fields(4);
  const auto out = mv::shell::run_meta_job(job, store);
  CHECK(out.ok);
  CHECK(out.target == mv::meta::write_target::sidecar);
  CHECK(out.sidecar_touched);
  CHECK(fs::exists(dir / "IMG_1.xmp"));

  mv::shell::meta_job back;
  back.path = path;
  back.revert = true;
  CHECK(mv::shell::run_meta_job(back, store).ok);
  CHECK_FALSE(fs::exists(dir / "IMG_1.xmp"));

  // A file that is not there is a failure, with the status kept for the log.
  mv::shell::meta_job missing;
  missing.path = (dir / "gone.jpg").string();
  missing.fields = rating_fields(1);
  const auto bad = mv::shell::run_meta_job(missing, store);
  CHECK_FALSE(bad.ok);
  CHECK(bad.error != mv::status::ok);
  fs::remove_all(dir);
}
