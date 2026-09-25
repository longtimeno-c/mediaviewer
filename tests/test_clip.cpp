// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 13 (two-path trim) and PR 14 (extract & remux), shared core.
// plan/10 Milestone E verify lines, walked on synthetic clips (clip_fixture.h):
//   * keyframe trim is proportional in size and snaps to the keyframe grid;
//   * the re-encode path is frame-accurate;
//   * the source file is never modified;
//   * cancelling leaves no partial output;
//   * each PR 14 operation round-trips; lossless rotate does not re-encode.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <spng.h>
}

#include "clip_fixture.h"
#include "edit/clip.h"
#include "edit/clip_internal.h"
#include "edit/clip_jobs.h"
#include "edit/hwencode.h"
#include "import_fixture.h"

using namespace mv::test;
namespace clip = mv::edit::clip;
namespace fx = mv::test::clipfx;

namespace {

constexpr std::int64_t kFrameNs = 1'000'000'000 / 30;

std::vector<std::string> names_in(const fs::path& dir) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) out.push_back(utf8(e.path().filename()));
  std::sort(out.begin(), out.end());
  return out;
}

clip::request req_for(clip::op kind, const fs::path& src) {
  clip::request r;
  r.kind = kind;
  r.source = utf8(src);
  return r;
}

// Frame index for a decoded luma (bottom half is 16 + 4 * (i % 50)).
int frame_of(int luma) { return (luma - 16 + 2) / 4; }

// Path 2 with FFmpeg's own MPEG-4 encoder: the product path takes hardware
// encoders only, which a CI runner does not have.
mv::result<clip::outcome> run_soft(const clip::request& r, const clip::control& c = {}) {
  return clip::detail::run_with_encoders(r, c, {"mpeg4"}, true);
}

}  // namespace

