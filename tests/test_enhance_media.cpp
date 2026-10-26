// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's sound job end to end over the real host table (#297 slice 4): the
// sound rows' thunks (src/addon/host.cpp) over the hosts' own reader and
// writer (abi/addon_media, edit/sound_writer.h), Enhance's port of them
// (addons/enhance/host_port.h) and its runner, on synthetic files
// (clip_fixture.h). The model is a stand-in that delays its input by the
// shipped model's lag, unless MV_ENHANCE_DFN3 and MV_ENHANCE_RUNTIME_DIR name
// the real one (hidden case).
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "abi/addon_media.h"
#include "addon/host.h"
#include "addons/enhance/host_port.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/spec.h"
#include "clip_fixture.h"
#include "edit/clip_sample.h"
#include "import_fixture.h"

using namespace mv::enhance;
using mv::test::scratch_dir;
using mv::test::utf8;

namespace {

namespace fs = std::filesystem;

struct lag final : frame_model {
  std::size_t frame() const noexcept override { return 480; }
  std::size_t delay() const noexcept override { return 1440; }
  std::vector<float> initial_state() const override { return std::vector<float>(1440, 0.0f); }
  mv::expected process(std::span<const float> in, std::vector<float>& state, std::span<float> out) override {
    std::vector<float> line(state);
    line.insert(line.end(), in.begin(), in.end());
    std::copy(line.begin(), line.begin() + 480, out.begin());
    state.assign(line.end() - 1440, line.end());
    return {};
  }
};

model_spec shipped_audio_spec() {
  const auto text = mv::test::read_bytes(MV_ENHANCE_AUDIO_SPEC);
  auto s = parse_spec(std::string(text.begin(), text.end()), model_kind::audio);
  REQUIRE(s);
  return *s;
}

struct outcome {
  std::mutex m;
  bool done = false, ok = false;
  std::string name, message;
};

// One job through the host table, as the add-on runs it.
void run_one(run_port port, const std::string& path, item_kind item, outcome& out) {
  static const model_spec spec = shipped_audio_spec();
  job_runner r(std::move(port));
  job_request req;
  req.path = path;
  req.item = item;
  req.target = clip_target::sound;
  job_request::part p;
  p.kind = model_kind::audio;
  p.spec = &spec;
  p.models_dir = "unused";
  p.values["strength"] = *mv::json::parse("1");
  req.parts.push_back(p);
  REQUIRE(r.check(req).empty());
  job_events ev;
  ev.done = [&](bool ok, const std::string& name, const std::string& message) {
    std::lock_guard lock(out.m);
    out.done = true;
    out.ok = ok;
    out.name = name;
    out.message = message;
  };
  r.start(std::move(req), std::move(ev));
  for (int i = 0; i < 6000; ++i) {
    {
      std::lock_guard lock(out.m);
      if (out.done) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::lock_guard lock(out.m);
  REQUIRE(out.done);
}

mv::addon::host_services sound_services() {
  mv::addon::host_services svc;
  svc.open_sound = &mv::addon::media::open_sound;
  svc.open_sound_writer = &mv::addon::media::open_sound_writer;
  return svc;
}

std::int64_t frames_in(const std::string& path) {
  auto r = mv::edit::clip::audio_reader::open(path, 48000, 0, nullptr, 0);
  REQUIRE(r);
  std::int64_t n = 0, t = 0;
  while (true) {
    auto c = (*r)->read(48000 * 2, t);
    REQUIRE(c);
    if (c->empty()) break;
    n += static_cast<std::int64_t>(c->size() / (*r)->facts().channels);
  }
  return n;
}

}  // namespace

TEST_CASE("enhance: an audio file and a clip's sound, through the host table", "[enhance][sound]") {
  scratch_dir s("enhance_media");
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 1.5));
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 45;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  const auto song_before = mv::test::read_bytes(song);
  const auto clip_before = mv::test::read_bytes(clip);

  mv::addon::host_table table(sound_services());
  REQUIRE(has_sound_rows(table.api()));
  run_port port = run_port_of(table.api(), "");
  port.load_model = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<frame_model>> {
    return std::unique_ptr<frame_model>(new lag());
  };

  outcome a;
  run_one(port, song, item_kind::audio, a);
  INFO(a.message);
  REQUIRE(a.ok);
  CHECK(a.name == "song-enhance.m4a");
  CHECK(mv::test::read_bytes(song) == song_before);
  // As long as the original, give or take the AAC encoder's own frames.
  CHECK(std::llabs(frames_in(utf8(s / a.name)) - frames_in(song)) < 2048);

  outcome b;
  run_one(port, clip, item_kind::media, b);
  INFO(b.message);
  REQUIRE(b.ok);
  CHECK(b.name == "clip-enhance.mp4");
  CHECK(mv::test::read_bytes(clip) == clip_before);
  CHECK(mv::test::clipfx::video_packets(utf8(s / b.name)) == mv::test::clipfx::video_packets(clip));
}

TEST_CASE("enhance: a host without the sound rows runs nothing", "[enhance][sound]") {
  mv::addon::host_table table(mv::addon::host_services{});
  CHECK_FALSE(has_sound_rows(table.api()));
}

TEST_CASE("enhance: DeepFilterNet3 end to end through the host table", "[.enhance-model]") {
  const char* model = std::getenv("MV_ENHANCE_DFN3");
  const char* runtime = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  if (!model || !runtime) SKIP("MV_ENHANCE_DFN3 / MV_ENHANCE_RUNTIME_DIR not set");
  scratch_dir s("enhance_dfn3");
  const std::string song = utf8(s / "song.m4a");
  REQUIRE(mv::test::clipfx::make_audio(song, 5.0));
  mv::addon::host_table table(sound_services());
  run_port port = run_port_of(table.api(), runtime);
  const std::string dir = utf8(fs::path(model).parent_path());
  auto real = port.load_model;
  port.load_model = [real, dir](const model_spec& spec, const std::string&) { return real(spec, dir); };
  const auto t0 = std::chrono::steady_clock::now();
  outcome a;
  run_one(port, song, item_kind::audio, a);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  INFO(a.message);
  REQUIRE(a.ok);
  WARN("5 s of stereo 48 kHz enhanced in " << secs << " s");
  CHECK(std::llabs(frames_in(utf8(s / a.name)) - frames_in(song)) < 2048);
}
