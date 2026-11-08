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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "abi/addon_media.h"
#include "addon/host.h"
#include "addons/enhance/face_restore.h"
#include "addons/enhance/host_port.h"
#include "addons/enhance/picture_job.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/stream_model.h"
#include "addons/enhance/spec.h"
#include "clip_fixture.h"
#include "codec/decode.h"
#include "codec/exif.h"
#include "edit/clip_sample.h"
#include "edit/encode.h"
#include "core/json.h"
#include "image/half.h"
#include "image/linear.h"
#include "edit/eyes.h"
#include "edit/picture_io.h"
#include "infer/ort.h"
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
  CHECK((a.name == "song-enhance.m4a" || a.name == "song-enhance.flac"));
  CHECK(mv::test::read_bytes(song) == song_before);
  // As long as the original, give or take the AAC encoder's own frames.
  CHECK(std::llabs(frames_in(utf8(s / a.name)) - frames_in(song)) < 2048);

  outcome b;
  run_one(port, clip, item_kind::media, b);
  INFO(b.message);
  REQUIRE(b.ok);
  CHECK((b.name == "clip-enhance.mp4" || b.name == "clip-enhance.mkv"));
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

// ---- slice 5: a clip's picture over the host table ------------------------------

namespace {

struct nearest final : tile_model {
  std::uint32_t tile() const noexcept override { return 64; }
  std::uint32_t scale() const noexcept override { return 4; }
  mv::expected run(std::span<const float> in, std::vector<float>& out) override {
    out.assign(std::size_t{3} * 256 * 256, 0);
    for (int c = 0; c < 3; ++c) {
      for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) out[std::size_t(c) * 65536 + y * 256 + x] = in[std::size_t(c) * 4096 + (y / 4) * 64 + x / 4];
      }
    }
    return {};
  }
};

model_spec video_spec(std::uint32_t tile) {
  auto s = parse_spec(R"json({"kind": "video", "name": "Detail", "weights": ["realesr-general-x4v3.onnx"],
    "run_bytes": 1000000000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}], "default": "2"}],
    "io": {"runner": "tiles", "scale": 4, "tile": )json" + std::to_string(tile) +
                          R"json(, "overlap": 16, "dims": ["h", "w"]}})json",
                      model_kind::video);
  REQUIRE(s);
  return *s;
}

void run_clip(run_port port, const std::string& path, const model_spec& video, const model_spec* audio,
              const char* scale, outcome& out) {
  job_runner r(std::move(port));
  job_request req;
  req.path = path;
  req.item = item_kind::media;
  req.target = audio ? clip_target::both : clip_target::picture;
  job_request::part v;
  v.kind = model_kind::video;
  v.spec = &video;
  v.models_dir = "unused";
  v.values["scale"] = *mv::json::parse(std::string("\"") + scale + "\"");
  req.parts.push_back(v);
  if (audio) {
    job_request::part a;
    a.kind = model_kind::audio;
    a.spec = audio;
    a.models_dir = "unused";
    req.parts.push_back(a);
  }
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
  for (int i = 0; i < 60000; ++i) {
    {
      std::lock_guard lock(out.m);
      if (out.done) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::lock_guard lock(out.m);
  REQUIRE(out.done);
}

mv::addon::host_services picture_services() {
  mv::addon::host_services svc = sound_services();
  svc.open_frames = &mv::addon::media::open_frames;
  svc.open_clip_writer = &mv::addon::media::open_clip_writer;
  return svc;
}

}  // namespace

TEST_CASE("enhance: a clip at twice the size, through the host table and a hardware encoder", "[enhance][picture]") {
  scratch_dir s("enhance_picture");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 20;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  const auto before = mv::test::read_bytes(clip);
  mv::addon::host_table table(picture_services());
  REQUIRE(has_picture_rows(table.api()));
  run_port port = run_port_of(table.api(), "");
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest());
  };
  const model_spec video = video_spec(64);
  outcome a;
  run_clip(port, clip, video, nullptr, "2", a);
  if (!a.ok && a.message.find("hardware video encoder") != std::string::npos) {
    SKIP("no hardware video encoder on this machine");
  }
  INFO(a.message);
  REQUIRE(a.ok);
  CHECK(a.name == "clip-enhance.mp4");
  CHECK(mv::test::read_bytes(clip) == before);
  auto frames = mv::edit::clip::frame_reader::open(utf8(s / a.name));
  REQUIRE(frames);
  CHECK((*frames)->facts().width == 640u);
  CHECK((*frames)->facts().height == 480u);
  mv::test::clipfx::decoded d;
  REQUIRE(mv::test::clipfx::decode_all(utf8(s / a.name), d));
  CHECK(d.luma.size() == 20u);
  CHECK(d.audio_packets > 0);  // the source's sound, copied
  for (std::size_t i = 0; i < d.luma.size(); ++i) {
    CHECK(std::abs(d.luma[i] - mv::test::clipfx::luma_of_frame(static_cast<int>(i))) <= 4);
  }
  CHECK_FALSE(table.api()->clip_write_open == nullptr);
}

