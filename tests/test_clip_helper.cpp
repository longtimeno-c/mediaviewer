// SPDX-License-Identifier: GPL-2.0-or-later
// Clip jobs in MediaViewerClipJob (edit/clip_wire.h, owner's call plan/12
// 2026-09-25): the viewer is not affected by what happens in the helper.
// MV_CLIPJOB_PATH is the test build of the helper (hooks compiled in).
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "clip_fixture.h"
#include "edit/clip_helper.h"
#include "edit/clip_jobs.h"
#include "edit/clip_wire.h"
#include "import_fixture.h"

using namespace mv::test;
namespace clip = mv::edit::clip;
namespace fx = mv::test::clipfx;

namespace {

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) setenv(name, value, 1);
  else unsetenv(name);
#endif
}

struct env_guard {
  const char* name;
  env_guard(const char* n, const char* v) : name(n) { set_env(n, v); }
  ~env_guard() { set_env(name, nullptr); }
};

std::vector<std::string> names_in(const fs::path& dir) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) out.push_back(utf8(e.path().filename()));
  std::sort(out.begin(), out.end());
  return out;
}

clip::job_snapshot wait_finished(clip::job_queue& q, std::uint64_t id, int seconds = 30) {
  clip::job_snapshot s;
  for (int i = 0; i < seconds * 100; ++i) {
    if (q.snapshot(id, s) && s.state != clip::job_state::queued && s.state != clip::job_state::running) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return s;
}

}  // namespace

TEST_CASE("the request survives the wire; bad lines are refused", "[clip][helper]") {
  clip::request r;
  r.kind = clip::op::animation;
  r.source = "C:\\clips\\a \"quoted\"\tname\nwith newline.mp4";
  r.out_dir = "/tmp/out";
  r.in_ns = 123;
  r.out_ns = -1;
  r.animation = clip::anim_format::webp;
  r.animation_width = 320;
  const std::string line = clip::wire::encode_request(r);
  CHECK(line.find('\n') == std::string::npos);
  clip::request back;
  REQUIRE(clip::wire::decode_request(line, back));
  CHECK(back.source == r.source);
  CHECK(back.kind == r.kind);
  CHECK(back.out_ns == -1);
  CHECK(back.animation == clip::anim_format::webp);
  CHECK(back.animation_width == 320);
  CHECK_FALSE(clip::wire::decode_request("{}", back));
  CHECK_FALSE(clip::wire::decode_request("not json", back));
  CHECK_FALSE(clip::wire::decode_request(R"({"op":42,"source":"x","out_dir":""})", back));

  clip::wire::reply reply;
  clip::wire::apply_line("encoder h264_nvenc", reply, {});
  clip::wire::apply_line(clip::wire::output_line("/a/b \"c\".mp4"), reply, {});
  clip::wire::apply_line("written 5 9", reply, {});
  clip::wire::apply_line("done", reply, {});
  CHECK(reply.done);
  CHECK(reply.result.encoder == "h264_nvenc");
  REQUIRE(reply.result.outputs.size() == 1);
  CHECK(reply.result.outputs[0] == "/a/b \"c\".mp4");
  CHECK(reply.result.written.out_ns == 9);
  clip::wire::reply bad;
  clip::wire::apply_line("error 4", bad, {});
  CHECK(bad.failed);
  CHECK(bad.error == mv::status::unsupported_format);
}

TEST_CASE("which jobs leave the process", "[clip][helper]") {
  clip::request r;
  r.kind = clip::op::trim_keyframe;
  CHECK_FALSE(clip::wire::runs_in_helper(r));
  r.kind = clip::op::remux;
  CHECK_FALSE(clip::wire::runs_in_helper(r));
  r.kind = clip::op::audio;
  CHECK_FALSE(clip::wire::runs_in_helper(r));  // copy
  r.audio = clip::audio_format::flac;
  CHECK(clip::wire::runs_in_helper(r));
  for (auto k : {clip::op::trim_reencode, clip::op::frame, clip::op::animation}) {
    r.kind = k;
    CHECK(clip::wire::runs_in_helper(r));
  }
}

TEST_CASE("temporaries are swept, and only ours", "[clip][helper]") {
  scratch_dir d("clip_sweep");
  write_text(d / ".IMG_1_trimmed.mp4.mvpart", "x");
  write_text(d / "IMG_1.mp4", "x");
  write_text(d / ".hidden", "x");
  write_text(d / "not.mvpart", "x");
  clip::sweep_temporaries(utf8(d.root()));
  CHECK(names_in(d.root()) == std::vector<std::string>{".hidden", "IMG_1.mp4", "not.mvpart"});
}

