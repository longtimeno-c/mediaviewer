// SPDX-License-Identifier: GPL-2.0-or-later
// io/verified_copy.h: plan/18 "Verify" and the base app's F8 across volumes.
#include "catch_compat.h"

#include <atomic>
#include <string>
#include <vector>

#include "import_fixture.h"
#include "io/file_port.h"
#include "io/verified_copy.h"

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
