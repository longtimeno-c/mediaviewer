// SPDX-License-Identifier: GPL-2.0-or-later
// abi/clip_session: the logic behind mediaviewer_clip.h, without a window.
#include "catch_compat.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "abi/clip_session.h"
#include "clip_fixture.h"
#include "import_fixture.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;

namespace {

struct sink {
  std::mutex mu;
  std::vector<mv_completion> got;
  void operator()(const mv_completion& c) {
    std::lock_guard lock(mu);
    got.push_back(c);
  }
  bool wait_for(std::uint32_t kind, std::uint64_t id, std::int64_t payload, int ms = 10000) {
    for (int i = 0; i < ms / 5; ++i) {
      {
        std::lock_guard lock(mu);
        for (const auto& c : got) {
          if (c.kind == kind && c.job_id == id && (payload < 0 || c.payload == payload)) return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
};

mv_clip_request make(std::uint32_t op, std::int64_t in, std::int64_t out, std::uint32_t option = 0) {
  mv_clip_request r{};
  r.struct_size = sizeof(r);
  r.op = op;
  r.in_ns = in;
  r.out_ns = out;
  r.option = option;
  return r;
}

}  // namespace

TEST_CASE("clip session: index answers and job completions", "[clip][abi][pr13]") {
  scratch_dir d("clip_abi");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  sink s;
  mv::abi::clip_session cs([&s](const mv_completion& c) { s(c); });

  std::uint64_t idx = 0;
  REQUIRE(cs.request_index(utf8(src), idx) == mv::status::ok);
  REQUIRE(s.wait_for(MV_COMPLETION_CLIP_INDEX, idx, 8));
  std::vector<std::int64_t> kf(16);
  std::uint32_t n = 0;
  std::int64_t dur = 0;
  REQUIRE(cs.index_get(idx, kf.data(), 16, &n, &dur) == mv::status::ok);
  CHECK(n == 8);
  CHECK(kf[1] > 0);
  CHECK(dur > 3'900'000'000);
  CHECK(cs.index_get(idx + 100, kf.data(), 16, &n, &dur) == mv::status::invalid_arg);
  CHECK(cs.request_index("", idx) == mv::status::invalid_arg);

  std::uint64_t job = 0;
  REQUIRE(cs.submit(utf8(src), make(MV_CLIP_TRIM_KEYFRAME, 0, 1'000'000'000), job) == mv::status::ok);
  REQUIRE(s.wait_for(MV_COMPLETION_CLIP_JOB, job, MV_CLIP_JOB_DONE));
  mv_clip_progress p{};
  REQUIRE(cs.progress(job, p) == mv::status::ok);
  CHECK(p.state == MV_CLIP_JOB_DONE);
  CHECK(p.op == MV_CLIP_TRIM_KEYFRAME);
  CHECK(p.output_count == 1);
  CHECK(std::strcmp(p.title_utf8, "Trim (keyframe)") == 0);
  CHECK(std::strcmp(p.source_name_utf8, "clip.mp4") == 0);
  std::uint32_t bytes = 0;
  char path[1024];
  REQUIRE(cs.output(job, 0, path, sizeof(path), &bytes) == mv::status::ok);
  CHECK(fs::exists(fs::path(reinterpret_cast<const char8_t*>(path))));
  CHECK(cs.output(job, 1, path, sizeof(path), &bytes) == mv::status::invalid_arg);

  // A failed job carries its reason on the completion, and can be retried.
  std::uint64_t soft = 0;
  REQUIRE(cs.submit(utf8(src), make(MV_CLIP_SPLIT, 0, -1), soft) == mv::status::ok);  // 0 is no split
  REQUIRE(s.wait_for(MV_COMPLETION_CLIP_JOB, soft, MV_CLIP_JOB_FAILED));
  {
    std::lock_guard lock(s.mu);
    const auto it = std::find_if(s.got.begin(), s.got.end(), [&](const mv_completion& c) {
      return c.job_id == soft && c.payload == MV_CLIP_JOB_FAILED;
    });
    REQUIRE(it != s.got.end());
    CHECK(it->status == MV_ERR_INVALID_ARG);
  }
  std::uint64_t again = 0;
  CHECK(cs.retry(soft, again) == mv::status::ok);
  CHECK(cs.cancel(job) == mv::status::invalid_arg);  // already done

  std::uint64_t ids[8];
  REQUIRE(cs.jobs(ids, 8, &n) == mv::status::ok);
  CHECK(n == 3);
  CHECK(ids[0] == job);
  REQUIRE(s.wait_for(MV_COMPLETION_CLIP_JOB, again, MV_CLIP_JOB_FAILED));
  cs.clear_finished();
  REQUIRE(cs.jobs(ids, 8, &n) == mv::status::ok);
  CHECK(n == 0);

  // Bad requests.
  mv_clip_request bad = make(42, 0, -1);
  CHECK(cs.submit(utf8(src), bad, job) == mv::status::invalid_arg);
  bad = make(MV_CLIP_ROTATE, 0, -1);
  bad.struct_size = 8;
  CHECK(cs.submit(utf8(src), bad, job) == mv::status::invalid_arg);
}

TEST_CASE("clip session: option numbers map onto the core request", "[clip][abi][pr14]") {
  mv::edit::clip::request r;
  REQUIRE(mv::abi::clip_session::to_request(make(MV_CLIP_ROTATE, 0, -1, 3), "a", r));
  CHECK(r.rotate_degrees == 180);
  REQUIRE(mv::abi::clip_session::to_request(make(MV_CLIP_AUDIO, 0, -1, 3), "a", r));
  CHECK(r.audio == mv::edit::clip::audio_format::flac);
  mv_clip_request anim = make(MV_CLIP_ANIMATION, 0, 2'000'000'000, 2);
  anim.animation_width = 320;
  REQUIRE(mv::abi::clip_session::to_request(anim, "a", r));
  CHECK(r.animation == mv::edit::clip::anim_format::webp);
  CHECK(r.animation_width == 320);
  CHECK(r.animation_fps == 15);
}