// The Video Editor's cut through the host table (frames_keep, sound_keep):
// only the kept frames, on the edit's timeline, and the kept sound; then a
// cut clip end to end, its sound read through the cut and written again.
TEST_CASE("enhance: the Video Editor's edit through the host table", "[enhance][picture][edit]") {
  scratch_dir s("enhance_edit");
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.frames = 30;  // 1 s at 30 fps, with a 48 kHz tone
  REQUIRE(mv::test::clipfx::make(clip, spec));
  mv::addon::host_table table(picture_services());
  const mv_host_api* h = table.api();
  REQUIRE(has_keep_rows(h));
  // Frames 0-5 and 15-20 (each frame's luma says which it is), the edges
  // between frames: 183 ms, then 200 ms. On the edit, frame i is at i / 30 s.
  const std::int64_t edit[] = {0, 183'000'000, 483'000'000, 683'000'000};

  {
    mv_addon_frames_info info{};
    info.struct_size = sizeof info;
    void* frames = nullptr;
    REQUIRE(h->frames_open(h->host, clip.c_str(), &info, &frames) == MV_OK);
    REQUIRE(h->frames_keep(h->host, frames, edit, 2) == MV_OK);
    std::vector<std::uint8_t> rgb(std::size_t{info.width} * info.height * 3);
    std::vector<int> lumas;
    std::vector<std::int64_t> times;
    for (;;) {
      std::int64_t pts = 0;
      std::uint32_t got = 0;
      REQUIRE(h->frames_read(h->host, frames, rgb.data(), rgb.size(), &pts, &got) == MV_OK);
      if (!got) break;
      lumas.push_back(rgb[(std::size_t{info.height * 3 / 4} * info.width + info.width / 2) * 3 + 1]);
      times.push_back(pts);
    }
    // Once read, the cut cannot change.
    CHECK(h->frames_keep(h->host, frames, edit, 2) == MV_ERR_INVALID_ARG);
    h->frames_close(h->host, frames);
    REQUIRE(lumas.size() == 12u);
    for (std::size_t i = 0; i < 12; ++i) {
      INFO(i);
      // RGB from limited-range YUV: compare the order, not the level.
      if (i > 0) CHECK(lumas[i] > lumas[i - 1]);
      CHECK(std::abs(times[i] - static_cast<std::int64_t>(i) * 33'333) <= 1000);
    }
    // Frame 15 follows frame 5 (luma 76 against 36): nothing between.
    CHECK(lumas[6] - lumas[5] > 4 * (lumas[5] - lumas[4]));
  }
  {
    mv_addon_sound_info info{};
    info.struct_size = sizeof info;
    void* sound = nullptr;
    REQUIRE(h->sound_open(h->host, clip.c_str(), 48000, 0, &info, &sound) == MV_OK);
    REQUIRE(h->sound_keep(h->host, sound, edit, 2) == MV_OK);
    std::vector<float> buf(4096 * info.channels);
    std::uint64_t total = 0;
    for (;;) {
      std::uint32_t got = 0;
      REQUIRE(h->sound_read(h->host, sound, buf.data(), 4096, &got) == MV_OK);
      if (!got) break;
      total += got;
    }
    h->sound_close(h->host, sound);
    // 383 ms at 48 kHz, give or take the encoder's priming and the 1 ms the
    // reader's first time is rounded to.
    CHECK(total >= 18'384u - 1100u);
    CHECK(total <= 18'384u + 100u);
  }

  // End to end: the picture alone at the same size, so the cut clip's own
  // sound is written again (it cannot be copied whole).
  run_port port = run_port_of(h, "");
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest());
  };
  const model_spec video = video_spec(64);
  job_runner r(std::move(port));
  job_request req;
  req.path = clip;
  req.item = item_kind::media;
  req.target = clip_target::picture;
  req.has_sound = true;
  req.keep_ns.assign(std::begin(edit), std::end(edit));
  job_request::part v;
  v.kind = model_kind::video;
  v.spec = &video;
  v.models_dir = "unused";
  v.values["scale"] = *mv::json::parse("\"1\"");
  req.parts.push_back(v);
  REQUIRE(r.check(req).empty());
  outcome a;
  job_events ev;
  ev.done = [&](bool ok, const std::string& name, const std::string& message) {
    std::lock_guard lock(a.m);
    a.done = true;
    a.ok = ok;
    a.name = name;
    a.message = message;
  };
  r.start(std::move(req), std::move(ev));
  for (int i = 0; i < 60000; ++i) {
    {
      std::lock_guard lock(a.m);
      if (a.done) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::lock_guard lock(a.m);
  REQUIRE(a.done);
  if (!a.ok && a.message.find("hardware video encoder") != std::string::npos) {
    SKIP("no hardware video encoder on this machine");
  }
  INFO(a.message);
  REQUIRE(a.ok);
  mv::test::clipfx::decoded d;
  REQUIRE(mv::test::clipfx::decode_all(utf8(s / a.name), d));
  REQUIRE(d.luma.size() == 12u);
  CHECK(std::abs(d.luma[5] - mv::test::clipfx::luma_of_frame(5)) <= 4);
  CHECK(std::abs(d.luma[6] - mv::test::clipfx::luma_of_frame(15)) <= 4);
  CHECK(d.audio_packets > 0);
  auto out = mv::edit::clip::audio_reader::open(utf8(s / a.name), 48000, 0);
  REQUIRE(out);
  CHECK(std::abs((*out)->facts().duration_ms - 383) <= 60);
}

// The real Real-ESRGAN on ONNX Runtime: tiled as the add-on runs it, against
// one pass over the whole picture; and a clip end to end with both models,
// timed. MV_ENHANCE_ESRGAN names realesr-general-x4v3.onnx.
TEST_CASE("enhance: Real-ESRGAN tiled agrees with one pass, and a clip end to end", "[.enhance-model]") {
  const char* esr = std::getenv("MV_ENHANCE_ESRGAN");
  const char* dfn = std::getenv("MV_ENHANCE_DFN3");
  const char* runtime = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  if (!esr || !runtime) SKIP("MV_ENHANCE_ESRGAN / MV_ENHANCE_RUNTIME_DIR not set");
  scratch_dir s("enhance_esrgan");
  const std::string dir = utf8(fs::path(esr).parent_path());
  const model_spec video = video_spec(256);

  // A 300 x 250 picture: four tiles. Smooth with some edges, as a frame is.
  const std::uint32_t w = 300, h = 250;
  std::vector<std::uint8_t> rgb(std::size_t{w} * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const double v = 0.5 + 0.4 * std::sin(x * 0.07) * std::cos(y * 0.05);
      rgb[(std::size_t{y} * w + x) * 3] = static_cast<std::uint8_t>(255 * v);
      rgb[(std::size_t{y} * w + x) * 3 + 1] = static_cast<std::uint8_t>((x / 25 + y / 25) % 2 ? 200 : 60);
      rgb[(std::size_t{y} * w + x) * 3 + 2] = static_cast<std::uint8_t>(255 * (1 - v));
    }
  }
  runtime_holder rt(runtime);
  auto tiles = rt.open_tiles(video, dir, utf8(s / "cache"));
  REQUIRE(tiles);
  picture_options o;
  o.out_scale = 4;
  o.overlap = 16;
  std::vector<std::uint8_t> tiled;
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(enhance_picture(rgb, w, h, **tiles, o, tiled));
  const double tile_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  // One pass: the same model with free dimensions, on the CPU.
  auto whole_spec = video_spec(256);
  struct one_pass final : tile_model {
    std::unique_ptr<mv::infer::session> s;
    std::uint32_t side;
    std::uint32_t tile() const noexcept override { return side; }
    std::uint32_t scale() const noexcept override { return 4; }
    mv::expected run(std::span<const float> in, std::vector<float>& out) override {
      mv::infer::tensor_f32 t;
      t.shape = {1, 3, side, side};
      t.data.assign(in.begin(), in.end());
      auto r = s->run(std::span<const mv::infer::tensor_f32>(&t, 1));
      if (!r) return mv::err(r.error());
      out = std::move((*r)[0].data);
      return {};
    }
  };
  auto loaded = mv::infer::runtime::load(runtime);
  REQUIRE(loaded);
  mv::infer::session_options cpu;
  cpu.threads = 4;
  auto sess = mv::infer::session::open(**loaded, std::string(esr), cpu);
  REQUIRE(sess);
  one_pass whole;
  whole.s = std::move(*sess);
  whole.side = 300;  // one tile covers it all (square, edges replicated)
  picture_options o1;
  o1.out_scale = 4;
  o1.overlap = 0;
  std::vector<std::uint8_t> reference;
  REQUIRE(enhance_picture(rgb, w, h, whole, o1, reference));
  REQUIRE(reference.size() == tiled.size());
  double sum = 0;
  int worst = 0;
  std::size_t n = 0;
  // Away from the picture's own border (where padding differs), every pixel.
  for (std::uint32_t y = 64; y < 4 * h - 64; ++y) {
    for (std::uint32_t x = 64; x < 4 * w - 64; ++x) {
      for (int c = 0; c < 3; ++c) {
        const std::size_t i = (std::size_t{y} * 4 * w + x) * 3 + c;
        const int d = std::abs(int(tiled[i]) - int(reference[i]));
        sum += d;
        worst = std::max(worst, d);
        ++n;
      }
    }
  }
  WARN("tiled vs one pass: mean " << sum / static_cast<double>(n) << ", worst " << worst << " (of 255); 4 tiles in "
                                   << tile_s << " s");
  CHECK(sum / static_cast<double>(n) < 1.0);

  // A 640 x 360 clip, a second long, at twice the size with its sound too.
  if (!dfn) return;
  const std::string clip = utf8(s / "clip.mp4");
  mv::test::clipfx::spec spec;
  spec.width = 640;
  spec.height = 360;
  spec.frames = 30;
  REQUIRE(mv::test::clipfx::make(clip, spec));
  mv::addon::host_table table(picture_services());
  run_port port = run_port_of(table.api(), runtime);
  auto real_tiles = port.load_tiles;
  port.load_tiles = [real_tiles, dir](const model_spec& sp, const std::string&) { return real_tiles(sp, dir); };
  const std::string dfn_dir = utf8(fs::path(dfn).parent_path());
  auto real_sound = port.load_model;
  port.load_model = [real_sound, dfn_dir](const model_spec& sp, const std::string&) { return real_sound(sp, dfn_dir); };
  const model_spec audio = shipped_audio_spec();
  const auto c0 = std::chrono::steady_clock::now();
  outcome a;
  run_clip(port, clip, video, &audio, "2", a);
  const double clip_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
  INFO(a.message);
  REQUIRE(a.ok);
  WARN("30 frames of 640x360 to 1280x720 with the sound: " << clip_s << " s (" << clip_s / 30 << " s a frame)");
  auto frames = mv::edit::clip::frame_reader::open(utf8(s / a.name));
  REQUIRE(frames);
  CHECK((*frames)->facts().width == 1280u);
}

// ---- slice 3: a photo over the host table -------------------------------------------

namespace {

// A JPEG whose EXIF says Orientation 6 (turn 90 degrees clockwise to view).
std::vector<std::uint8_t> sideways_jpeg(std::uint32_t w, std::uint32_t h) {
  mv::codec::raster r;
  r.width = w;
  r.height = h;
  r.rgba.resize(std::size_t{w} * h * 4, 255);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = &r.rgba[(std::size_t{y} * w + x) * 4];
      p[0] = static_cast<std::uint8_t>(x * 255 / w);
      p[1] = static_cast<std::uint8_t>(y * 255 / h);
      p[2] = 90;
    }
  }
  mv::edit::metadata_blobs meta;
  meta.exif = {0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,              // big-endian TIFF, IFD at 8
               0x00, 0x01,                                                  // one entry
               0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00,  // Orientation = 6
               0x00, 0x00, 0x00, 0x00};
  mv::edit::encode_options o;
  auto bytes = mv::edit::encode(r, o, meta);
  REQUIRE(bytes);
  return *bytes;
}

mv::addon::host_services still_services() {
  mv::addon::host_services svc = picture_services();
  svc.full_still = &mv::addon::media::full_still;
  svc.write_still = &mv::addon::media::write_still;
  svc.still_extend = &mv::addon::media::still_extend;  // edit-refresh S6
  return svc;
}

model_spec still_spec() {
  auto s = parse_spec(R"json({"kind": "still", "name": "Detail", "weights": ["realesr-general-x4v3.onnx"],
    "run_bytes": 1000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}], "default": "2"}],
    "io": {"runner": "tiles", "scale": 4, "tile": 64, "overlap": 16, "dims": ["h", "w"]}})json",
                      model_kind::still);
  REQUIRE(s);
  return *s;
}

