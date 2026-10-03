// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io/verified_copy.h: docs/design/18 "Verify" and the base app's F8 across volumes.
#include "catch_compat.h"

#include <atomic>
#include <string>
#include <vector>

#include "import_fixture.h"
#include "io/file_port.h"
#include "io/in_flight.h"
#include "io/verified_copy.h"
#include "io/volume.h"

using namespace mv::test;
using mv::io::copy_target_outcome;

namespace {

bool no_temps_under(const fs::path& dir) {
  for (const std::string& rel : list_tree(dir)) {
    if (rel.find(".mvtmp") != std::string::npos) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("a verified copy lands whole, hashed, with the source's mtime", "[io][verify]") {
  scratch_dir s("vcopy");
  const auto bytes = pattern(9 * 1024 * 1024 + 17, 1);  // several buffers, a ragged tail
  write_bytes(s / "card/IMG_0001.JPG", bytes);
  set_mtime(s / "card/IMG_0001.JPG", 1790000000);
  fs::create_directories(s / "dest");

  mv::io::copy_options o;
  std::uint64_t progressed = 0;
  o.on_progress = [&](std::uint64_t d) { progressed += d; };
  const std::vector<std::string> targets{utf8(s / "dest/IMG_0001.JPG")};
  auto r = mv::io::verified_copy(utf8(s / "card/IMG_0001.JPG"), targets, o);
  REQUIRE(r);
  REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
  REQUIRE_FALSE(r->targets[0].retried);
  REQUIRE(r->bytes == bytes.size());
  REQUIRE(progressed == bytes.size());
  REQUIRE(read_bytes(s / "dest/IMG_0001.JPG") == bytes);
  REQUIRE(r->source_hash == mv::io::hash_bytes(bytes));
  auto st = mv::io::stat_path(targets[0]);
  REQUIRE(st);
  REQUIRE(st->mtime_unix == 1790000000);
  REQUIRE(no_temps_under(s.root()));
}

TEST_CASE("one read writes two verified destinations", "[io][verify]") {
  scratch_dir s("vcopy2");
  const auto bytes = pattern(5 * 1024 * 1024, 2);
  write_bytes(s / "card/a.CR3", bytes);
  fs::create_directories(s / "main");
  fs::create_directories(s / "backup");
  const std::vector<std::string> targets{utf8(s / "main/a.CR3"), utf8(s / "backup/a.CR3")};
  std::uint64_t read = 0;
  mv::io::copy_options o;
  o.on_progress = [&](std::uint64_t d) { read += d; };
  auto r = mv::io::verified_copy(utf8(s / "card/a.CR3"), targets, o);
  REQUIRE(r);
  REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
  REQUIRE(r->targets[1].outcome == copy_target_outcome::verified);
  REQUIRE(read == bytes.size());  // the card was read once
  REQUIRE(read_bytes(s / "main/a.CR3") == bytes);
  REQUIRE(read_bytes(s / "backup/a.CR3") == bytes);
}

TEST_CASE("a taken final name is never overwritten", "[io][verify]") {
  scratch_dir s("vcopy3");
  write_text(s / "card/x.jpg", "new bytes");
  write_text(s / "dest/x.jpg", "someone else's");
  const std::vector<std::string> targets{utf8(s / "dest/x.jpg")};
  auto r = mv::io::verified_copy(utf8(s / "card/x.jpg"), targets, {});
  REQUIRE(r);
  REQUIRE(r->targets[0].outcome == copy_target_outcome::name_taken);
  std::vector<std::uint8_t> want{'s', 'o', 'm', 'e', 'o', 'n', 'e', ' ', 'e', 'l', 's', 'e', '\'',
                                 's'};
  REQUIRE(read_bytes(s / "dest/x.jpg") == want);
}

TEST_CASE("a corrupted write is caught by the read-back, retried once, then failed",
          "[io][verify]") {
  scratch_dir s("vcopy4");
  const auto bytes = pattern(6 * 1024 * 1024, 3);
  write_bytes(s / "card/b.jpg", bytes);
  fs::create_directories(s / "dest");
  const std::vector<std::string> targets{utf8(s / "dest/b.jpg")};

  SECTION("one bad write: the retry lands a good copy and says it retried") {
    mv::io::copy_fault fault{0, 4 * 1024 * 1024 + 5, 1};
    mv::io::copy_options o;
    o.fault = &fault;
    auto r = mv::io::verified_copy(utf8(s / "card/b.jpg"), targets, o);
    REQUIRE(r);
    REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
    REQUIRE(r->targets[0].retried);
    REQUIRE(read_bytes(s / "dest/b.jpg") == bytes);
  }
  SECTION("two bad writes: reported, and no file and no temporary left") {
    mv::io::copy_fault fault{0, 12, 2};
    mv::io::copy_options o;
    o.fault = &fault;
    auto r = mv::io::verified_copy(utf8(s / "card/b.jpg"), targets, o);
    REQUIRE(r);
    REQUIRE(r->targets[0].outcome == copy_target_outcome::verify_failed);
    REQUIRE_FALSE(fs::exists(s / "dest/b.jpg"));
    REQUIRE(no_temps_under(s.root()));
  }
  // The source is never touched.
  REQUIRE(read_bytes(s / "card/b.jpg") == bytes);
}

TEST_CASE("cancel leaves no temporary behind", "[io][verify]") {
  scratch_dir s("vcopy5");
  write_bytes(s / "card/c.mov", pattern(12 * 1024 * 1024, 4));
  fs::create_directories(s / "dest");
  std::atomic<bool> cancel{false};
  mv::io::copy_options o;
  o.cancel = &cancel;
  o.on_progress = [&](std::uint64_t) { cancel = true; };
  const std::vector<std::string> targets{utf8(s / "dest/c.mov")};
  auto r = mv::io::verified_copy(utf8(s / "card/c.mov"), targets, o);
  REQUIRE_FALSE(r);
  REQUIRE(r.error() == mv::status::cancelled);
  REQUIRE(list_tree(s / "dest").empty());
}

TEST_CASE("empty files copy and verify; a missing source is an error", "[io][verify]") {
  scratch_dir s("vcopy6");
  write_text(s / "card/empty.xmp", "");
  fs::create_directories(s / "dest");
  const std::vector<std::string> targets{utf8(s / "dest/empty.xmp")};
  auto r = mv::io::verified_copy(utf8(s / "card/empty.xmp"), targets, {});
  REQUIRE(r);
  REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
  REQUIRE(fs::file_size(s / "dest/empty.xmp") == 0);
  const std::vector<std::string> fresh{utf8(s / "dest/nope.jpg")};
  REQUIRE_FALSE(mv::io::verified_copy(utf8(s / "card/nope.jpg"), fresh, {}));
}

TEST_CASE("the file port refuses to replace and walks in a stable order", "[io][port]") {
  scratch_dir s("port");
  write_text(s / "a.txt", "a");
  write_text(s / "b.txt", "b");
  auto taken = mv::io::rename_no_replace(utf8(s / "a.txt"), utf8(s / "b.txt"));
  REQUIRE(taken);
  REQUIRE(*taken == mv::io::rename_outcome::name_taken);
  REQUIRE(read_bytes(s / "b.txt") == std::vector<std::uint8_t>{'b'});

  write_text(s / "tree/DCIM/100CANON/IMG_0002.JPG", "2");
  write_text(s / "tree/DCIM/100CANON/IMG_0001.JPG", "1");
  write_text(s / "tree/.hidden/x.jpg", "h");
  write_text(s / "tree/.DS_Store", "h");
  std::vector<std::string> seen;
  REQUIRE(mv::io::walk_files(utf8(s / "tree"), 8, [&](const mv::io::tree_entry& e) {
    seen.push_back(e.relative_utf8);
    return true;
  }));
  REQUIRE(seen == std::vector<std::string>{"DCIM/100CANON/IMG_0001.JPG", "DCIM/100CANON/IMG_0002.JPG"});

  REQUIRE(mv::io::remove_tree(utf8(s / "tree")));
  REQUIRE_FALSE(fs::exists(s / "tree"));
}

// The deep path (copy_options::read_depth / write_depth > 1): what a network
// share gets. Several reads and writes in flight at their offsets, hashed in
// order; it must land the same bytes and keep every rule the sequential path
// keeps.
namespace {

mv::io::copy_options deep_options(unsigned depth, std::size_t chunk) {
  mv::io::copy_options o;
  o.read_depth = depth;
  o.write_depth = depth;
  o.buffer_bytes = chunk;
  return o;
}

}  // namespace

TEST_CASE("the deep path lands the same bytes, hash and mtime", "[io][verify][deep]") {
  scratch_dir s("vdeep");
  // Sizes around the chunk edges: empty, one byte, one chunk exactly, a
  // ragged tail, and many chunks for a deep queue.
  const std::size_t chunk = 64 * 1024;
  const std::size_t sizes[] = {0, 1, chunk - 1, chunk, chunk + 1, 37 * chunk + 4095,
                               9 * 1024 * 1024 + 17};
  fs::create_directories(s / "dest");
  for (const unsigned depth : {2u, 8u, 16u}) {
    for (std::size_t i = 0; i < std::size(sizes); ++i) {
      const auto bytes = pattern(sizes[i], static_cast<std::uint32_t>(i + depth));
      const std::string name = "f" + std::to_string(depth) + "_" + std::to_string(i) + ".bin";
      write_bytes(s / "src" / name, bytes);
      set_mtime(s / "src" / name, 1790000000);
      auto o = deep_options(depth, chunk);
      std::uint64_t progressed = 0;
      o.on_progress = [&](std::uint64_t d) { progressed += d; };
      const std::vector<std::string> targets{utf8(s / "dest" / name)};
      auto r = mv::io::verified_copy(utf8(s / "src" / name), targets, o);
      REQUIRE(r);
      REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
      REQUIRE(r->bytes == bytes.size());
      REQUIRE(progressed == bytes.size());
      REQUIRE(r->source_hash == mv::io::hash_bytes(bytes));
      REQUIRE(read_bytes(s / "dest" / name) == bytes);  // exact size: nothing left past the end
      auto st = mv::io::stat_path(targets[0]);
      REQUIRE(st);
      REQUIRE(st->mtime_unix == 1790000000);
    }
  }
  REQUIRE(no_temps_under(s.root()));
}

TEST_CASE("the deep path writes two destinations from one read", "[io][verify][deep]") {
  scratch_dir s("vdeep2");
  const auto bytes = pattern(3 * 1024 * 1024 + 99, 7);
  write_bytes(s / "src/a.CR3", bytes);
  fs::create_directories(s / "main");
  fs::create_directories(s / "backup");
  const std::vector<std::string> targets{utf8(s / "main/a.CR3"), utf8(s / "backup/a.CR3")};
  auto o = deep_options(8, 128 * 1024);
  std::uint64_t read = 0;
  o.on_progress = [&](std::uint64_t d) { read += d; };
  auto r = mv::io::verified_copy(utf8(s / "src/a.CR3"), targets, o);
  REQUIRE(r);
  REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
  REQUIRE(r->targets[1].outcome == copy_target_outcome::verified);
  REQUIRE(read == bytes.size());
  REQUIRE(read_bytes(s / "main/a.CR3") == bytes);
  REQUIRE(read_bytes(s / "backup/a.CR3") == bytes);
}

TEST_CASE("the deep path catches a bad write, retries, and fails a second one",
          "[io][verify][deep]") {
  scratch_dir s("vdeep3");
  const auto bytes = pattern(2 * 1024 * 1024 + 3, 8);
  write_bytes(s / "src/b.jpg", bytes);
  fs::create_directories(s / "dest");
  const std::vector<std::string> targets{utf8(s / "dest/b.jpg")};

  SECTION("one bad write: retried, verified") {
    mv::io::copy_fault fault{0, 1024 * 1024 + 5, 1};
    auto o = deep_options(8, 64 * 1024);
    o.fault = &fault;
    auto r = mv::io::verified_copy(utf8(s / "src/b.jpg"), targets, o);
    REQUIRE(r);
    REQUIRE(r->targets[0].outcome == copy_target_outcome::verified);
    REQUIRE(r->targets[0].retried);
    REQUIRE(read_bytes(s / "dest/b.jpg") == bytes);
  }
  SECTION("two bad writes: reported, nothing left") {
    mv::io::copy_fault fault{0, 12, 2};
    auto o = deep_options(8, 64 * 1024);
    o.fault = &fault;
    auto r = mv::io::verified_copy(utf8(s / "src/b.jpg"), targets, o);
    REQUIRE(r);
    REQUIRE(r->targets[0].outcome == copy_target_outcome::verify_failed);
    REQUIRE_FALSE(fs::exists(s / "dest/b.jpg"));
    REQUIRE(no_temps_under(s.root()));
  }
}

TEST_CASE("the deep path cancels cleanly with requests in flight", "[io][verify][deep]") {
  scratch_dir s("vdeep4");
  write_bytes(s / "src/c.mov", pattern(16 * 1024 * 1024, 9));
  fs::create_directories(s / "dest");
  std::atomic<bool> cancel{false};
  auto o = deep_options(16, 64 * 1024);
  o.cancel = &cancel;
  std::uint64_t seen = 0;
  o.on_progress = [&](std::uint64_t d) {
    seen += d;
    if (seen > 1024 * 1024) cancel = true;
  };
  const std::vector<std::string> targets{utf8(s / "dest/c.mov")};
  auto r = mv::io::verified_copy(utf8(s / "src/c.mov"), targets, o);
  REQUIRE_FALSE(r);
  REQUIRE(r.error() == mv::status::cancelled);
  REQUIRE(list_tree(s / "dest").empty());
}

TEST_CASE("a deep uncached hash matches the sequential one", "[io][verify][deep]") {
  scratch_dir s("vdeep5");
  const auto bytes = pattern(5 * 1024 * 1024 + 4097, 10);
  write_bytes(s / "f.bin", bytes);
  for (const auto mode : {mv::io::read_mode::sequential, mv::io::read_mode::uncached}) {
    auto deep = mv::io::hash_file(utf8(s / "f.bin"), mode, nullptr, {}, {}, 8, 256 * 1024);
    REQUIRE(deep);
    REQUIRE(*deep == mv::io::hash_bytes(bytes));
  }
}

TEST_CASE("several files at once: each copies once, a name clash keeps both",
          "[io][verify][deep]") {
  scratch_dir s("vflight");
  constexpr std::size_t kFiles = 40;
  fs::create_directories(s / "dest");
  std::vector<std::vector<std::uint8_t>> bytes(kFiles);
  for (std::size_t i = 0; i < kFiles; ++i) {
    bytes[i] = pattern(200 * 1024 + i * 1000, static_cast<std::uint32_t>(i));
    write_bytes(s / "src" / ("f" + std::to_string(i) + ".bin"), bytes[i]);
  }
  std::vector<int> ran(kFiles, 0);
  std::atomic<int> verified{0};
  mv::io::for_each_in_flight(kFiles, 4, [&](std::size_t i) {
    ++ran[i];
    auto o = deep_options(4, 64 * 1024);
    const std::string name = "f" + std::to_string(i) + ".bin";
    const std::vector<std::string> targets{utf8(s / "dest" / name)};
    auto r = mv::io::verified_copy(utf8(s / "src" / name), targets, o);
    if (r && r->targets[0].outcome == copy_target_outcome::verified) ++verified;
  });
  REQUIRE(verified == static_cast<int>(kFiles));
  for (std::size_t i = 0; i < kFiles; ++i) {
    REQUIRE(ran[i] == 1);
    REQUIRE(read_bytes(s / "dest" / ("f" + std::to_string(i) + ".bin")) == bytes[i]);
  }

  // Two files racing for one final name: exactly one lands there; the other
  // is told the name is taken, and nothing is overwritten.
  write_bytes(s / "src/a/same.bin", pattern(512 * 1024, 50));
  write_bytes(s / "src/b/same.bin", pattern(512 * 1024, 51));
  const std::string dirs[] = {"a", "b"};
  std::atomic<int> landed{0};
  std::atomic<int> taken{0};
  mv::io::for_each_in_flight(2, 2, [&](std::size_t i) {
    const std::vector<std::string> targets{utf8(s / "dest/same.bin")};
    auto r = mv::io::verified_copy(utf8(s / "src" / dirs[i] / "same.bin"), targets,
                                   deep_options(4, 64 * 1024));
    // No REQUIRE off the test's thread: count, and check after the join.
    if (r && r->targets[0].outcome == copy_target_outcome::verified) ++landed;
    if (r && r->targets[0].outcome == copy_target_outcome::name_taken) ++taken;
  });
  REQUIRE(landed == 1);
  REQUIRE(taken == 1);
  REQUIRE(no_temps_under(s.root()));
}

TEST_CASE("for_each_in_flight stops starting items once told to", "[io][deep]") {
  std::atomic<int> started{0};
  std::atomic<bool> stop{false};
  mv::io::for_each_in_flight(
      1000, 4,
      [&](std::size_t) {
        if (++started >= 10) stop = true;
      },
      [&] { return stop.load(); });
  REQUIRE(started >= 10);
  REQUIRE(started < 20);  // at most one more per thread after the stop
}

TEST_CASE("a local folder takes the sequential profile", "[io][deep]") {
  scratch_dir s("vprofile");
  fs::create_directories(s / "a");
  fs::create_directories(s / "b");
  const auto p = mv::io::copy_profile_for(utf8(s / "a/x.jpg"), utf8(s / "b/x.jpg"));
  REQUIRE(p.read_depth == 1);
  REQUIRE(p.write_depth == 1);
  const auto b = mv::io::batch_copy_profile(utf8(s / "a"), utf8(s / "b"));
  REQUIRE(b.files_in_flight == 1);
  REQUIRE_FALSE(mv::io::is_network_path(utf8(s / "a")));
}
