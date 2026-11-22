// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// shell/file_transfer.h: a drop's copy / move on real folders. Never replaces,
// moves across volumes only after a verified copy, cleans up what it wrote.
#include "catch_compat.h"

#include <atomic>
#include <string>
#include <vector>

#include "import_fixture.h"
#include "shell/file_transfer.h"

using namespace mv::test;
using namespace mv::shell;

namespace {

// The local folder, as if every rename crossed a volume: the copy-then-remove path.
class other_volume : public transfer_target {
 public:
  std::unique_ptr<transfer_target> real = local_target();
  int puts = 0;
  int fail_after = -1;  // put_file fails once this many have succeeded
  bool verified = false;
  bool exists(const std::string& p) override { return real->exists(p); }
  mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
  mv::status put_file(const std::string& s, const std::string& d, bool verify, const std::atomic<bool>* c,
                      std::atomic<std::uint64_t>* b) override {
    if (fail_after >= 0 && puts >= fail_after) return mv::status::io;
    ++puts;
    verified = verified || verify;
    return real->put_file(s, d, verify, c, b);
  }
  rename_outcome rename(const std::string&, const std::string&) override { return rename_outcome::other_volume; }
  mv::status remove(const std::string& p) override { return real->remove(p); }
};

std::string text_of(const fs::path& p) {
  const auto b = read_bytes(p);
  return std::string(b.begin(), b.end());
}

}  // namespace

TEST_CASE("file_transfer: a copy keeps both, Finder-style numbering", "[file_transfer]") {
  scratch_dir s("xfer_copy");
  write_text(s / "src/a.jpg", "new");
  write_text(s / "dst/a.jpg", "old");
  auto target = local_target();
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::copy, {utf8(s / "src/a.jpg")}, utf8(s / "dst"), *target, progress);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].status == mv::status::ok);
  CHECK(text_of(s / "dst/a.jpg") == "old");        // never replaced
  CHECK(text_of(s / "dst/a (2).jpg") == "new");
  CHECK(text_of(s / "src/a.jpg") == "new");        // the original stays
  CHECK(progress.done_items.load() == 1);
  CHECK(progress.done_bytes.load() == progress.total_bytes.load());
  CHECK(transfer_summary(r, utf8(s / "dst")) == "Copied a.jpg to dst");
}

TEST_CASE("file_transfer: a move on one volume is a rename, folders whole", "[file_transfer]") {
  scratch_dir s("xfer_move");
  write_text(s / "src/trip/day1/a.jpg", "a");
  write_text(s / "src/trip/b.jpg", "b");
  fs::create_directories(s / "dst");
  auto target = local_target();
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "src/trip")}, utf8(s / "dst"), *target, progress);
  REQUIRE(r.succeeded() == 1);
  CHECK_FALSE(fs::exists(s / "src/trip"));
  CHECK(text_of(s / "dst/trip/day1/a.jpg") == "a");
  CHECK(text_of(s / "dst/trip/b.jpg") == "b");
  CHECK(transfer_summary(r, utf8(s / "dst")) == "Moved trip to dst");
}

TEST_CASE("file_transfer: across volumes a move copies, verifies, then removes", "[file_transfer]") {
  scratch_dir s("xfer_cross");
  write_bytes(s / "src/trip/a.raw", pattern(300000, 7));
  write_text(s / "src/b.jpg", "b");
  fs::create_directories(s / "dst");
  other_volume target;
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "src/trip"), utf8(s / "src/b.jpg")}, utf8(s / "dst"),
                              target, progress);
  CHECK(r.succeeded() == 2);
  CHECK(target.verified);  // a move's copy is read back before the original goes
  CHECK(read_bytes(s / "dst/trip/a.raw") == pattern(300000, 7));
  CHECK(text_of(s / "dst/b.jpg") == "b");
  CHECK_FALSE(fs::exists(s / "src/trip"));
  CHECK_FALSE(fs::exists(s / "src/b.jpg"));
  CHECK(transfer_summary(r, utf8(s / "dst")) == "Moved 2 items to dst");
}