#if defined(MV_CLIPJOB_PATH)

TEST_CASE("encode jobs run in the helper and land like in-process ones", "[clip][helper]") {
  scratch_dir d("clip_helper_ok");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  clip::job_queue q;
  q.set_helper(MV_CLIPJOB_PATH);

  clip::request frame;
  frame.kind = clip::op::frame;
  frame.source = utf8(src);
  frame.in_ns = 1'000'000'000;
  auto s = wait_finished(q, q.submit(frame));
  REQUIRE(s.state == clip::job_state::done);
  REQUIRE(s.outputs.size() == 1);
  CHECK(fs::exists(fs::path(reinterpret_cast<const char8_t*>(s.outputs[0].c_str()))));

  // Path 2 through the helper, frame-accurate (its software test encoder).
  env_guard soft("MV_CLIPJOB_TEST_SOFTWARE", "1");
  clip::request trim;
  trim.kind = clip::op::trim_reencode;
  trim.source = utf8(src);
  trim.in_ns = 37 * 1'000'000'000LL / 30;
  trim.out_ns = 88 * 1'000'000'000LL / 30;
  s = wait_finished(q, q.submit(trim));
  REQUIRE(s.state == clip::job_state::done);
  CHECK(s.encoder == "mpeg4");
  CHECK(s.fraction == 1.0);
  fx::decoded out;
  REQUIRE(fx::decode_all(s.outputs[0], out));
  CHECK(out.luma.size() == 51);
}

TEST_CASE("a helper that crashes fails its job, leaves nothing, and the viewer lives", "[clip][helper]") {
  scratch_dir d("clip_helper_crash");
  const fs::path src = d / "clip.mp4";
  fx::spec s;
  s.frames = 240;
  REQUIRE(fx::make(utf8(src), s));
  env_guard crash("MV_CLIPJOB_TEST_CRASH", "1");
  clip::job_queue q;
  q.set_helper(MV_CLIPJOB_PATH);
  clip::request gif;
  gif.kind = clip::op::animation;
  gif.source = utf8(src);
  gif.out_ns = 6'000'000'000;
  const auto snap = wait_finished(q, q.submit(gif));
  CHECK(snap.state == clip::job_state::failed);
  CHECK(snap.error == mv::status::internal);
  CHECK(names_in(d.root()) == std::vector<std::string>{"clip.mp4"});
}

TEST_CASE("a helper that ignores cancel is killed, and its temporaries go", "[clip][helper]") {
  scratch_dir d("clip_helper_hang");
  const fs::path src = d / "clip.mp4";
  fx::spec s;
  s.frames = 240;
  REQUIRE(fx::make(utf8(src), s));
  env_guard hang("MV_CLIPJOB_TEST_HANG", "1");
  clip::job_queue q;
  q.set_helper(MV_CLIPJOB_PATH);
  clip::request gif;
  gif.kind = clip::op::animation;
  gif.source = utf8(src);
  gif.out_ns = 6'000'000'000;
  const auto id = q.submit(gif);
  clip::job_snapshot snap;
  for (int i = 0; i < 1000 && !(q.snapshot(id, snap) && snap.state == clip::job_state::running); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the helper has a temporary open
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(q.cancel(id));
  snap = wait_finished(q, id);
  CHECK(snap.state == clip::job_state::cancelled);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15));
  CHECK(names_in(d.root()) == std::vector<std::string>{"clip.mp4"});
}

TEST_CASE("without its helper an encode job fails; a stream copy still runs", "[clip][helper]") {
  scratch_dir d("clip_helper_missing");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  clip::job_queue q;
  q.set_helper(utf8(d / "no-such-helper"));
  clip::request frame;
  frame.kind = clip::op::frame;
  frame.source = utf8(src);
  auto s = wait_finished(q, q.submit(frame));
  CHECK(s.state == clip::job_state::failed);
  CHECK(s.error == mv::status::io);
  clip::request trim;
  trim.kind = clip::op::trim_keyframe;
  trim.source = utf8(src);
  trim.out_ns = 1'000'000'000;
  s = wait_finished(q, q.submit(trim));
  CHECK(s.state == clip::job_state::done);
}

#endif  // MV_CLIPJOB_PATH
