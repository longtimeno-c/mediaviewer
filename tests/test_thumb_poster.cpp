// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #610: Explorer and Finder thumbnails for clips (shellext/thumb_poster).
// The COM and Quick Look wrappers are host code; everything they decide about
// a clip is decided here: the same poster as the filmstrip, read through a
// seekable reader rather than into memory, under the still path's deadline and
// in-flight cap. Synthetic clips (tests/clip_fixture.h), no corpus.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "clip_fixture.h"
#include "import_fixture.h"
#include "player/poster.h"
#include "shellext/thumb_poster.h"
#include "shellext/thumb_request.h"

using namespace mv::test;
namespace fx = mv::test::clipfx;

namespace {

std::vector<std::uint8_t> clip_bytes(const scratch_dir& d, const char* name, const char* muxer,
                                     int frames = 30, int rotation = 0) {
  const std::string path = utf8(d / name);
  fx::spec s;
  s.muxer = muxer;
  s.audio = false;
  s.frames = frames;
  s.rotation = rotation;
  REQUIRE(fx::make(path, s));
  return read_bytes(d / name);
}

// Memory standing in for a file far larger than it: the clip's bytes, then
// zeros up to `size`. Counts what is read, so a test can tell the poster
// read the head of the file and not the whole of it.
class sparse_source final : public mv::player::byte_source {
 public:
  sparse_source(std::vector<std::uint8_t> head, std::uint64_t size)
      : head_(std::move(head)), size_(size) {}
  mv::result<std::size_t> read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept override {
    if (offset >= size_) return std::size_t{0};
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size_ - offset));
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t at = offset + i;
      out[i] = at < head_.size() ? head_[static_cast<std::size_t>(at)] : 0;
    }
    read_ += n;
    return n;
  }
  std::uint64_t size() const noexcept override { return size_; }
  std::uint64_t bytes_read() const noexcept { return read_; }

 private:
  std::vector<std::uint8_t> head_;
  std::uint64_t size_;
  std::uint64_t read_ = 0;
};

// A source that fails every read: a share that went away.
class failing_source final : public mv::player::byte_source {
 public:
  mv::result<std::size_t> read_at(std::uint64_t, std::span<std::uint8_t>) noexcept override {
    return mv::err(mv::status::io);
  }
  std::uint64_t size() const noexcept override { return 1u << 20; }
};

void wait_for_idle(std::chrono::seconds limit) {
  for (auto end = std::chrono::steady_clock::now() + limit;
       mv::shellext::decodes_in_flight() != 0 && std::chrono::steady_clock::now() < end;)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

}  // namespace

TEST_CASE("an Explorer clip thumbnail is the filmstrip's poster, as BGRA", "[shellext][poster]") {
  scratch_dir d("thumb_poster_match");
  for (const char* muxer : {"matroska", "avi", "mpegts", "mp4"}) {
    INFO(muxer);
    const std::string name = std::string("clip.") + muxer;
    const auto bytes = clip_bytes(d, name.c_str(), muxer);
    CHECK(mv::shellext::classify(bytes) == mv::shellext::source_kind::clip);

    mv::shellext::span_source src(bytes);
    auto t = mv::shellext::render_poster(src, 256);
    REQUIRE(t);
    CHECK(t->width == 256);
    CHECK(t->height == 192);
    CHECK_FALSE(t->has_alpha);
    REQUIRE(t->bgra.size() == 256u * 192u * 4u);

    // Same frame, same scale as the app's own poster from the path.
    auto app = mv::player::poster_frame(utf8(d / name).c_str(), 256, nullptr);
    REQUIRE(app);
    REQUIRE(app->rgba.size() == t->bgra.size());
    bool same = true;
    for (std::size_t i = 0; i < app->rgba.size() && same; i += 4) {
      same = t->bgra[i] == app->rgba[i + 2] && t->bgra[i + 1] == app->rgba[i + 1] &&
             t->bgra[i + 2] == app->rgba[i] && t->bgra[i + 3] == 255;
    }
    CHECK(same);
  }
}