TEST_CASE("file_transfer: a copy that fails part way leaves nothing half and the source whole",
          "[file_transfer]") {
  scratch_dir s("xfer_fail");
  write_text(s / "src/trip/1.jpg", "1");
  write_text(s / "src/trip/2.jpg", "2");
  fs::create_directories(s / "dst");
  other_volume target;
  target.fail_after = 1;  // the second file fails
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "src/trip")}, utf8(s / "dst"), target, progress);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].status != mv::status::ok);
  CHECK_FALSE(fs::exists(s / "dst/trip"));       // what it wrote is gone
  CHECK(text_of(s / "src/trip/1.jpg") == "1");   // and nothing left the source
  CHECK(text_of(s / "src/trip/2.jpg") == "2");
  CHECK(transfer_summary(r, utf8(s / "dst")) == "Could not move trip to dst");
}

#if !defined(_WIN32)
TEST_CASE("file_transfer: a source that will not go takes its copy back", "[file_transfer]") {
  scratch_dir s("xfer_locked");
  write_text(s / "src/a.jpg", "a");
  fs::create_directories(s / "dst");
  // The folder holding it cannot be written: the file cannot be removed.
  fs::permissions(s / "src", fs::perms::owner_read | fs::perms::owner_exec);
  other_volume target;
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "src/a.jpg")}, utf8(s / "dst"), target, progress);
  fs::permissions(s / "src", fs::perms::owner_all);
  CHECK(r.failed() == 1);
  CHECK(fs::exists(s / "src/a.jpg"));
  CHECK_FALSE(fs::exists(s / "dst/a.jpg"));  // a move is never two copies
}
#endif

TEST_CASE("file_transfer: a move skips files already in that folder", "[file_transfer]") {
  scratch_dir s("xfer_here");
  write_text(s / "dst/a.jpg", "a");
  write_text(s / "src/b.jpg", "b");
  auto target = local_target();
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "dst/a.jpg"), utf8(s / "src/b.jpg")}, utf8(s / "dst"),
                              *target, progress);
  REQUIRE(r.items.size() == 1);
  CHECK(text_of(s / "dst/a.jpg") == "a");
  CHECK(text_of(s / "dst/b.jpg") == "b");
}

TEST_CASE("file_transfer: cancel stops before the next item and says so", "[file_transfer]") {
  scratch_dir s("xfer_cancel");
  write_text(s / "src/a.jpg", "a");
  fs::create_directories(s / "dst");
  auto target = local_target();
  transfer_progress progress;
  std::atomic<bool> cancel{true};
  const auto r = run_transfer(drop_verb::copy, {utf8(s / "src/a.jpg")}, utf8(s / "dst"), *target, progress, &cancel);
  CHECK(r.cancelled);
  CHECK_FALSE(fs::exists(s / "dst/a.jpg"));
  CHECK(transfer_summary(r, utf8(s / "dst")) == "Cancelled · nothing changed");
}

TEST_CASE("file_transfer: New Folder takes the next free name", "[file_transfer]") {
  scratch_dir s("xfer_mkdir");
  auto target = local_target();
  const std::string a = make_new_folder(utf8(s.root()), "untitled folder", *target);
  const std::string b = make_new_folder(utf8(s.root()), "untitled folder", *target);
  CHECK(a == utf8(s / "untitled folder"));
  CHECK(b == utf8(s / "untitled folder (2)"));
  CHECK(fs::is_directory(s / "untitled folder (2)"));
}

TEST_CASE("transfer_board: rows report progress, finish, cancel and clear", "[file_transfer]") {
  transfer_board board;
  auto j = board.add("Copying 2 items to dst", "/dst", 1000);
  CHECK((j->id & kTransferJobBit) != 0);
  j->state.store(2);
  j->progress.total_bytes.store(200);
  j->progress.done_bytes.store(50);
  transfer_job_view v;
  REQUIRE(board.view(j->id, 1500, v));
  CHECK(v.state == 2);
  CHECK(v.fraction == 0.25);
  CHECK(v.elapsed_ms == 500);
  CHECK(board.any_running());
  board.cancel(j->id);
  CHECK(j->cancel.load());
  transfer_result r;
  r.cancelled = true;
  board.finish(j, r, 2000);
  REQUIRE(board.view(j->id, 9000, v));
  CHECK(v.state == 5);
  CHECK(v.elapsed_ms == 1000);
  const auto g = board.generation();
  board.clear_finished();
  CHECK(board.generation() != g);
  CHECK(board.ids().empty());
}
