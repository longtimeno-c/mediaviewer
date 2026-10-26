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
#include "addons/enhance/picture_job.h"
#include "addons/enhance/runner.h"
#include "addons/enhance/stream_model.h"
#include "addons/enhance/spec.h"
#include "clip_fixture.h"
#include "codec/decode.h"
#include "codec/exif.h"
#include "edit/clip_sample.h"
#include "edit/encode.h"
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