void run_photo(run_port port, const std::string& path, const model_spec& spec, outcome& out) {
  job_runner r(std::move(port));
  job_request req;
  req.path = path;
  req.item = item_kind::still;
  job_request::part p;
  p.kind = model_kind::still;
  p.spec = &spec;
  p.models_dir = "unused";
  p.values["scale"] = *mv::json::parse("\"2\"");
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

}  // namespace

TEST_CASE("enhance: a photo at twice the size, upright, with its metadata, beside the original", "[enhance][still]") {
  scratch_dir s("enhance_still");
  const fs::path photo = s / "IMG_0001.JPG";
  mv::test::write_bytes(photo, sideways_jpeg(64, 48));
  const auto before = mv::test::read_bytes(photo);
  mv::addon::host_table table(still_services());
  REQUIRE(has_still_rows(table.api()));
  run_port port = run_port_of(table.api(), "");
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest());
  };
  const model_spec spec = still_spec();

  outcome a;
  run_photo(port, utf8(photo), spec, a);
  INFO(a.message);
  REQUIRE(a.ok);
  CHECK(a.name == "IMG_0001-enhance.jpg");
  CHECK(mv::test::read_bytes(photo) == before);
  // Upright and twice the size: the source was 64 x 48 stored, 48 x 64 shown.
  const auto out = mv::test::read_bytes(s / a.name);
  auto decoded = mv::codec::decode(out);
  REQUIRE(decoded);
  CHECK(decoded->width == 96u);
  CHECK(decoded->height == 128u);
  // The metadata came along, with Orientation 1 (else the decode above would
  // have turned it again).
  CHECK(mv::codec::find_jpeg_exif(out).has_value());

  // A second run takes the next free name.
  outcome b;
  run_photo(port, utf8(photo), spec, b);
  REQUIRE(b.ok);
  CHECK(b.name != a.name);

  // A PNG stays a PNG.
  const fs::path png = s / "shot.png";
  std::vector<std::uint8_t> rgba(32 * 24 * 4, 200);
  mv::codec::raster r;
  r.width = 32;
  r.height = 24;
  r.rgba = rgba;
  mv::edit::encode_options po;
  po.format = mv::edit::image_format::png;
  auto png_bytes = mv::edit::encode(r, po, {});
  REQUIRE(png_bytes);
  mv::test::write_bytes(png, *png_bytes);
  outcome c;
  run_photo(port, utf8(png), spec, c);
  INFO(c.message);
  REQUIRE(c.ok);
  CHECK(c.name == "shot-enhance.png");
}

// docs/plans/edit-refresh.md S10: Restore faces through the host table's
// still_faces row (a stand-in for the AI pack) and still_write's own name.
TEST_CASE("enhance: Restore faces through the host table writes <stem>-restored.jpg", "[enhance][still][faces]") {
  scratch_dir s("enhance_faces");
  const fs::path photo = s / "IMG_0002.JPG";
  mv::test::write_bytes(photo, sideways_jpeg(160, 120));
  const auto before = mv::test::read_bytes(photo);
  mv::addon::host_services svc = still_services();
  std::atomic<int> asked{0}, judged{0};
  // One face, the FFHQ template at a fifth of 512, wherever it is asked about;
  // the guard's cosine says the same person.
  svc.still_faces = [&](const std::uint8_t*, std::uint32_t w, std::uint32_t h, std::uint32_t stride,
                        const std::uint8_t* other) -> mv::result<std::string> {
    CHECK(stride == w * 3);
    ++(other ? judged : asked);
    const float k = w == 512 ? 1.0f : 0.2f;  // the aligned crop, or the photo
    const float ox = w == 512 ? 0.0f : 20.0f, oy = w == 512 ? 0.0f : 10.0f;
    std::string lm;
    for (std::size_t i = 0; i < 10; ++i) {
      const float v = (kFfhq512[i] * k + (i % 2 == 0 ? ox : oy)) / static_cast<float>(i % 2 == 0 ? w : h);
      lm += (i ? "," : "") + std::to_string(v);
    }
    const float bx = (128 * k + ox) / w, by = (150 * k + oy) / h, bw = 256 * k / w, bh = 280 * k / h;
    return std::string("{\"faces\":[{\"x\":") + std::to_string(bx) + ",\"y\":" + std::to_string(by) +
           ",\"w\":" + std::to_string(bw) + ",\"h\":" + std::to_string(bh) + ",\"score\":0.9,\"lm\":[" + lm +
           "]" + (other ? ",\"sim\":0.95" : "") + "}]}";
  };
  mv::addon::host_table table(svc);
  REQUIRE(has_faces_row(table.api()));
  run_port port = run_port_of(table.api(), "");
  REQUIRE(port.still_faces);
  REQUIRE(port.write_still_as);
  struct invert final : face_restorer {
    std::uint32_t side() const noexcept override { return 512; }
    mv::result<rgb8> restore(const rgb8& f) override {
      rgb8 r = f;
      for (auto& v : r.rgb) v = static_cast<std::uint8_t>(255 - v);
      return r;
    }
  };
  port.load_face = [](const model_spec&, const std::string&, const std::string&,
                      int) -> mv::result<std::unique_ptr<face_restorer>> {
    return std::unique_ptr<face_restorer>(new invert());
  };
  auto spec = parse_spec(R"json({"kind": "face", "name": "Restore faces", "weights": ["f.onnx"], "run_bytes": 1000,
    "cpu": true, "knobs": [{"id": "strength", "label": "Strength", "type": "slider", "min": 0, "max": 1, "default": 0.7}],
    "io": {"runner": "face", "side": 512, "models": {"restoreformer": {"file": "f.onnx"}}}})json",
                         model_kind::face);
  REQUIRE(spec);
  job_runner r(std::move(port));
  job_request req;
  req.path = utf8(photo);
  req.item = item_kind::still;
  job_request::part p;
  p.kind = model_kind::face;
  p.spec = &*spec;
  p.models_dir = "unused";
  req.parts.push_back(p);
  REQUIRE(r.check(req).empty());
  outcome out;
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
  INFO(out.message);
  REQUIRE(out.ok);
  CHECK(out.name == "IMG_0002-restored.jpg");
  CHECK(asked.load() >= 2);   // the photo, then the aligned crop
  CHECK(judged.load() == 1);  // the guard, once for the one face
  CHECK(mv::test::read_bytes(photo) == before);  // never the original
  auto decoded = mv::codec::decode(mv::test::read_bytes(s / out.name));
  REQUIRE(decoded);
  CHECK(decoded->width == 120u);  // upright, the same size
  CHECK(decoded->height == 160u);
}

// docs/plans/edit-adjust.md slice 5: the photo being edited carries its edit
// into Enhance's copy; any other photo, or none registered, does not.
TEST_CASE("enhance: a Photos-library item is read through the host and saved into the library",
          "[enhance][still][photos]") {
  // Owner 2026-11-04: edits of library photos. The key is resolved to a file
  // to read; what is made is staged outside the library and handed to the
  // library as a new photo; nothing is written beside the file.
  scratch_dir s("enhance_photos");
  const fs::path library = s.root() / "Photos Library.photoslibrary" / "originals";
  fs::create_directories(library);
  const fs::path original = library / "IMG_0412.JPG";
  mv::test::write_bytes(original, sideways_jpeg(48, 32));
  std::string asked, added;
  mv::addon::media::set_photos_hooks(
      [&](const std::string& key, const mv::job_context*) -> mv::result<std::string> {
        asked = key;
        return original.string();
      },
      [&](const std::string& file, const std::string& source) -> mv::result<std::string> {
        added = file;
        CHECK(source == "photos:ABC-123/L0/001");
        CHECK(fs::exists(fs::path(file)));  // staged until the library has it
        return std::string("photos:NEW-456/L0/001");  // the new photo's key
      });
  const auto pic = mv::addon::media::full_still("photos:ABC-123/L0/001");
  REQUIRE(pic);
  CHECK(asked == "photos:ABC-123/L0/001");
  CHECK(pic->width == 32);  // upright, as any file's
  mv::addon::rgb_image out;
  out.width = pic->width;
  out.height = pic->height;
  out.rgb = pic->rgb;
  const auto written = mv::addon::media::write_still("photos:ABC-123/L0/001", out, "enhance");
  REQUIRE(written);
  CHECK(written.value() == "photos:NEW-456/L0/001");
  CHECK(added.find("MediaViewer Saves") != std::string::npos);
  CHECK(fs::path(added).filename() == "IMG_0412-enhance.jpg");
  CHECK_FALSE(fs::exists(fs::path(added)));  // the staged file is gone once added
  CHECK(added.find(".photoslibrary") == std::string::npos);
  std::size_t files = 0;
  for (const auto& e : fs::directory_iterator(library)) files += e.is_regular_file() ? 1 : 0;
  CHECK(files == 1);  // only the original
  mv::addon::media::set_photos_hooks(nullptr, nullptr);
  CHECK_FALSE(mv::addon::media::full_still("photos:ABC-123/L0/001"));  // no hooks: not found
}