TEST_CASE("a clip's display matrix turns its Explorer thumbnail", "[shellext][poster]") {
  scratch_dir d("thumb_poster_turned");
  const auto bytes = clip_bytes(d, "portrait.mp4", "mp4", 30, 90);
  mv::shellext::span_source src(bytes);
  auto t = mv::shellext::render_poster(src, 512);
  REQUIRE(t);
  CHECK(t->width == 240);  // coded 320x240, shown 240x320
  CHECK(t->height == 320);
}

TEST_CASE("a clip thumbnail's size is clamped, never enlarged", "[shellext][poster]") {
  scratch_dir d("thumb_poster_clamp");
  const auto bytes = clip_bytes(d, "clip.mkv", "matroska");
  mv::shellext::span_source src(bytes);
  auto big = mv::shellext::render_poster(src, 100000);
  REQUIRE(big);
  CHECK(big->width == 320);  // the clip is smaller than kMaxThumbEdge
  auto tiny = mv::shellext::render_poster(src, 1);
  REQUIRE(tiny);
  CHECK(tiny->width == 16);
  CHECK(tiny->height == 12);
}

TEST_CASE("a still is classified as a still, a clip as a clip", "[shellext][poster]") {
  const std::uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
  CHECK(mv::shellext::classify(png) == mv::shellext::source_kind::still);
  const std::uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0};
  CHECK(mv::shellext::classify(jpeg) == mv::shellext::source_kind::still);
  const std::uint8_t ebml[] = {0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01};
  CHECK(mv::shellext::classify(ebml) == mv::shellext::source_kind::clip);
}