TEST_CASE("probe lists every keyframe on the player's timeline", "[clip][pr13]") {
  scratch_dir d("clip_probe");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  auto info = clip::probe(utf8(src));
  REQUIRE(info);
  CHECK(info->has_video);
  CHECK(info->has_audio);
  CHECK(info->width == 320);
  CHECK(info->container == "mp4");
  REQUIRE(info->keyframes_ns.size() == 8);  // 120 frames, GOP 15
  for (std::size_t i = 0; i < info->keyframes_ns.size(); ++i) {
    CHECK(std::llabs(info->keyframes_ns[i] - static_cast<std::int64_t>(i) * 15 * kFrameNs) < 1'000'000);
  }
  CHECK(std::llabs(info->duration_ns - 4'000'000'000) < 100'000'000);
}

TEST_CASE("snapping helpers", "[clip][pr13]") {
  const std::vector<std::int64_t> kf{0, 500, 1000, 1500};
  CHECK(clip::keyframe_at_or_before(kf, 999) == 500);
  CHECK(clip::keyframe_at_or_before(kf, 1000) == 1000);
  CHECK(clip::keyframe_at_or_after(kf, 1001, 2000) == 1500);
  CHECK(clip::keyframe_at_or_after(kf, 1501, 2000) == 2000);
  CHECK(clip::keyframe_nearest(kf, 740) == 500);
  CHECK(clip::keyframe_nearest(kf, 760) == 1000);
  clip::clip_info info;
  info.duration_ns = 2000;
  info.keyframes_ns = kf;
  const auto r = clip::keyframe_range(info, 600, 1200);
  CHECK(r.in_ns == 500);
  CHECK(r.out_ns == 1500);
  const auto whole = clip::keyframe_range(info, 0, -1);
  CHECK(whole.out_ns == 2000);
}

TEST_CASE("keyframe trim: stream copy on the grid, proportional, source untouched", "[clip][pr13]") {
  for (const char* muxer : {"mp4", "matroska"}) {
    for (int b : {0, 2}) {
      DYNAMIC_SECTION(muxer << " b_frames=" << b) {
        scratch_dir d("clip_trim");
        const fs::path src = d / (std::string("clip") + (std::strcmp(muxer, "mp4") == 0 ? ".mp4" : ".mkv"));
        fx::spec s;
        s.muxer = muxer;
        s.b_frames = b;
        s.frames = 300;
        REQUIRE(fx::make(utf8(src), s));
        const auto before = read_bytes(src);
        auto info = clip::probe(utf8(src));
        REQUIRE(info);
        const auto& kf = info->keyframes_ns;
        REQUIRE(kf.size() > 8);
        fx::decoded whole;
        REQUIRE(fx::decode_all(utf8(src), whole));

        auto r = req_for(clip::op::trim_keyframe, src);
        r.in_ns = kf[2] + (kf[3] - kf[2]) / 2;  // between two keyframes
        r.out_ns = kf[6] + (kf[7] - kf[6]) / 2;
        auto done = clip::run(r, {});
        REQUIRE(done);
        REQUIRE(done->outputs.size() == 1);
        CHECK(done->written.in_ns == kf[2]);   // snapped back
        CHECK(done->written.out_ns == kf[7]);  // snapped forward
        CHECK(fs::path(done->outputs[0]).filename().string().find("_trimmed") != std::string::npos);

        // Exactly the source frames in [kf2, kf7), in order.
        std::vector<int> expect;
        for (std::size_t i = 0; i < whole.pts_ns.size(); ++i) {
          if (whole.pts_ns[i] >= kf[2] - 1'000'000 && whole.pts_ns[i] < kf[7] - 1'000'000) expect.push_back(whole.luma[i]);
        }
        fx::decoded out;
        REQUIRE(fx::decode_all(done->outputs[0], out));
        CHECK(out.luma == expect);
        CHECK(out.pts_ns.front() < 3 * kFrameNs);  // re-timed to start at zero
        CHECK(out.audio_packets > 0);

        // Proportional: the share of frames kept, within container overhead.
        const double share = static_cast<double>(expect.size()) / static_cast<double>(whole.luma.size());
        const double ratio = static_cast<double>(fs::file_size(done->outputs[0])) / static_cast<double>(before.size());
        CHECK(ratio > share * 0.85);
        CHECK(ratio < share * 1.15);
        CHECK(read_bytes(src) == before);  // rule 5
      }
    }
  }
}

TEST_CASE("re-encode trim is frame-accurate and labelled with its encoder", "[clip][pr13]") {
  scratch_dir d("clip_reencode");
  const fs::path src = d / "clip.mp4";
  fx::spec s;
  s.frames = 150;
  s.b_frames = 2;
  REQUIRE(fx::make(utf8(src), s));
  const auto before = read_bytes(src);
  auto r = req_for(clip::op::trim_reencode, src);
  r.in_ns = 37 * kFrameNs;
  r.out_ns = 88 * kFrameNs;
  auto done = run_soft(r);
  REQUIRE(done);
  CHECK(done->encoder == "mpeg4");
  fx::decoded out;
  REQUIRE(fx::decode_all(done->outputs[0], out));
  REQUIRE(out.luma.size() == 51);  // exactly frames 37..87
  CHECK(frame_of(out.luma.front()) == 37);
  CHECK(frame_of(out.luma.back()) == 87 % 50);
  CHECK(out.audio_packets > 0);
  CHECK(read_bytes(src) == before);
}

TEST_CASE("the product re-encode path refuses software encoders", "[clip][pr13]") {
  scratch_dir d("clip_licence");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  auto r = req_for(clip::op::trim_reencode, src);
  r.out_ns = 30 * kFrameNs;
  // mpeg4 is software; x264 / x265 are refused by name whatever a port says.
  auto soft = clip::detail::run_with_encoders(r, {}, {"mpeg4", "libx264", "libx265"}, false);
  CHECK(soft.error() == mv::status::unsupported_format);
  CHECK(names_in(d.root()) == std::vector<std::string>{"clip.mp4"});
  // The product path uses the encode port only: nothing on a headless build,
  // a hardware encoder where the machine has one.
  auto product = clip::run(r, {});
  if (product) {
    bool listed = false;
    for (auto c : {mv::edit::hwencode::codec::h264, mv::edit::hwencode::codec::hevc}) {
      for (const char* n : mv::edit::hwencode::candidates(c)) listed = listed || product->encoder == n;
    }
    CHECK(listed);
  } else {
    CHECK(product.error() == mv::status::unsupported_format);
  }
}

TEST_CASE("cancelling leaves no partial output", "[clip][pr13]") {
  for (clip::op kind : {clip::op::trim_keyframe, clip::op::trim_reencode, clip::op::split, clip::op::animation}) {
    DYNAMIC_SECTION("op " << static_cast<int>(kind)) {
      scratch_dir d("clip_cancel");
      const fs::path src = d / "clip.mp4";
      fx::spec s;
      s.frames = 240;
      REQUIRE(fx::make(utf8(src), s));
      std::atomic<bool> cancel{false};
      auto r = req_for(kind, src);
      r.in_ns = kind == clip::op::split ? 120 * kFrameNs : 0;
      r.out_ns = kind == clip::op::split ? -1 : 200 * kFrameNs;
      clip::control c;
      c.cancel = &cancel;
      c.user = &cancel;
      c.progress = [](void* user, double f) noexcept {
        if (f > 0.3) static_cast<std::atomic<bool>*>(user)->store(true);
      };
      auto done = kind == clip::op::trim_reencode || kind == clip::op::animation ? run_soft(r, c) : clip::run(r, c);
      CHECK(done.error() == mv::status::cancelled);
      CHECK(names_in(d.root()) == std::vector<std::string>{"clip.mp4"});
    }
  }
}

TEST_CASE("a second trim of the same range takes the next free name", "[clip][pr13]") {
  scratch_dir d("clip_names");
  const fs::path src = d / "IMG_0001.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  auto r = req_for(clip::op::trim_keyframe, src);
  r.in_ns = 0;
  r.out_ns = 30 * kFrameNs;
  REQUIRE(clip::run(r, {}));
  REQUIRE(clip::run(r, {}));
  CHECK(names_in(d.root()) ==
        std::vector<std::string>{"IMG_0001.mp4", "IMG_0001_trimmed (2).mp4", "IMG_0001_trimmed.mp4"});
}

TEST_CASE("lossless rotate writes a new matrix and the same packets", "[clip][pr14]") {
  scratch_dir d("clip_rotate");
  const fs::path src = d / "clip.mov";
  fx::spec s;
  s.muxer = "mov";
  s.rotation = 90;
  REQUIRE(fx::make(utf8(src), s));
  auto r = req_for(clip::op::rotate, src);
  r.rotate_degrees = 90;
  auto done = clip::run(r, {});
  REQUIRE(done);
  CHECK(fs::path(done->outputs[0]).extension() == ".mov");
  auto info = clip::probe(done->outputs[0]);
  REQUIRE(info);
  CHECK(info->rotation == 180);
  CHECK(fx::video_packets(done->outputs[0]) == fx::video_packets(utf8(src)));  // no re-encode
  r.source = done->outputs[0];
  r.rotate_degrees = 180;
  auto back = clip::run(r, {});
  REQUIRE(back);
  CHECK(clip::probe(back->outputs[0])->rotation == 0);
}

TEST_CASE("split at the nearest keyframe gives two halves that sum to the clip", "[clip][pr14]") {
  scratch_dir d("clip_split");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  auto r = req_for(clip::op::split, src);
  r.in_ns = 62 * kFrameNs;  // nearest keyframe is 60
  auto done = clip::run(r, {});
  REQUIRE(done);
  REQUIRE(done->outputs.size() == 2);
  fx::decoded a, b;
  REQUIRE(fx::decode_all(done->outputs[0], a));
  REQUIRE(fx::decode_all(done->outputs[1], b));
  CHECK(a.luma.size() == 60);
  CHECK(b.luma.size() == 60);
  CHECK(frame_of(b.luma.front()) == 60 % 50);
  CHECK(fs::path(done->outputs[0]).filename() == "clip_part1.mp4");
  CHECK(fs::path(done->outputs[1]).filename() == "clip_part2.mp4");
}

TEST_CASE("remove-middle joins the two sides on keyframes", "[clip][pr14]") {
  for (int b : {0, 2}) {
    DYNAMIC_SECTION("b_frames=" << b) {
      scratch_dir d("clip_cut");
      const fs::path src = d / "clip.mp4";
      fx::spec s;
      s.b_frames = b;
      REQUIRE(fx::make(utf8(src), s));
      auto info = clip::probe(utf8(src));
      REQUIRE(info);
      const auto& kf = info->keyframes_ns;
      fx::decoded whole;
      REQUIRE(fx::decode_all(utf8(src), whole));
      auto r = req_for(clip::op::remove_middle, src);
      r.in_ns = kf[2] + kFrameNs;   // nearest: kf[2]
      r.out_ns = kf[5] - kFrameNs;  // nearest: kf[5]
      auto done = clip::run(r, {});
      REQUIRE(done);
      CHECK(done->written.in_ns == kf[2]);
      CHECK(done->written.out_ns == kf[5]);
      std::vector<int> expect;
      for (std::size_t i = 0; i < whole.pts_ns.size(); ++i) {
        const auto t = whole.pts_ns[i];
        if (t < kf[2] - 1'000'000 || t >= kf[5] - 1'000'000) expect.push_back(whole.luma[i]);
      }
      fx::decoded out;
      REQUIRE(fx::decode_all(done->outputs[0], out));
      CHECK(out.luma == expect);
      for (std::size_t i = 1; i < out.pts_ns.size(); ++i) CHECK(out.pts_ns[i] > out.pts_ns[i - 1]);
    }
  }
}

TEST_CASE("remux MP4 -> MKV -> MP4 keeps every packet", "[clip][pr14]") {
  scratch_dir d("clip_remux");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  auto r = req_for(clip::op::remux, src);
  r.remux = clip::remux_target::mkv;
  auto mkv = clip::run(r, {});
  REQUIRE(mkv);
  CHECK(fs::path(mkv->outputs[0]).extension() == ".mkv");
  CHECK(clip::probe(mkv->outputs[0])->container == "matroska");
  r.source = mkv->outputs[0];
  r.remux = clip::remux_target::mp4;
  auto mp4 = clip::run(r, {});
  REQUIRE(mp4);
  CHECK(fs::path(mp4->outputs[0]).filename() == "clip (2).mp4");  // never over the source
  CHECK(fx::video_packets(mp4->outputs[0]) == fx::video_packets(utf8(src)));
  fx::decoded a, b;
  REQUIRE(fx::decode_all(utf8(src), a));
  REQUIRE(fx::decode_all(mp4->outputs[0], b));
  CHECK(a.audio_packets == b.audio_packets);
}

TEST_CASE("frame -> PNG and JPEG is the frame on screen, upright", "[clip][pr14]") {
  scratch_dir d("clip_frame");
  const fs::path src = d / "clip.mp4";
  fx::spec s;
  s.rotation = 90;
  REQUIRE(fx::make(utf8(src), s));
  auto r = req_for(clip::op::frame, src);
  r.in_ns = 45 * kFrameNs + kFrameNs / 2;  // mid-frame 45
  auto png = clip::run(r, {});
  REQUIRE(png);
  CHECK(fs::path(png->outputs[0]).filename() == "clip_frame_00-01.516.png");
  const auto bytes = read_bytes(png->outputs[0]);
  spng_ctx* ctx = spng_ctx_new(0);
  REQUIRE(spng_set_png_buffer(ctx, bytes.data(), bytes.size()) == 0);
  spng_ihdr ihdr{};
  REQUIRE(spng_get_ihdr(ctx, &ihdr) == 0);
  CHECK(ihdr.width == 240);  // rotated 90: portrait
  CHECK(ihdr.height == 320);
  std::size_t size = 0;
  REQUIRE(spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size) == 0);
  std::vector<std::uint8_t> px(size);
  REQUIRE(spng_decode_image(ctx, px.data(), size, SPNG_FMT_RGBA8, 0) == 0);
  spng_ctx_free(ctx);
  // After a clockwise turn the source's bottom half is the left half. Its
  // luma (limited range 196) is full-range ~(196-16)*255/219.
  const std::size_t mid = (static_cast<std::size_t>(160) * 240 + 20) * 4;
  CHECK(std::abs(static_cast<int>(px[mid]) - (fx::luma_of_frame(45) - 16) * 255 / 219) <= 3);

  r.frame = clip::frame_format::jpeg;
  auto jpg = clip::run(r, {});
  REQUIRE(jpg);
  const auto jb = read_bytes(jpg->outputs[0]);
  REQUIRE(jb.size() > 4);
  CHECK((jb[0] == 0xFF && jb[1] == 0xD8));
}

TEST_CASE("audio extract: copy, WAV and FLAC", "[clip][pr14]") {
  scratch_dir d("clip_audio");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  for (auto fmt : {clip::audio_format::copy, clip::audio_format::wav, clip::audio_format::flac}) {
    auto r = req_for(clip::op::audio, src);
    r.audio = fmt;
    auto done = clip::run(r, {});
    REQUIRE(done);
    auto info = clip::probe(done->outputs[0]);
    REQUIRE(info);
    CHECK(info->has_audio);
    CHECK_FALSE(info->has_video);
    CHECK(std::llabs(info->duration_ns - 4'000'000'000) < 150'000'000);
  }
  // The copy's box follows the codec (the fixture's MPEG audio reads back as MP3).
  CHECK(names_in(d.root()).size() == 4);
  CHECK(fs::exists(d / "clip_audio.wav"));
  CHECK(fs::exists(d / "clip_audio.flac"));
  // A range is sample-accurate on the transcodes.
  auto r = req_for(clip::op::audio, src);
  r.audio = clip::audio_format::wav;
  r.in_ns = 1'000'000'000;
  r.out_ns = 2'500'000'000;
  auto part = clip::run(r, {});
  REQUIRE(part);
  // 1.5 s of 48 kHz stereo 16-bit plus a small header.
  const auto size = fs::file_size(part->outputs[0]);
  CHECK(size > 288'000);
  CHECK(size < 288'000 + 200);
}

TEST_CASE("clip -> GIF (two-pass palette) and WebP", "[clip][pr14]") {
  scratch_dir d("clip_anim");
  const fs::path src = d / "clip.mp4";
  REQUIRE(fx::make(utf8(src), {}));
  for (auto fmt : {clip::anim_format::gif, clip::anim_format::webp}) {
    auto r = req_for(clip::op::animation, src);
    r.animation = fmt;
    r.in_ns = 1'000'000'000;
    r.out_ns = 3'000'000'000;
    r.animation_fps = 10;
    r.animation_width = 160;
    auto done = clip::run(r, {});
    if (fmt == clip::anim_format::webp && !done && done.error() == mv::status::unsupported_format) {
      WARN("this FFmpeg has no libwebp_anim; the product builds enable it");
      continue;
    }
    REQUIRE(done);
    if (fmt == clip::anim_format::gif) {
      fx::decoded out;
      REQUIRE(fx::decode_all(done->outputs[0], out));
      CHECK(out.luma.size() == 20);  // 2 s at 10 fps
      CHECK(out.width == 160);
      CHECK(out.height == 120);
    } else {
      // FFmpeg reads no animated WebP, so count the RIFF's frame chunks.
      const auto b = read_bytes(done->outputs[0]);
      REQUIRE(b.size() > 12);
      CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
      CHECK(std::memcmp(b.data() + 8, "WEBP", 4) == 0);
      int frames = 0;
      for (std::size_t i = 12; i + 8 <= b.size();) {
        const std::uint32_t len = b[i + 4] | (b[i + 5] << 8) | (b[i + 6] << 16) | (static_cast<std::uint32_t>(b[i + 7]) << 24);
        if (std::memcmp(&b[i], "ANMF", 4) == 0) ++frames;
        i += 8 + len + (len & 1);
      }
      // libwebp merges identical neighbours; the fixture changes every frame.
      CHECK(frames == 20);
    }
  }
  // Longer than a minute is a clip, not an animation.
  const fs::path longer = d / "long.mp4";
  fx::spec s;
  s.fps = 5;
  s.frames = 5 * 65;
  s.gop = 5;
  s.audio = false;
  REQUIRE(fx::make(utf8(longer), s));
  auto r = req_for(clip::op::animation, longer);
  CHECK(clip::run(r, {}).error() == mv::status::invalid_arg);
}

TEST_CASE("the job queue runs, cancels and retries", "[clip][pr13][jobs]") {
  scratch_dir d("clip_jobs");
  const fs::path src = d / "clip.mp4";
  fx::spec s;
  s.frames = 300;
  REQUIRE(fx::make(utf8(src), s));

  struct events {
    std::atomic<int> finished{0};
  } ev;
  clip::job_queue q;
  q.set_listener(
      [](void* user, std::uint64_t, clip::job_state st) noexcept {
        if (st == clip::job_state::done || st == clip::job_state::failed || st == clip::job_state::cancelled) {
          static_cast<events*>(user)->finished.fetch_add(1);
        }
      },
      &ev);
  auto trim = req_for(clip::op::trim_keyframe, src);
  trim.out_ns = 60 * kFrameNs;
  const auto a = q.submit(trim);
  auto remux = req_for(clip::op::remux, src);
  remux.remux = clip::remux_target::mkv;
  const auto b = q.submit(remux);
  const auto c = q.submit(req_for(clip::op::trim_reencode, src));  // no hardware encoder here
  CHECK(q.cancel(b));  // likely still queued; either way it must not publish
  for (int i = 0; i < 400 && ev.finished.load() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(ev.finished.load() == 3);
  clip::job_snapshot sa, sb, sc;
  REQUIRE(q.snapshot(a, sa));
  REQUIRE(q.snapshot(b, sb));
  REQUIRE(q.snapshot(c, sc));
  CHECK(sa.state == clip::job_state::done);
  CHECK(sa.fraction == 1.0);
  CHECK(sa.outputs.size() == 1);
  CHECK(sa.source_name == "clip.mp4");
  CHECK(sb.state == clip::job_state::cancelled);
  // Path 2: failed for want of a hardware encoder, or done on one.
  if (sc.state == clip::job_state::failed) {
    CHECK(sc.error == mv::status::unsupported_format);
  } else {
    CHECK(sc.state == clip::job_state::done);
  }
  CHECK_FALSE(q.busy());
  const auto again = q.retry(sb.id);
  CHECK(again != 0);
  CHECK(q.retry(a) == 0);  // done jobs are not retried
  for (int i = 0; i < 400 && ev.finished.load() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  clip::job_snapshot sr;
  REQUIRE(q.snapshot(again, sr));
  CHECK(sr.state == clip::job_state::done);
  q.clear_finished();
  CHECK(q.ids().empty());
  const auto left = names_in(d.root());
  CHECK(std::count(left.begin(), left.end(), "clip.mkv") == 1);
  CHECK(std::count(left.begin(), left.end(), "clip_trimmed.mp4") == 1);
  for (const auto& n : left) CHECK(n.find(".mvpart") == std::string::npos);
}


// plan/10 PR 13 verify: "keyframe trim of a 1 GB MP4 completes in seconds
// with proportional output size". Hidden ([.]): it writes ~1 GB first. Run it
// with `mv_clip_tests "[bench]"`; MV_CLIP_BENCH_FILE points it at a real clip.
TEST_CASE("keyframe trim of a 1 GB MP4 takes seconds", "[.][bench]") {
  scratch_dir d("clip_bench");
  std::string src;
  if (const char* env = std::getenv("MV_CLIP_BENCH_FILE"); env != nullptr && *env != '\0') {
    src = env;
  } else {
    src = utf8(d / "big.mp4");
    fx::spec s;
    s.width = 1920;
    s.height = 1080;
    s.frames = 1500;
    s.gop = 30;
    REQUIRE(fx::make(src, s));
  }
  const auto size = fs::file_size(src);
  auto info = clip::probe(src);
  REQUIRE(info);
  auto r = req_for(clip::op::trim_keyframe, fs::path(src));
  r.out_dir = utf8(d.root());
  r.in_ns = info->duration_ns / 4;
  r.out_ns = info->duration_ns / 2;
  const auto t0 = std::chrono::steady_clock::now();
  auto done = clip::run(r, {});
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(done);
  const double ratio = static_cast<double>(fs::file_size(done->outputs[0])) / static_cast<double>(size);
  WARN("source " << size / (1024 * 1024) << " MB, trim (probe + copy) " << secs << " s, output share "
                 << ratio << " for a range share "
                 << static_cast<double>(done->written.out_ns - done->written.in_ns) / static_cast<double>(info->duration_ns));
  CHECK(secs < 10.0);
}