TEST_CASE("enhance: the photo being edited is enhanced with its crop and colour", "[enhance][still]") {
  scratch_dir s("enhance_edited");
  const fs::path photo = s / "IMG_0003.JPG";
  mv::test::write_bytes(photo, sideways_jpeg(64, 48));  // 48 x 64 shown
  mv::addon::host_table table(still_services());
  run_port port = run_port_of(table.api(), "");
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest());
  };
  const model_spec spec = still_spec();

  mv::edit::geometry g;
  g.crop = {0.0f, 0.0f, 1.0f, 0.75f};  // the top 48 x 48
  mv::edit::colour c;
  c.v[static_cast<std::size_t>(mv::edit::adjust_param::mono)] = 100.0f;
  mv::addon::media::set_still_edit(utf8(photo), g, c);
  outcome a;
  run_photo(port, utf8(photo), spec, a);
  INFO(a.message);
  REQUIRE(a.ok);
  auto edited = mv::codec::decode(mv::test::read_bytes(s / a.name));
  REQUIRE(edited);
  CHECK(edited->width == 96u);
  CHECK(edited->height == 96u);
  // Black & white came along: a pixel's channels agree (JPEG rounding aside).
  const std::uint8_t* px = &edited->rgba[(std::size_t{40} * edited->width + 30) * 4];
  CHECK(std::abs(int(px[0]) - int(px[2])) <= 6);

  // Another photo is not the one being edited: its own pixels.
  mv::addon::media::set_still_edit(utf8(s / "other.jpg"), g, c);
  outcome b;
  run_photo(port, utf8(photo), spec, b);
  REQUIRE(b.ok);
  auto plain = mv::codec::decode(mv::test::read_bytes(s / b.name));
  REQUIRE(plain);
  CHECK(plain->height == 128u);
  mv::addon::media::clear_still_edit();
}

// docs/plans/edit-adjust.md slice 6: Open closed eyes, from the pack's answer
// to a new file. The faces here are drawn: skin, and two eyes either open (a
// white almond, a dark iris) or shut (a line of lashes).
namespace {

std::vector<std::uint8_t> face_jpeg(std::uint32_t w, std::uint32_t h, float cx, float cy, float d, bool open) {
  mv::codec::raster r;
  r.width = w;
  r.height = h;
  r.format = mv::codec::format_family::jpeg;
  r.rgba.assign(std::size_t{w} * h * 4, 255);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = &r.rgba[(std::size_t{y} * w + x) * 4];
      p[0] = 200, p[1] = 150, p[2] = 125;
      for (float side : {-0.5f, 0.5f}) {
        const float ux = static_cast<float>(x) - (cx + side * d), uy = static_cast<float>(y) - cy;
        // A brow over each eye (edit::eyes_readable looks for one).
        if (uy > -0.36f * d && uy < -0.28f * d && std::abs(ux) < 0.28f * d) p[0] = 70, p[1] = 50, p[2] = 40;
        if (open) {
          if ((ux / (0.25f * d)) * (ux / (0.25f * d)) + (uy / (0.11f * d)) * (uy / (0.11f * d)) < 1.0f) {
            p[0] = p[1] = p[2] = 235;
          }
          if (ux * ux + uy * uy < (0.09f * d) * (0.09f * d)) p[0] = 25, p[1] = 20, p[2] = 18;
        } else if (std::abs(uy) < 1.5f && std::abs(ux) < 0.25f * d) {
          p[0] = 30, p[1] = 25, p[2] = 25;
        }
      }
    }
  }
  mv::edit::encode_options o;
  o.quality = 95;
  auto bytes = mv::edit::encode(r, o, {});
  REQUIRE(bytes);
  return *bytes;
}

std::string lm_json(float w, float h, float cx, float cy, float d) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "[%g,%g,%g,%g,%g,%g,%g,%g,%g,%g]", (cx - d / 2) / w, cy / h, (cx + d / 2) / w,
                cy / h, cx / w, (cy + d / 2) / h, (cx - d / 3) / w, (cy + d) / h, (cx + d / 3) / w, (cy + d) / h);
  return buf;
}

}  // namespace

namespace {
// A JPEG with an EXIF DateTimeOriginal (`stamp`, "YYYY:MM:DD HH:MM:SS"): an
// APP1 after SOI holding IFD0 -> Exif IFD -> 0x9003, little-endian.
std::vector<std::uint8_t> with_taken(std::vector<std::uint8_t> jpeg, const char* stamp) {
  std::vector<std::uint8_t> t = {'I', 'I', 42, 0, 8, 0, 0, 0,
                                 1, 0, 0x69, 0x87, 4, 0, 1, 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0,
                                 1, 0, 0x03, 0x90, 2, 0, 20, 0, 0, 0, 44, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 19; ++i) t.push_back(static_cast<std::uint8_t>(stamp[i]));
  t.push_back(0);
  const std::size_t len = 2 + 6 + t.size();
  std::vector<std::uint8_t> app1 = {0xFF, 0xE1, static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len & 0xFF),
                                    'E', 'x', 'i', 'f', 0, 0};
  app1.insert(app1.end(), t.begin(), t.end());
  jpeg.insert(jpeg.begin() + 2, app1.begin(), app1.end());
  return jpeg;
}
}  // namespace

TEST_CASE("eyes: the same moment is when the photos were taken, not when the files were written", "[enhance][eyes]") {
  scratch_dir s("moment");
  // Copied off a phone: every file written minutes apart, in another order.
  const fs::path a = s / "IMG_0300.JPG", b = s / "IMG_0301.JPG", c = s / "IMG_0302.JPG";
  mv::test::write_bytes(a, with_taken(face_jpeg(64, 48, 32, 24, 16, false), "2026:10:09 10:42:37"));
  mv::test::write_bytes(b, with_taken(face_jpeg(64, 48, 32, 24, 16, true), "2026:10:09 10:42:38"));
  mv::test::write_bytes(c, with_taken(face_jpeg(64, 48, 32, 24, 16, true), "2026:10:10 20:30:00"));  // another day
  const auto t0 = fs::last_write_time(a);
  fs::last_write_time(b, t0 + std::chrono::seconds(240));
  fs::last_write_time(c, t0 + std::chrono::seconds(5));
  const std::vector<std::string> near = mv::addon::media::burst_neighbours(utf8(a));
  REQUIRE(near.size() == 1);
  CHECK(near[0] == utf8(b));
  // The same evening, later: the occasion's frame follows the moment's.
  const fs::path d = s / "IMG_0303.JPG";
  mv::test::write_bytes(d, with_taken(face_jpeg(64, 48, 32, 24, 16, true), "2026:10:09 13:10:00"));
  const std::vector<std::string> both = mv::addon::media::burst_neighbours(utf8(a));
  REQUIRE(both.size() == 2);
  CHECK(both[0] == utf8(b));
  CHECK(both[1] == utf8(d));
}

