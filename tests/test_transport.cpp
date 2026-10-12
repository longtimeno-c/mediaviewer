// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 5c transport policy, headless. No clip, no device, no clock.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "player/resume_store.h"
#include "player/transport.h"

using namespace mv::player;

namespace {
constexpr time_ns sec(double v) { return static_cast<time_ns>(v * 1'000'000'000.0); }
}  // namespace

// --- seek mode --------------------------------------------------------------

TEST_CASE("dragging the scrubber seeks to a keyframe, releasing is exact", "[transport]") {
  // docs/design/05's fast-then-accurate rule. Getting this backwards makes scrubbing
  // feel like treacle, which is the whole thing 5c's verify line asks for.
  REQUIRE(make_seek(sec(30), /*dragging=*/true, 1).mode == seek_mode::keyframe);
  REQUIRE(make_seek(sec(30), /*dragging=*/false, 1).mode == seek_mode::exact);
}

TEST_CASE("a negative seek target is clamped to the start", "[transport]") {
  REQUIRE(make_seek(sec(-5), false, 1).target_ns == 0);
}

TEST_CASE("the seek carries the generation that discards in-flight frames", "[transport]") {
  REQUIRE(make_seek(sec(10), false, 42).generation == 42);
}

TEST_CASE("a scrub that reports the origin is not a seek", "[transport]") {
  // Issue #362. The slider's minimum, sought nearest-keyframe, presents the
  // opening frame on the way to the time under the pointer.
  REQUIRE(scrub_origin_is_spurious(sec(30), /*target=*/0, /*exact=*/false));
  REQUIRE(scrub_origin_is_spurious(sec(30), /*target=*/-1, /*exact=*/false));
  REQUIRE_FALSE(scrub_origin_is_spurious(sec(30), sec(28), false));
  // Release, a step and a typed position at the start are that time.
  REQUIRE_FALSE(scrub_origin_is_spurious(sec(30), 0, true));
  // Still at the start: the minimum is where the playhead already is.
  REQUIRE_FALSE(scrub_origin_is_spurious(sec(0.2), 0, false));
  REQUIRE_FALSE(scrub_origin_is_spurious(0, 0, false));
  REQUIRE_FALSE(scrub_origin_is_spurious(scrub_origin_playhead_ns, 0, false));
  REQUIRE(scrub_origin_is_spurious(scrub_origin_playhead_ns + 1, 0, false));
}

// --- speed ------------------------------------------------------------------

TEST_CASE("rate 1.0 produces no filter at all", "[transport]") {
  // Not "atempo=1.000": a no-op filter still costs a resample pass per block.
  REQUIRE(build_atempo_chain(1.0).empty());
}

TEST_CASE("rates inside atempo's range use a single instance", "[transport]") {
  REQUIRE(build_atempo_chain(1.5) == "atempo=1.500");
  REQUIRE(build_atempo_chain(0.5) == "atempo=0.500");
  REQUIRE(build_atempo_chain(2.0) == "atempo=2.000");
}

TEST_CASE("the extremes are chained, because atempo caps at 0.5-2.0", "[transport]") {
  // docs/design/05 names this case explicitly. One instance cannot do 0.25x or 4x.
  REQUIRE(build_atempo_chain(0.25) == "atempo=0.500,atempo=0.500");
  REQUIRE(build_atempo_chain(4.0) == "atempo=2.000,atempo=2.000");
}

TEST_CASE("an uneven ratio splits rather than assuming a fixed chain", "[transport]") {
  REQUIRE(build_atempo_chain(3.0) == "atempo=2.000,atempo=1.500");
}

TEST_CASE("every chain multiplies back to the requested rate", "[transport]") {
  // The property that actually matters: whatever the split, the product is the
  // speed the user asked for.
  for (double rate : {0.25, 0.3, 0.5, 0.75, 1.25, 1.5, 2.0, 2.5, 3.0, 3.7, 4.0}) {
    const std::string chain = build_atempo_chain(rate);
    REQUIRE_FALSE(chain.empty());

    double product = 1.0;
    std::size_t pos = 0;
    while ((pos = chain.find("atempo=", pos)) != std::string::npos) {
      pos += 7;
      product *= std::stod(chain.substr(pos));
    }
    REQUIRE(product > rate - 0.005);
    REQUIRE(product < rate + 0.005);
  }
}

TEST_CASE("every atempo instance in a chain is within FFmpeg's legal range", "[transport]") {
  // A chain that multiplies correctly but contains an out-of-range instance
  // fails at filter-graph construction, at runtime, on the user's machine.
  for (double rate : {0.25, 0.26, 0.49, 0.51, 1.99, 2.01, 3.99, 4.0}) {
    const std::string chain = build_atempo_chain(rate);
    std::size_t pos = 0;
    while ((pos = chain.find("atempo=", pos)) != std::string::npos) {
      pos += 7;
      const double factor = std::stod(chain.substr(pos));
      REQUIRE(factor >= 0.5);
      REQUIRE(factor <= 2.0);
    }
  }
}

TEST_CASE("rates outside 0.25-4.0 are clamped, and NaN falls back to 1.0", "[transport]") {
  REQUIRE(clamp_rate(0.01) == min_rate);
  REQUIRE(clamp_rate(99.0) == max_rate);
  REQUIRE(clamp_rate(0.0) == 1.0);
  REQUIRE(clamp_rate(-2.0) == 1.0);
  REQUIRE(clamp_rate(std::nan("")) == 1.0);
}

// --- A-B loop ---------------------------------------------------------------

TEST_CASE("no loop is set means no wrap", "[transport]") {
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(ab_loop{}, sec(100), &to));
}

TEST_CASE("playback wraps to A once it passes B", "[transport]") {
  const ab_loop loop{sec(10), sec(20)};
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(loop, sec(15), &to));
  REQUIRE(loop_wrap(loop, sec(20), &to));
  REQUIRE(to == sec(10));
}