TEST_CASE("a truncated or junk clip is no thumbnail, not a crash", "[shellext][poster]") {
  scratch_dir d("thumb_poster_broken");
  const auto bytes = clip_bytes(d, "clip.mkv", "matroska", 60);
  // The EBML header and nothing behind it.
  {
    const std::vector<std::uint8_t> head(bytes.begin(), bytes.begin() + 64);
    mv::shellext::span_source src(head);
    CHECK_FALSE(mv::shellext::render_poster(src, 256));
  }
  // Cut mid-cluster at every eighth: a thumbnail or none, never a crash, and
  // the answer comes back well inside the deadline.
  for (std::size_t cut = bytes.size() / 8; cut < bytes.size(); cut += bytes.size() / 8) {
    const std::vector<std::uint8_t> part(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
    auto r = mv::shellext::render_poster_by(std::make_unique<mv::shellext::span_source>(
                                                std::span<const std::uint8_t>(part)),
                                            256, mv::shellext::kDeadline);
    if (r) {
      CHECK(r->width == 256);
    } else {
      CHECK(r.error() != mv::status::cancelled);
    }
  }
  const std::vector<std::uint8_t> junk(64 * 1024, 0xA5);
  mv::shellext::span_source junk_src(junk);
  CHECK_FALSE(mv::shellext::render_poster(junk_src, 256));
  failing_source gone;
  CHECK_FALSE(mv::shellext::render_poster(gone, 256));
  const std::vector<std::uint8_t> empty;
  mv::shellext::span_source empty_src(empty);
  CHECK_FALSE(mv::shellext::render_poster(empty_src, 256));
  CHECK_FALSE(mv::shellext::render_poster_by(nullptr, 256));
}

TEST_CASE("a 4 GB clip is read at its head, not whole", "[shellext][poster]") {
  scratch_dir d("thumb_poster_big");
  const auto bytes = clip_bytes(d, "clip.mkv", "matroska");
  // The clip, then zeros to 4 GiB: what a long recording looks like to a
  // reader that never gets past its first clusters.
  sparse_source src(bytes, 4ull << 30);
  const auto start = std::chrono::steady_clock::now();
  auto t = mv::shellext::render_poster(src, 256);
  const auto took = std::chrono::steady_clock::now() - start;
  REQUIRE(t);
  CHECK(t->width == 256);
  CHECK(src.bytes_read() < 16u * 1024u * 1024u);
  CHECK(took < mv::shellext::kDeadline);
}

TEST_CASE("a cancelled clip poster stops", "[shellext][poster]") {
  scratch_dir d("thumb_poster_cancel");
  const auto bytes = clip_bytes(d, "clip.mkv", "matroska");
  std::atomic<mv::generation> current{2};
  const mv::job_context ctx(0, 1, &current, 0);  // already moved on
  mv::shellext::span_source src(bytes);
  auto r = mv::shellext::render_poster(src, 256, &ctx);
  REQUIRE_FALSE(r);
  CHECK(r.error() == mv::status::cancelled);
}

TEST_CASE("the clip deadline path answers, abandons, and shares the cap", "[shellext][poster]") {
  scratch_dir d("thumb_poster_deadline");
  const auto bytes = clip_bytes(d, "clip.mkv", "matroska");
  auto ok = mv::shellext::render_poster_by(
      std::make_unique<mv::shellext::span_source>(std::span<const std::uint8_t>(bytes)), 64,
      std::chrono::seconds(10));
  REQUIRE(ok);
  CHECK(ok->width == 64);
  // No time at all: "no thumbnail", and the decode ends on its own thread.
  // The source must outlive it: the thread owns the reader, the reader only
  // borrows `bytes`, which this test keeps until the slots are free again.
  for (std::uint32_t i = 0; i < mv::shellext::kMaxInFlight + 2; ++i) {
    auto late = mv::shellext::render_poster_by(
        std::make_unique<mv::shellext::span_source>(std::span<const std::uint8_t>(bytes)), 256,
        std::chrono::milliseconds(0));
    REQUIRE_FALSE(late);
    CHECK((late.error() == mv::status::cancelled || late.error() == mv::status::busy));
  }
  CHECK(mv::shellext::decodes_in_flight() <= mv::shellext::kMaxInFlight);
  wait_for_idle(std::chrono::seconds(60));
  CHECK(mv::shellext::decodes_in_flight() == 0);
}

TEST_CASE("a clip on disk posterises through the file reader", "[shellext][poster]") {
  scratch_dir d("thumb_poster_file");
  (void)clip_bytes(d, "clip.avi", "avi");
  auto src = mv::shellext::open_file_source(utf8(d / "clip.avi").c_str());
  REQUIRE(src);
  CHECK(src->size() > 0);
  auto t = mv::shellext::render_poster_by(std::move(src), 128, std::chrono::seconds(10));
  REQUIRE(t);
  CHECK(t->width == 128);
  CHECK(t->height == 96);
  CHECK_FALSE(mv::shellext::open_file_source(utf8(d / "missing.mkv").c_str()));
  write_bytes(d / "empty.mkv", {});
  CHECK_FALSE(mv::shellext::open_file_source(utf8(d / "empty.mkv").c_str()));
  wait_for_idle(std::chrono::seconds(30));
}

// fuzz_thumbnail's video seeds (tests/data/seeds/video): small clips of the
// containers the handler is for, written by the fixture's LGPL encoders.
// Hidden: `MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"`.
TEST_CASE("write the clip thumbnail fuzz seeds", "[.write-seeds]") {
  const char* dir = std::getenv("MV_SEED_DIR");
  if (!dir) SKIP("set MV_SEED_DIR");
  const std::filesystem::path out = std::filesystem::path(dir) / "video";
  std::filesystem::create_directories(out);
  const struct {
    const char* file;
    const char* muxer;
    int rotation;
  } seeds[] = {{"tiny.mkv", "matroska", 0}, {"tiny.avi", "avi", 0}, {"tiny.ts", "mpegts", 0},
               {"turned.mp4", "mp4", 90}};
  for (const auto& s : seeds) {
    fx::spec spec;
    spec.muxer = s.muxer;
    spec.width = 32;
    spec.height = 24;
    spec.frames = 4;
    spec.gop = 2;
    spec.audio = false;
    spec.rotation = s.rotation;
    REQUIRE(fx::make(utf8(out / s.file), spec));
  }
}