TEST_CASE("eyes: a shut-eyed photo becomes a new file with a neighbour's open eyes", "[enhance][eyes]") {
  scratch_dir s("open_eyes");
  const fs::path shut = s / "IMG_0101.JPG", open = s / "IMG_0102.JPG", far = s / "IMG_0200.JPG";
  mv::test::write_bytes(shut, face_jpeg(400, 300, 200, 150, 120, false));
  mv::test::write_bytes(open, face_jpeg(400, 300, 210, 145, 124, true));
  mv::test::write_bytes(far, face_jpeg(400, 300, 200, 150, 120, true));
  const auto before = mv::test::read_bytes(shut);
  // The burst: the open frame was taken 2 s later, the far one five minutes
  // on (outside kBurstSeconds). No EXIF stamp here: the files' times decide.
  const auto t0 = fs::last_write_time(shut);
  fs::last_write_time(open, t0 + std::chrono::seconds(2));
  fs::last_write_time(far, t0 + std::chrono::seconds(300));
  const std::vector<std::string> near = mv::addon::media::burst_neighbours(utf8(shut));
  REQUIRE(near.size() == 1);
  CHECK(near[0] == utf8(open));

  // What the pack answers: this face, and the same person in the open frame.
  // Paths through the JSON writer, as the pack writes them (a Windows path's
  // backslashes need escaping).
  auto quoted = [](const std::string& p) {
    mv::json::writer w;
    w.string(p);
    return w.take();
  };
  const std::string pack = std::string("{\"faces\":[{\"x\":0.3,\"y\":0.3,\"w\":0.4,\"h\":0.5,\"lm\":") +
                           lm_json(400, 300, 200, 150, 120) + ",\"person\":7,\"donors\":[{\"path\":" +
                           quoted(near[0]) + ",\"lm\":" + lm_json(400, 300, 210, 145, 124) + ",\"sim\":0.9}]}]}";
  auto r = mv::addon::media::open_closed_eyes(utf8(shut), pack);
  REQUIRE(r);
  CHECK(r->faces == 1);
  CHECK(r->fixed == 1);
  CHECK(fs::path(r->written).filename() == "IMG_0101-eyes.jpg");
  CHECK(mv::test::read_bytes(shut) == before);  // the original is never written
  auto fixed = mv::image::decode_linear(mv::test::read_bytes(fs::path(r->written)));
  auto was = mv::image::decode_linear(before);
  REQUIRE(fixed);
  REQUIRE(was);
  mv::edit::face_landmarks lm{};
  auto parsed = mv::json::parse(lm_json(400, 300, 200, 150, 120));
  REQUIRE(parsed);
  for (std::size_t i = 0; i < lm.size(); ++i) lm[i] = static_cast<float>(parsed->a[i].d);
  CHECK(mv::edit::eye_openness(*fixed, lm) > mv::edit::eye_openness(*was, lm) * mv::edit::kEyesBetter);

  // Eyes already open: nothing better to give, no file.
  const std::string none = std::string("{\"faces\":[{\"lm\":") + lm_json(400, 300, 210, 145, 124) +
                           ",\"donors\":[{\"path\":" + quoted(utf8(shut)) + ",\"lm\":" +
                           lm_json(400, 300, 200, 150, 120) + "}]}]}";
  auto n = mv::addon::media::open_closed_eyes(utf8(open), none);
  REQUIRE(n);
  CHECK(n->fixed == 0);
  CHECK(n->written.empty());
  CHECK_FALSE(mv::addon::media::open_closed_eyes(utf8(shut), "not json"));
}

// docs/plans/edit-refresh.md S8: the Faces tool changes only the faces chosen
// on the canvas. Two faces in one photo (eyes shut), their open-eyed selves in
// a frame of the same moment; a faces_mask picks which are fixed, and every
// pixel outside the chosen faces' edit regions (edit::face_region, the boxes
// the canvas draws) is bit for bit what it was.
namespace {

struct drawn_face {
  float cx, cy, d;
  bool open;
};

std::vector<std::uint8_t> faces_jpeg(std::uint32_t w, std::uint32_t h, std::initializer_list<drawn_face> faces) {
  mv::codec::raster r;
  r.width = w;
  r.height = h;
  r.format = mv::codec::format_family::jpeg;
  r.rgba.assign(std::size_t{w} * h * 4, 255);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = &r.rgba[(std::size_t{y} * w + x) * 4];
      // Skin with a gentle gradient, so a fix that leaked would show.
      p[0] = static_cast<std::uint8_t>(190 + (x * 20) / w), p[1] = static_cast<std::uint8_t>(140 + (y * 20) / h),
      p[2] = 120;
      for (const drawn_face& f : faces) {
        for (float side : {-0.5f, 0.5f}) {
          const float ux = static_cast<float>(x) - (f.cx + side * f.d), uy = static_cast<float>(y) - f.cy;
          // A brow over each eye (edit::eyes_readable looks for one).
          if (uy > -0.36f * f.d && uy < -0.28f * f.d && std::abs(ux) < 0.28f * f.d) p[0] = 70, p[1] = 50, p[2] = 40;
          if (f.open) {
            if ((ux / (0.25f * f.d)) * (ux / (0.25f * f.d)) + (uy / (0.11f * f.d)) * (uy / (0.11f * f.d)) < 1.0f) {
              p[0] = p[1] = p[2] = 235;
            }
            if (ux * ux + uy * uy < (0.09f * f.d) * (0.09f * f.d)) p[0] = 25, p[1] = 20, p[2] = 18;
          } else if (std::abs(uy) < 1.5f && std::abs(ux) < 0.25f * f.d) {
            p[0] = 30, p[1] = 25, p[2] = 25;
          }
        }
      }
    }
  }
  mv::edit::encode_options o;
  o.quality = 95;
  auto bytes = mv::edit::encode(r, o, {});
  REQUIRE(bytes);
  return *bytes;
}

// A face of the pack's answer: its box (two eye distances wide, 2.4 tall),
// landmarks, and the donor face in `donor`.
std::string pack_face(float w, float h, const drawn_face& f, const std::string& donor_quoted, const drawn_face& d,
                      const char* extra = "") {
  char box[160];
  std::snprintf(box, sizeof box, "{\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,%s\"lm\":", (f.cx - f.d) / w,
                (f.cy - f.d) / h, 2 * f.d / w, 2.4f * f.d / h, extra);
  return std::string(box) + lm_json(w, h, f.cx, f.cy, f.d) + ",\"donors\":[{\"path\":" + donor_quoted +
         ",\"quality\":0.9,\"lm\":" + lm_json(w, h, d.cx, d.cy, d.d) + "}]}";
}

std::array<float, 4> region_of(float w, float h, const drawn_face& f) {
  mv::edit::face_landmarks lm{};
  auto parsed = mv::json::parse(lm_json(w, h, f.cx, f.cy, f.d));
  for (std::size_t i = 0; i < lm.size(); ++i) lm[i] = static_cast<float>(parsed->a[i].d);
  return mv::edit::face_region({(f.cx - f.d) / w, (f.cy - f.d) / h, 2 * f.d / w, 2.4f * f.d / h}, lm, w, h);
}

bool inside(const std::array<float, 4>& r, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
  const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
  // The region's pixels as fix_faces_in rounds them (floor / ceil).
  return x >= static_cast<std::uint32_t>(std::floor(r[0] * w)) && x < static_cast<std::uint32_t>(std::ceil((r[0] + r[2]) * w)) &&
         y >= static_cast<std::uint32_t>(std::floor(r[1] * h)) && y < static_cast<std::uint32_t>(std::ceil((r[1] + r[3]) * h)) &&
         fx >= 0.0f && fy >= 0.0f;
}

// How many pixels changed inside and outside `regions` (any channel's bits).
struct diff_count {
  std::size_t in = 0, out = 0;
};
diff_count diff(const mv::image::linear_image& a, const mv::image::linear_image& b,
                std::initializer_list<std::array<float, 4>> regions) {
  diff_count d;
  for (std::uint32_t y = 0; y < a.height; ++y) {
    for (std::uint32_t x = 0; x < a.width; ++x) {
      const std::size_t i = (std::size_t{y} * a.width + x) * 4;
      if (std::equal(a.rgba.begin() + static_cast<std::ptrdiff_t>(i), a.rgba.begin() + static_cast<std::ptrdiff_t>(i + 4),
                     b.rgba.begin() + static_cast<std::ptrdiff_t>(i))) {
        continue;
      }
      bool in = false;
      for (const auto& r : regions) in = in || inside(r, x, y, a.width, a.height);
      ++(in ? d.in : d.out);
    }
  }
  return d;
}

}  // namespace