TEST_CASE("marking B before A still loops", "[transport]") {
  // The user's intent is unambiguous; silently doing nothing is the wrong
  // answer to a reversed pair.
  const ab_loop reversed{sec(20), sec(10)};
  time_ns to = -1;
  REQUIRE(loop_wrap(reversed, sec(25), &to));
  REQUIRE(to == sec(10));
}

TEST_CASE("a zero-length loop does not wrap", "[transport]") {
  time_ns to = -1;
  REQUIRE_FALSE(loop_wrap(ab_loop{sec(10), sec(10)}, sec(30), &to));
}

// --- frame step -------------------------------------------------------------

TEST_CASE("stepping forward lands inside the next frame", "[transport]") {
  const double fps = 25.0;
  const time_ns frame = sec(1) / 25;
  const time_ns target = step_target(sec(10), +1, fps);
  REQUIRE(target > sec(10) + frame / 2);
  REQUIRE(target < sec(10) + frame + frame);
}

TEST_CASE("stepping backward lands inside the previous frame", "[transport]") {
  const double fps = 25.0;
  const time_ns frame = sec(1) / 25;
  const time_ns target = step_target(sec(10), -1, fps);
  REQUIRE(target < sec(10) - frame / 2);
  REQUIRE(target > sec(10) - frame - frame);
}

TEST_CASE("repeated steps advance rather than sticking on one frame", "[transport]") {
  // The half-frame bias exists for this: without it a position sitting exactly
  // on a boundary steps onto the same frame twice.
  const double fps = 30.0;
  time_ns pos = 0;
  time_ns previous = -1;
  for (int i = 0; i < 20; ++i) {
    pos = step_target(pos, +1, fps);
    REQUIRE(pos > previous);
    previous = pos;
  }
}

TEST_CASE("stepping back at the start clamps to zero rather than going negative",
          "[transport]") {
  REQUIRE(step_target(0, -1, 25.0) == 0);
}

TEST_CASE("a clip with no usable frame rate can still be stepped", "[transport]") {
  REQUIRE(step_target(sec(10), +1, 0.0) > sec(10));
  REQUIRE(step_target(sec(10), +1, -5.0) > sec(10));
}

TEST_CASE("a step of zero frames is a no-op", "[transport]") {
  REQUIRE(step_target(sec(10), 0, 25.0) == sec(10));
}

// --- resume -----------------------------------------------------------------

TEST_CASE("the first few seconds are not worth resuming", "[transport]") {
  REQUIRE_FALSE(should_store_resume(sec(5), sec(600)));
}

TEST_CASE("a position mid-clip is stored", "[transport]") {
  REQUIRE(should_store_resume(sec(300), sec(600)));
}

TEST_CASE("watching to the end does not store a resume on the credits", "[transport]") {
  // Reopening two seconds before the end is worse than starting over.
  REQUIRE_FALSE(should_store_resume(sec(598), sec(600)));
}

TEST_CASE("a stored position near the end restarts from the beginning", "[transport]") {
  REQUIRE(resume_start_position(sec(595), sec(600)) == 0);
  REQUIRE(resume_start_position(sec(300), sec(600)) == sec(300));
}