TEST_CASE("faces: a fix changes only the chosen faces; nothing outside their boxes moves", "[enhance][eyes][faces-mask]") {
  scratch_dir s("faces_mask");
  constexpr std::uint32_t W = 800, H = 320;
  const drawn_face a{200, 150, 100, false}, b{600, 150, 100, false};
  const drawn_face da{205, 148, 102, true}, db{596, 152, 98, true};
  const fs::path photo = s / "IMG_0300.JPG", donor = s / "IMG_0301.JPG";
  mv::test::write_bytes(photo, faces_jpeg(W, H, {a, b}));
  mv::test::write_bytes(donor, faces_jpeg(W, H, {da, db}));
  mv::json::writer q;
  q.string(utf8(donor));
  const std::string dq = q.take();
  const std::string pack = "{\"faces\":[" + pack_face(W, H, a, dq, da, "\"quality\":0.2,") + "," +
                           pack_face(W, H, b, dq, db, "\"quality\":0.2,") + "]}";
  auto decoded = mv::image::decode_linear(mv::test::read_bytes(photo));
  REQUIRE(decoded);
  const mv::image::linear_image src = std::move(decoded).value();
  const mv::image::linear_image* source = &src;
  const auto ra = region_of(W, H, a), rb = region_of(W, H, b);
  REQUIRE(ra[0] + ra[2] < rb[0]);  // the two regions do not touch

  using mv::addon::media::face_fix;
  for (const face_fix fix : {face_fix::open_eyes, face_fix::improve, face_fix::do_all}) {
    INFO("fix " << static_cast<int>(fix));
    // Face 0 only: face 1 and everything around both are untouched.
    mv::image::linear_image only_a = *source;
    int found = 0;
    auto fa = mv::addon::media::fix_faces_in(only_a, pack, fix, 1u, nullptr, &found);
    REQUIRE(fa);
    CHECK(*fa == 1);
    CHECK(found == 2);
    const diff_count da_ = diff(*source, only_a, {ra});
    CHECK(da_.out == 0);  // the verify line: 0 outside the chosen box
    CHECK(da_.in > 100);
    CHECK(diff(*source, only_a, {rb}).in == 0);  // face 1's box: the same pixels
    // Face 1 only, the other way round.
    mv::image::linear_image only_b = *source;
    REQUIRE(mv::addon::media::fix_faces_in(only_b, pack, fix, 2u).value_or(-1) == 1);
    CHECK(diff(*source, only_b, {rb}).out == 0);
    CHECK(diff(*source, only_b, {ra}).in == 0);
    // Both: each face exactly as when it was fixed alone.
    mv::image::linear_image both = *source;
    REQUIRE(mv::addon::media::fix_faces_in(both, pack, fix, mv::addon::media::kAllFaces).value_or(-1) == 2);
    CHECK(diff(*source, both, {ra, rb}).out == 0);
    CHECK(diff(only_a, both, {rb}).out == 0);
    CHECK(diff(only_b, both, {ra}).out == 0);
    // Nothing chosen: nothing changes.
    mv::image::linear_image none = *source;
    REQUIRE(mv::addon::media::fix_faces_in(none, pack, fix, 0u).value_or(-1) == 0);
    CHECK(none.rgba == source->rgba);
  }
  // Open eyes really opened face 0's eyes.
  {
    mv::image::linear_image fixed = *source;
    REQUIRE(mv::addon::media::fix_faces_in(fixed, pack, face_fix::open_eyes, 1u).value_or(-1) == 1);
    mv::edit::face_landmarks lm{};
    auto parsed = mv::json::parse(lm_json(W, H, a.cx, a.cy, a.d));
    for (std::size_t i = 0; i < lm.size(); ++i) lm[i] = static_cast<float>(parsed->a[i].d);
    CHECK(mv::edit::eye_openness(fixed, lm) > mv::edit::eye_openness(*source, lm) * mv::edit::kEyesBetter);
  }

  // The file: "<stem>-eyes.jpg" beside the original, which is never written;
  // the face left out is the source's own (the JPEG's noise only).
  const auto before = mv::test::read_bytes(photo);
  auto r = mv::addon::media::fix_faces(utf8(photo), pack, face_fix::open_eyes, 2u);
  REQUIRE(r);
  CHECK(r->faces == 2);
  CHECK(r->fixed == 1);
  CHECK(fs::path(r->written).filename() == "IMG_0300-eyes.jpg");
  CHECK(mv::test::read_bytes(photo) == before);
  auto written = mv::image::decode_linear(mv::test::read_bytes(fs::path(r->written)));
  REQUIRE(written);
  double err_a = 0, err_b = 0;
  std::size_t n_a = 0, n_b = 0;
  for (std::uint32_t y = 0; y < H; ++y) {
    for (std::uint32_t x = 0; x < W; ++x) {
      const std::size_t i = (std::size_t{y} * W + x) * 4;
      const double e = std::abs(mv::image::half_to_float(written->rgba[i]) - mv::image::half_to_float(source->rgba[i]));
      if (inside(ra, x, y, W, H)) err_a += e, ++n_a;
      if (inside(rb, x, y, W, H)) err_b += e, ++n_b;
    }
  }
  CHECK(err_a / static_cast<double>(n_a) < 0.01);  // face 0, left out: re-encoded, not changed
  CHECK(err_b / static_cast<double>(n_b) > err_a / static_cast<double>(n_a) * 2);
  // Do all names its file "-faces"; nothing chosen writes nothing.
  auto all = mv::addon::media::fix_faces(utf8(photo), pack, face_fix::do_all, mv::addon::media::kAllFaces);
  REQUIRE(all);
  CHECK(all->fixed == 2);
  CHECK(fs::path(all->written).filename() == "IMG_0300-faces.jpg");
  auto nothing = mv::addon::media::fix_faces(utf8(photo), pack, face_fix::improve, 0u);
  REQUIRE(nothing);
  CHECK(nothing->written.empty());
  CHECK(mv::addon::media::fix_note(face_fix::do_all, mv::status::ok, all).find("2 faces") == 0);
}

TEST_CASE("faces: the canvas's chosen boxes find their faces in the run's answer", "[enhance][faces-mask]") {
  const std::string pack =
      "{\"faces\":[{\"x\":0.1,\"y\":0.2,\"w\":0.2,\"h\":0.3,\"lm\":[0,0,0,0,0,0,0,0,0,0]},"
      "{\"x\":0.6,\"y\":0.2,\"w\":0.2,\"h\":0.3,\"lm\":[0,0,0,0,0,0,0,0,0,0]},"
      "{\"x\":0.62,\"y\":0.25,\"w\":0.05,\"h\":0.05,\"lm\":[0,0,0,0,0,0,0,0,0,0]}]}";
  using mv::addon::media::faces_mask_of;
  // The second face, read a little differently by the earlier answer.
  const float second[4] = {0.61f, 0.21f, 0.19f, 0.31f};
  CHECK(faces_mask_of(pack, second) == 2u);
  // Both big faces; a box over nothing chooses nothing.
  const float two[12] = {0.1f, 0.2f, 0.2f, 0.3f, 0.6f, 0.2f, 0.2f, 0.3f, 0.4f, 0.8f, 0.05f, 0.05f};
  CHECK(faces_mask_of(pack, two) == 3u);
  // The same box twice takes one face, then the next best that overlaps
  // enough (here none: the small face's overlap is under 0.3).
  const float twice[8] = {0.6f, 0.2f, 0.2f, 0.3f, 0.6f, 0.2f, 0.2f, 0.3f};
  CHECK(faces_mask_of(pack, twice) == 2u);
  CHECK(faces_mask_of(pack, {}) == 0u);
  CHECK(faces_mask_of("not json", second) == 0u);
}