TEST_CASE("an unknown duration never stores or restores a resume", "[transport]") {
  REQUIRE_FALSE(should_store_resume(sec(300), 0));
  REQUIRE(resume_start_position(sec(300), 0) == 0);
}

// --- resume store (#235) ----------------------------------------------------

namespace {
struct temp_dir {
  std::filesystem::path path;
  temp_dir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() / ("mv-resume-" + std::to_string(rd()));
  }
  ~temp_dir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  temp_dir(const temp_dir&) = delete;
  temp_dir& operator=(const temp_dir&) = delete;
  std::size_t count() const {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path),
                                                  std::filesystem::directory_iterator()));
  }
};
}  // namespace

TEST_CASE("a resume point round-trips for the same file version", "[transport]") {
  temp_dir dir;
  const resume_key key{.path = "/clips/a.mp4", .size = 1234, .mtime = 99};
  resume_save(dir.path, key, sec(300), sec(600));
  REQUIRE(resume_load(dir.path, key, sec(600)) == sec(300));
  // Another size or mtime is another version of the file; another path another file.
  REQUIRE(resume_load(dir.path, {.path = key.path, .size = 1235, .mtime = 99}, sec(600)) == 0);
  REQUIRE(resume_load(dir.path, {.path = key.path, .size = 1234, .mtime = 100}, sec(600)) == 0);
  REQUIRE(resume_load(dir.path, {.path = "/clips/b.mp4", .size = 1234, .mtime = 99}, sec(600)) == 0);
}

TEST_CASE("a position not worth resuming writes nothing", "[transport]") {
  temp_dir dir;
  // The start, and an unknown duration (DRM, some audio): no entry, not a stored 0.
  resume_save(dir.path, {.path = "/clips/a.mp4", .size = 1, .mtime = 1}, sec(3), sec(600));
  resume_save(dir.path, {.path = "/clips/b.m4p", .size = 1, .mtime = 1}, sec(300), 0);
  REQUIRE(dir.count() == 0);
}

TEST_CASE("closing a clip near its start drops its stale resume entry", "[transport]") {
  temp_dir dir;
  const resume_key key{.path = "/clips/a.mp4", .size = 1, .mtime = 1};
  resume_save(dir.path, key, sec(300), sec(600));
  REQUIRE(dir.count() == 1);
  resume_save(dir.path, key, sec(2), sec(600));
  REQUIRE(dir.count() == 0);
  REQUIRE(resume_load(dir.path, key, sec(600)) == 0);
}

TEST_CASE("the resume store holds no path in plain text", "[transport]") {
  temp_dir dir;
  const std::string path = "/Users/someone/Holiday/IMG_0001.MOV";
  resume_save(dir.path, {.path = path, .size = 1, .mtime = 1}, sec(300), sec(600));
  REQUIRE(dir.count() == 1);
  for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
    std::ifstream in(entry.path());
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(body.find("IMG_0001") == std::string::npos);
    REQUIRE(body.find("Holiday") == std::string::npos);
    REQUIRE(entry.path().filename().string().find("IMG_0001") == std::string::npos);
  }
}

TEST_CASE("pruning keeps the newest entries and removes the old path-bearing format",
          "[transport]") {
  temp_dir dir;
  for (int i = 0; i < 12; ++i) {
    resume_save(dir.path, {.path = "/clips/" + std::to_string(i), .size = 1, .mtime = 1},
                sec(300), sec(600));
  }
  {
    std::ofstream legacy(dir.path / "123.txt");  // pre-#235: the path in plain text
    legacy << "/Users/someone/clip.mp4\n1 1 300000000000\n";
  }
  // Give each entry a distinct age so "newest" does not hang on mtime resolution.
  std::vector<std::filesystem::path> pos;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
    if (entry.path().extension() == ".pos") pos.push_back(entry.path());
  }
  REQUIRE(pos.size() == 12);
  std::sort(pos.begin(), pos.end());
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::size_t i = 0; i < pos.size(); ++i) {
    std::filesystem::last_write_time(pos[i], now - std::chrono::hours(100 - static_cast<int>(i)));
  }
  resume_prune(dir.path, 5);
  REQUIRE(dir.count() == 5);
  REQUIRE_FALSE(std::filesystem::exists(dir.path / "123.txt"));
  for (std::size_t i = pos.size() - 5; i < pos.size(); ++i) {
    REQUIRE(std::filesystem::exists(pos[i]));
  }
}