// S0 (docs/plans/edit-refresh.md §5): the writer audit. Every writer of a
// Photos-library item's result (Enhance's Detail, Remove and Extend through
// the host table; Open closed eyes; Improve faces; Save copy) reads the item
// through the resolve hook, stages its file outside the library, calls add
// exactly once with the source's key, and leaves nothing beside the source,
// whose bytes and stamp stay as they were; the staging folder is empty after.
namespace {

struct inpaint_grey final : inpaint_filler {
  std::uint32_t side() const noexcept override { return 64; }
  mv::result<rgb8> fill(const rgb8& image, const mask8& mask) override {
    rgb8 out = image;
    for (std::size_t i = 0; i < mask.a.size(); ++i) {
      if (mask.a[i] == 255) out.rgb[i * 3] = out.rgb[i * 3 + 1] = out.rgb[i * 3 + 2] = 128;
    }
    return out;
  }
};

void run_request(run_port port, job_request req, outcome& out) {
  job_runner r(std::move(port));
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

}  // namespace

TEST_CASE("photos: every writer of a library photo adds it once and writes nothing beside it",
          "[enhance][photos][audit]") {
  scratch_dir s("photos_audit");
  const fs::path library = s.root() / "Photos Library.photoslibrary" / "originals";
  fs::create_directories(library);
  const fs::path shut = library / "IMG_0101.JPG", open = library / "IMG_0102.JPG";
  mv::test::write_bytes(shut, face_jpeg(400, 300, 200, 150, 120, false));
  mv::test::write_bytes(open, face_jpeg(400, 300, 210, 145, 124, true));
  const auto shut_bytes = mv::test::read_bytes(shut), open_bytes = mv::test::read_bytes(open);
  const auto shut_stamp = fs::last_write_time(shut);
  const std::string kShut = "photos:SHUT-1/L0/001", kOpen = "photos:OPEN-2/L0/001";
  const auto staging = mv::addon::media::staging_dir();
  REQUIRE(staging);
  const fs::path staged_dir = fs::path(staging.value()).parent_path();

  struct calls {
    std::mutex m;
    std::vector<std::string> resolved;
    std::vector<std::pair<std::string, std::string>> added;  // staged file name, source
    bool staged_ok = true;  // every file handed over existed, in the staging folder
  } c;
  mv::addon::media::set_photos_hooks(
      [&](const std::string& key, const mv::job_context*) -> mv::result<std::string> {
        std::lock_guard lock(c.m);
        c.resolved.push_back(key);
        if (key == kShut) return utf8(shut);
        if (key == kOpen) return utf8(open);
        return mv::err(mv::status::not_found);
      },
      [&](const std::string& file, const std::string& source) -> mv::result<std::string> {
        // The runner's own thread for Enhance: no CHECK here, a flag instead.
        const fs::path f(file);
        const bool ok = fs::exists(f) && f.parent_path() == staged_dir &&  // staged outside the library
                        file.find(".photoslibrary") == std::string::npos;
        std::lock_guard lock(c.m);
        c.staged_ok = c.staged_ok && ok;
        c.added.emplace_back(utf8(f.filename()), source);
        return "photos:NEW-" + std::to_string(c.added.size()) + "/L0/001";
      });

  // After each writer: one more add, from the shut photo's key; the library
  // holds its two originals, unchanged; nothing of ours is left staged.
  std::size_t adds = 0;
  auto audit = [&](const char* writer, const std::string& suffix) {
    INFO(writer);
    std::lock_guard lock(c.m);
    REQUIRE(c.added.size() == adds + 1);
    CHECK(c.staged_ok);
    CHECK(c.added.back().second == kShut);
    CHECK(c.added.back().first.rfind("IMG_0101-" + suffix, 0) == 0);
    adds = c.added.size();
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(library)) files += e.is_regular_file() ? 1 : 0;
    CHECK(files == 2);
    CHECK(mv::test::read_bytes(shut) == shut_bytes);
    CHECK(mv::test::read_bytes(open) == open_bytes);
    CHECK((fs::last_write_time(shut) == shut_stamp));
    for (const auto& e : fs::directory_iterator(staged_dir)) {
      CHECK(utf8(e.path().filename()).rfind("IMG_0101", 0) != 0);
    }
  };

  // Enhance: Detail, Remove and Extend, through the host table's still rows.
  mv::addon::host_table table(still_services());
  run_port port = run_port_of(table.api(), "");
  port.load_tiles = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<tile_model>> {
    return std::unique_ptr<tile_model>(new nearest());
  };
  port.load_inpaint = [](const model_spec&, const std::string&) -> mv::result<std::unique_ptr<inpaint_filler>> {
    return std::unique_ptr<inpaint_filler>(new inpaint_grey);
  };
  port.still_mask = [](const std::string& path) { return mv::addon::media::still_mask(path); };
  {
    const model_spec spec = still_spec();
    outcome o;
    run_photo(port, kShut, spec, o);
    INFO(o.message);
    REQUIRE(o.ok);
    CHECK(o.name.rfind("photos:NEW-", 0) == 0);  // the engine says "Added to your Photos library"
    audit("detail", "enhance");
  }
  model_spec remove_spec;
  remove_spec.kind = model_kind::remove;
  remove_spec.run_bytes = 1;
  {
    mv::addon::media::set_still_mask(kShut, {0.5f, 0.5f, 0.05f});
    job_request req;
    req.path = kShut;
    req.item = item_kind::still;
    job_request::part p;
    p.kind = model_kind::remove;
    p.spec = &remove_spec;
    p.models_dir = "unused";
    req.parts.push_back(p);
    outcome o;
    run_request(port, req, o);
    INFO(o.message);
    REQUIRE(o.ok);
    audit("remove", "enhance");
    mv::addon::media::set_still_mask(kShut, {});
  }
  {
    job_request req;
    req.path = kShut;
    req.item = item_kind::still;
    job_request::part p;
    p.kind = model_kind::remove;
    p.spec = &remove_spec;
    p.models_dir = "unused";
    p.values["mode"] = *mv::json::parse("\"extend\"");
    p.values["extend"] = *mv::json::parse("\"wider\"");
    req.parts.push_back(p);
    outcome o;
    run_request(port, req, o);
    INFO(o.message);
    REQUIRE(o.ok);
    audit("extend", "extended");
  }
  {
    // edit-refresh S6: the frame dragged on the canvas, through the host
    // table's still_extend (the sheet's preset is then not used).
    mv::addon::media::set_still_extend(kShut, {0.1f, 0.0f, 0.2f, 0.0f});
    const mv_host_api* api = table.api();
    REQUIRE(api->struct_size >= offsetof(mv_host_api, still_extend) + sizeof(api->still_extend));
    REQUIRE(api->still_extend);
    float f[4] = {9, 9, 9, 9};
    REQUIRE(api->still_extend(api->host, kShut.c_str(), f) == MV_OK);
    CHECK(f[0] == 0.1f);
    CHECK(f[2] == 0.2f);
    REQUIRE(api->still_extend(api->host, kOpen.c_str(), f) == MV_OK);  // another photo: none
    CHECK(f[0] == 0.0f);
    CHECK(f[2] == 0.0f);
    REQUIRE(port.still_extend);
    job_request req;
    req.path = kShut;
    req.item = item_kind::still;
    job_request::part p;
    p.kind = model_kind::remove;
    p.spec = &remove_spec;
    p.models_dir = "unused";
    p.values["mode"] = *mv::json::parse("\"extend\"");
    p.values["extend"] = *mv::json::parse("\"taller\"");
    req.parts.push_back(p);
    outcome o;
    run_request(port, req, o);
    INFO(o.message);
    REQUIRE(o.ok);
    audit("extend frame", "extended");
    mv::addon::media::set_still_extend("", {});
    CHECK(mv::addon::media::still_extend(kShut)[0] == 0.0f);

    // The frame is measured against the edit it was set over: Fill's crop
    // undone (or any other turn or crop) and it no longer answers; a colour
    // change keeps it.
    mv::edit::geometry cropped;
    cropped.crop = {0.1f, 0.0f, 0.8f, 1.0f};
    mv::addon::media::set_still_edit(kShut, cropped, {});
    mv::addon::media::set_still_extend(kShut, {0.1f, 0.0f, 0.2f, 0.0f});
    CHECK(mv::addon::media::still_extend(kShut)[0] == 0.1f);
    mv::edit::colour warmer;
    warmer.v[static_cast<std::size_t>(mv::edit::adjust_param::mono)] = 100.0f;
    mv::addon::media::set_still_edit(kShut, cropped, warmer);
    CHECK(mv::addon::media::still_extend(kShut)[2] == 0.2f);
    mv::addon::media::clear_still_edit();  // the crop undone: nothing left to edit
    CHECK(mv::addon::media::still_extend(kShut)[0] == 0.0f);
    CHECK(mv::addon::media::still_extend(kShut)[2] == 0.0f);
    mv::addon::media::set_still_extend("", {});
  }

  // Open closed eyes and Improve faces: the target and the donor both by key.
  auto quoted = [](const std::string& v) {
    mv::json::writer w;
    w.string(v);
    return w.take();
  };
  {
    const std::string pack = std::string("{\"faces\":[{\"lm\":") + lm_json(400, 300, 200, 150, 120) +
                             ",\"donors\":[{\"path\":" + quoted(kOpen) + ",\"lm\":" + lm_json(400, 300, 210, 145, 124) +
                             "}]}]}";
    auto r = mv::addon::media::open_closed_eyes(kShut, pack);
    REQUIRE(r);
    CHECK(r->fixed == 1);
    CHECK(r->written.rfind("photos:NEW-", 0) == 0);
    CHECK(mv::addon::media::eyes_note(mv::status::ok, r).find("added to your Photos library") != std::string::npos);
    audit("eyes", "eyes");
  }
  {
    const std::string pack = std::string("{\"faces\":[{\"quality\":0.2,\"lm\":") + lm_json(400, 300, 200, 150, 120) +
                             ",\"donors\":[{\"path\":" + quoted(kOpen) + ",\"quality\":0.9,\"lm\":" +
                             lm_json(400, 300, 210, 145, 124) + "}]}]}";
    auto r = mv::addon::media::improve_faces(kShut, pack);
    REQUIRE(r);
    REQUIRE(r->fixed == 1);
    CHECK(r->written.rfind("photos:NEW-", 0) == 0);
    audit("faces", "faces");
  }

  // Save copy (the hosts' ⌘S on a library item): the item's file, exported
  // into the staging folder by the export (here a stand-in), then added.
  {
    auto r = mv::addon::media::save_copy_to_library(
        kShut, [&](const std::string& file, const std::string& dir) -> mv::result<std::string> {
          CHECK(file == utf8(shut));
          const std::string out = dir + "IMG_0101-edit.jpg";
          mv::test::write_bytes(fs::path(out), mv::test::read_bytes(fs::path(file)));
          return out;
        });
    REQUIRE(r);
    CHECK(r.value().rfind("photos:NEW-", 0) == 0);
    audit("save copy", "edit");
  }
  {
    std::lock_guard lock(c.m);
    CHECK(c.added.size() == 7);  // S6 adds the canvas frame's Extend
    for (const std::string& k : c.resolved) CHECK((k == kShut || k == kOpen));
  }

  // A refusal (no access): nothing added, nothing left staged, the reason kept.
  mv::addon::media::set_photos_hooks(
      [&](const std::string&, const mv::job_context*) -> mv::result<std::string> { return utf8(shut); },
      [](const std::string&, const std::string&) -> mv::result<std::string> {
        return mv::err(mv::status::permission_denied);
      });
  mv::addon::rgb_image px;
  px.width = 8;
  px.height = 8;
  px.rgb.assign(8 * 8 * 3, 100);
  auto refused = mv::addon::media::write_still(kShut, px, "enhance");
  REQUIRE_FALSE(refused);
  CHECK(refused.error() == mv::status::permission_denied);
  for (const auto& e : fs::directory_iterator(staged_dir)) CHECK(utf8(e.path().filename()).rfind("IMG_0101", 0) != 0);
  mv::addon::media::set_photos_hooks(nullptr, nullptr);
}

// A 12-megapixel photo with the real model, to twice the size, timed.
TEST_CASE("enhance: Real-ESRGAN on a 12 MP photo", "[.enhance-model]") {
  const char* esr = std::getenv("MV_ENHANCE_ESRGAN");
  const char* runtime = std::getenv("MV_ENHANCE_RUNTIME_DIR");
  if (!esr || !runtime) SKIP("MV_ENHANCE_ESRGAN / MV_ENHANCE_RUNTIME_DIR not set");
  scratch_dir s("enhance_12mp");
  const fs::path photo = s / "IMG_0002.JPG";
  mv::test::write_bytes(photo, sideways_jpeg(4032, 3024));
  mv::addon::host_table table(still_services());
  run_port port = run_port_of(table.api(), runtime);
  const std::string dir = utf8(fs::path(esr).parent_path());
  auto real = port.load_tiles;
  port.load_tiles = [real, dir](const model_spec& sp, const std::string&) { return real(sp, dir); };
  auto spec = parse_spec(R"json({"kind": "still", "name": "Detail", "weights": ["realesr-general-x4v3.onnx"],
    "run_bytes": 600000000, "cpu": true,
    "knobs": [{"id": "scale", "label": "Size", "type": "choice",
               "options": [{"id": "1", "text": "Same"}, {"id": "2", "text": "2x"}, {"id": "4", "text": "4x"}], "default": "2"}],
    "io": {"runner": "tiles", "scale": 4, "tile": 256, "overlap": 16, "dims": ["h", "w"]}})json",
                         model_kind::still);
  REQUIRE(spec);
  const auto t0 = std::chrono::steady_clock::now();
  outcome a;
  run_photo(port, utf8(photo), *spec, a);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  INFO(a.message);
  REQUIRE(a.ok);
  WARN("4032 x 3024 to 6048 x 8064 (upright) in " << secs << " s");
}

// A bench for the face fixes on real photos (hidden): MV_FACES_PHOTO the
// photo, MV_FACES_PACK a file holding eyes_json's answer for it (its donors'
// paths and landmarks), MV_FACES_FIX -1 / 0 / 1 (default 1, Improve) and
// MV_FACES_MASK the chosen faces (default every face). The fix runs as the
// Faces tool runs it; the new file's path is printed. Look at the result.
TEST_CASE("faces: bench on real photos", "[.faces-bench]") {
  const char* photo = std::getenv("MV_FACES_PHOTO");
  const char* pack_path = std::getenv("MV_FACES_PACK");
  if (!photo || !pack_path) SKIP("set MV_FACES_PHOTO and MV_FACES_PACK");
  const auto bytes = mv::test::read_bytes(pack_path);
  const std::string pack(bytes.begin(), bytes.end());
  const char* fix_env = std::getenv("MV_FACES_FIX");
  const char* mask_env = std::getenv("MV_FACES_MASK");
  const auto fix = static_cast<mv::addon::media::face_fix>(fix_env ? std::atoi(fix_env) : 1);
  const std::uint64_t mask = mask_env ? std::strtoull(mask_env, nullptr, 10) : mv::addon::media::kAllFaces;
  const auto r = mv::addon::media::fix_faces(photo, pack, fix, mask);
  REQUIRE(r);
  std::printf("faces %d fixed %d written %s\n", r->faces, r->fixed, r->written.c_str());
}

TEST_CASE("faces: a fix also offers the chosen face's person's photos as donors", "[enhance][eyes]") {
  // Two faces: person 7 (chosen) and person 9.
  const std::string first =
      "{\"faces\":[{\"x\":0.1,\"y\":0.1,\"w\":0.2,\"h\":0.3,\"lm\":[0.15,0.2,0.25,0.2,0.2,0.25,0.16,0.3,0.24,0.3],"
      "\"person\":7,\"donors\":[]},{\"x\":0.6,\"y\":0.1,\"w\":0.2,\"h\":0.3,"
      "\"lm\":[0.65,0.2,0.75,0.2,0.7,0.25,0.66,0.3,0.74,0.3],\"person\":9,\"donors\":[]}]}";
  std::vector<std::string> asked;
  const mv::addon::media::eyes_ask_fn ask = [&](const std::string&, const std::string& c) -> mv::result<std::string> {
    asked.push_back(c);
    return first;
  };
  std::vector<std::uint64_t> people;
  mv::addon::media::set_person_photos([&](std::uint64_t person) -> mv::result<std::string> {
    people.push_back(person);
    // A clip frame (never offered), the photo itself, a frame already offered,
    // a small face and a large one.
    return std::string("[{\"face\":1,\"path\":\"/p/clip.mov\",\"pts_ms\":1200,\"box\":[0,0,0.9,0.9]},"
                       "{\"face\":2,\"path\":\"/p/self.jpg\",\"pts_ms\":-1,\"box\":[0,0,0.8,0.8]},"
                       "{\"face\":3,\"path\":\"/p/moment.jpg\",\"pts_ms\":-1,\"box\":[0,0,0.7,0.7]},"
                       "{\"face\":4,\"path\":\"/p/small.jpg\",\"pts_ms\":-1,\"box\":[0,0,0.1,0.1]},"
                       "{\"face\":5,\"path\":\"/p/large.jpg\",\"pts_ms\":-1,\"box\":[0,0,0.5,0.5]}]");
  });
  const std::vector<float> chosen{0.1f, 0.1f, 0.2f, 0.3f};  // face 0's box
  auto r = mv::addon::media::eyes_answer("/p/self.jpg", "/p/moment.jpg\n", chosen, ask);
  mv::addon::media::set_person_photos(nullptr);
  REQUIRE(r);
  REQUIRE(people.size() == 1);
  CHECK(people[0] == 7);  // the chosen face's person only
  REQUIRE(asked.size() == 2);
  CHECK(asked[0] == "/p/moment.jpg\n");
  CHECK(asked[1] == "/p/moment.jpg\n/p/large.jpg\n/p/small.jpg\n");  // largest first; no clip, self or repeat
  // Without the hook: one question, the moment's frames.
  asked.clear();
  REQUIRE(mv::addon::media::eyes_answer("/p/self.jpg", "/p/moment.jpg\n", chosen, ask));
  CHECK(asked.size() == 1);
}
