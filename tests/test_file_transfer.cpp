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

namespace {

// A server's folder as remote_ops over a local folder: "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/<rel>".
struct fake_server {
  fs::path root;
  int removes = 0;
  bool refuse_remove = false;
  std::string prefix = "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/";
  fs::path at(const std::string& key) const {
    std::string rel = key.substr(prefix.size());
    if (!rel.empty() && rel.back() == '/') rel.pop_back();
    return rel.empty() ? root : root / fs::path(rel);
  }
  remote_ops ops() {
    remote_ops o;
    o.exists = [this](const std::string& k) -> mv::result<bool> { return fs::exists(at(k)); };
    o.make_folder = [this](const std::string& parent, const std::string& name) -> mv::result<std::string> {
      if (fs::exists(at(parent) / name)) return mv::err(mv::status::permission_denied);
      fs::create_directories(at(parent) / name);
      return parent + name + "/";
    };
    o.add = [this](const std::string& parent, const std::string& local, const std::string& name,
                   const std::atomic<bool>*, std::atomic<std::uint64_t>*) -> mv::result<std::string> {
      fs::copy_file(local, at(parent) / name);  // throws on a taken name: the test would see it
      return parent + name;
    };
    o.move = [this](const std::string& key, const std::string& to, const std::string& name,
                    bool& taken) -> mv::result<std::string> {
      if (fs::exists(at(to) / name)) {
        taken = true;
        return mv::err(mv::status::permission_denied);
      }
      fs::rename(at(key), at(to) / name);
      return to + name;
    };
    o.remove = [this](const std::string& key) -> mv::result<std::string> {
      if (refuse_remove) return mv::err(mv::status::permission_denied);
      ++removes;
      fs::remove_all(at(key));
      return std::string("2026-11-22/x");
    };
    o.fetch = [this](const std::string& key, const std::atomic<bool>*, std::atomic<std::uint64_t>*) -> mv::result<std::string> {
      return utf8(at(key));  // the host's cache stands in for it
    };
    return o;
  }
};

}  // namespace

TEST_CASE("file_transfer: files and folders go up to a server's folder, never replacing", "[file_transfer]") {
  scratch_dir s("xfer_up");
  fake_server srv{s / "server"};
  fs::create_directories(srv.root / "Trips");
  write_text(srv.root / "Trips" / "a.jpg", "theirs");
  write_text(s / "local/a.jpg", "mine");
  write_text(s / "local/day1/b.jpg", "b");
  auto target = mixed_target(srv.ops());
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::copy, {utf8(s / "local/a.jpg"), utf8(s / "local/day1")}, srv.prefix + "Trips/",
                              *target, progress);
  CHECK(r.succeeded() == 2);
  CHECK(text_of(srv.root / "Trips" / "a.jpg") == "theirs");
  CHECK(text_of(srv.root / "Trips" / "a (2).jpg") == "mine");
  CHECK(text_of(srv.root / "Trips" / "day1" / "b.jpg") == "b");
  CHECK(fs::exists(s / "local/a.jpg"));  // a copy up leaves the original
}

TEST_CASE("file_transfer: a move off the server removes it there only after the copy here is whole",
          "[file_transfer]") {
  scratch_dir s("xfer_down");
  fake_server srv{s / "server"};
  write_text(srv.root / "Trips" / "a.jpg", "a");
  fs::create_directories(s / "here");
  auto target = mixed_target(srv.ops());
  transfer_progress progress;
  auto r = run_transfer(drop_verb::move, {srv.prefix + "Trips/a.jpg"}, utf8(s / "here"), *target, progress);
  CHECK(r.succeeded() == 1);
  CHECK(text_of(s / "here" / "a.jpg") == "a");
  CHECK_FALSE(fs::exists(srv.root / "Trips" / "a.jpg"));
  CHECK(srv.removes == 1);

  // The server will not let it go: the copy here is taken back, so it is never two.
  write_text(srv.root / "Trips" / "b.jpg", "b");
  srv.refuse_remove = true;
  auto target2 = mixed_target(srv.ops());
  r = run_transfer(drop_verb::move, {srv.prefix + "Trips/b.jpg"}, utf8(s / "here"), *target2, progress);
  CHECK(r.failed() == 1);
  CHECK(fs::exists(srv.root / "Trips" / "b.jpg"));
  CHECK_FALSE(fs::exists(s / "here" / "b.jpg"));

  // A copy down never touches the server.
  srv.refuse_remove = false;
  r = run_transfer(drop_verb::copy, {srv.prefix + "Trips/b.jpg"}, utf8(s / "here"), *target2, progress);
  CHECK(r.succeeded() == 1);
  CHECK(fs::exists(srv.root / "Trips" / "b.jpg"));
  CHECK(srv.removes == 1);
}

TEST_CASE("file_transfer: within one server a move is its own, and a taken name keeps both", "[file_transfer]") {
  scratch_dir s("xfer_within");
  fake_server srv{s / "server"};
  write_text(srv.root / "Trips" / "a.jpg", "a");
  write_text(srv.root / "Trips" / "day1" / "c.jpg", "c");
  fs::create_directories(srv.root / "Archive");
  auto target = mixed_target(srv.ops());
  transfer_progress progress;
  auto r = run_transfer(drop_verb::move, {srv.prefix + "Trips/a.jpg", srv.prefix + "Trips/day1/"},
                        srv.prefix + "Archive/", *target, progress);
  CHECK(r.succeeded() == 2);
  CHECK(text_of(srv.root / "Archive" / "a.jpg") == "a");
  CHECK(text_of(srv.root / "Archive" / "day1" / "c.jpg") == "c");
  CHECK(srv.removes == 0);  // a move, not a copy and a delete

  // The name was free when planned and taken by the time it moved: keep both.
  write_text(srv.root / "Trips" / "a.jpg", "second");
  struct racing : transfer_target {
    std::unique_ptr<transfer_target> real;
    fake_server* srv;
    int asked = 0;
    bool exists(const std::string& p) override { return ++asked == 1 ? false : real->exists(p); }
    mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
    mv::status put_file(const std::string& a, const std::string& b, bool v, const std::atomic<bool>* c,
                        std::atomic<std::uint64_t>* d) override { return real->put_file(a, b, v, c, d); }
    rename_outcome rename(const std::string& a, const std::string& b) override { return real->rename(a, b); }
    mv::status remove(const std::string& p) override { return real->remove(p); }
  } race;
  race.real = mixed_target(srv.ops());
  race.srv = &srv;
  r = run_transfer(drop_verb::move, {srv.prefix + "Trips/a.jpg"}, srv.prefix + "Archive/", race, progress);
  REQUIRE(r.succeeded() == 1);
  CHECK(text_of(srv.root / "Archive" / "a.jpg") == "a");
  CHECK(text_of(srv.root / "Archive" / "a (2).jpg") == "second");
}

TEST_CASE("file_transfer: a server's busy says try again, not failed", "[file_transfer]") {
  transfer_result r;
  r.verb = drop_verb::move;
  r.items.push_back({"remote:0123456789abcdef/a1a1a1a1a1a1a1a1/a.jpg", "remote:0123456789abcdef/a1a1a1a1a1a1a1a1/Trip/a.jpg",
                     mv::status::busy});
  CHECK(transfer_summary(r, "Trip") == "Could not move a.jpg to Trip · in use there right now: try again in a moment");
  r.items.push_back({"/x/b.jpg", "/t/b.jpg", mv::status::ok});
  CHECK(transfer_summary(r, "Trip") == "Moved 1 of 2 to Trip · in use there right now: try again in a moment");

  // Through the seam: the service's busy on a move reaches the item as busy.
  scratch_dir s("xfer_busy");
  fake_server srv{s / "server"};
  write_text(srv.root / "a.jpg", "a");
  fs::create_directories(srv.root / "Trip");
  auto ops = srv.ops();
  ops.move = [](const std::string&, const std::string&, const std::string&, bool&) -> mv::result<std::string> {
    return mv::err(mv::status::busy);
  };
  auto target = mixed_target(ops);
  transfer_progress progress;
  const auto moved = run_transfer(drop_verb::move, {srv.prefix + "a.jpg"}, srv.prefix + "Trip/", *target, progress);
  REQUIRE(moved.items.size() == 1);
  CHECK(moved.items[0].status == mv::status::busy);
  CHECK(fs::exists(srv.root / "a.jpg"));
}

namespace {
// The local folder, but its first `blind` exists() calls say "free": the plan sees an empty
// folder while another job or app has already put a file there.
class blind_planner : public transfer_target {
 public:
  std::unique_ptr<transfer_target> real = local_target();
  int blind = 0;
  bool exists(const std::string& p) override { return blind-- > 0 ? false : real->exists(p); }
  mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
  mv::status put_file(const std::string& s, const std::string& d, bool v, const std::atomic<bool>* c,
                      std::atomic<std::uint64_t>* b) override {
    return real->put_file(s, d, v, c, b);
  }
  rename_outcome rename(const std::string& s, const std::string& d) override { return real->rename(s, d); }
  mv::status remove(const std::string& p) override { return real->remove(p); }
};
}  // namespace

TEST_CASE("file_transfer: a name taken after the plan gets a fresh name; the other file stays", "[file_transfer]") {
  scratch_dir s("xfer_raced");
  write_text(s / "src/a.jpg", "mine");
  // Another job's (or app's) file, there before this copy but after its plan.
  write_text(s / "dst/a.jpg", "theirs");
  blind_planner target;
  target.blind = 1;  // the plan's one look
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::copy, {utf8(s / "src/a.jpg")}, utf8(s / "dst"), target, progress);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].status == mv::status::ok);
  CHECK(r.items[0].dest == utf8(s / "dst/a (2).jpg"));
  CHECK(text_of(s / "dst/a.jpg") == "theirs");  // never deleted
  CHECK(text_of(s / "dst/a (2).jpg") == "mine");

  // A cross-volume move the same way: the other file stays, the source goes only once copied.
  write_text(s / "src/b.jpg", "moved");
  write_text(s / "dst/b.jpg", "foreign");
  other_volume ov;
  struct blind_ov : transfer_target {
    other_volume* real;
    int blind = 1;
    bool exists(const std::string& p) override { return blind-- > 0 ? false : real->exists(p); }
    mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
    mv::status put_file(const std::string& a, const std::string& b, bool v, const std::atomic<bool>* c,
                        std::atomic<std::uint64_t>* d) override { return real->put_file(a, b, v, c, d); }
    rename_outcome rename(const std::string& a, const std::string& b) override { return real->rename(a, b); }
    mv::status remove(const std::string& p) override { return real->remove(p); }
  } bov;
  bov.real = &ov;
  const auto m = run_transfer(drop_verb::move, {utf8(s / "src/b.jpg")}, utf8(s / "dst"), bov, progress);
  REQUIRE(m.succeeded() == 1);
  CHECK(text_of(s / "dst/b.jpg") == "foreign");
  CHECK(text_of(s / "dst/b (2).jpg") == "moved");
  CHECK_FALSE(fs::exists(s / "src/b.jpg"));
}

TEST_CASE("file_transfer: names that differ only in case are two names in one batch", "[file_transfer]") {
  auto none = [](std::string_view) { return false; };
  const auto plan = plan_transfer({"/x/IMG_1.JPG", "/y/img_1.jpg"}, "/t", drop_verb::copy, none);
  REQUIRE(plan.size() == 2);
  CHECK(plan[0].dest == "/t/IMG_1.JPG");
  CHECK(plan[1].dest == "/t/img_1 (2).jpg");
}

TEST_CASE("file_transfer: keep-both after a taken rename copies to, cleans and reports the new name",
          "[file_transfer]") {
  scratch_dir s("xfer_keepboth");
  write_text(s / "src/a.jpg", "mine");
  fs::create_directories(s / "dst");
  // A rename that finds the name taken (and it is: another app's file), then crosses volumes.
  struct taken_then_far : transfer_target {
    std::unique_ptr<transfer_target> real = local_target();
    int renames = 0;
    bool fail_put = false;
    bool exists(const std::string& p) override { return real->exists(p); }
    mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
    mv::status put_file(const std::string& a, const std::string& b, bool v, const std::atomic<bool>* c,
                        std::atomic<std::uint64_t>* d) override {
      return fail_put ? mv::status::io : real->put_file(a, b, v, c, d);
    }
    rename_outcome rename(const std::string&, const std::string& d) override {
      if (++renames == 1) {
        write_text(fs::path(d), "theirs");  // it appeared just now
        return rename_outcome::taken;
      }
      return rename_outcome::other_volume;
    }
    mv::status remove(const std::string& p) override { return real->remove(p); }
  } t;
  transfer_progress progress;
  auto r = run_transfer(drop_verb::move, {utf8(s / "src/a.jpg")}, utf8(s / "dst"), t, progress);
  REQUIRE(r.succeeded() == 1);
  CHECK(r.items[0].dest == utf8(s / "dst/a (2).jpg"));
  CHECK(text_of(s / "dst/a.jpg") == "theirs");
  CHECK(text_of(s / "dst/a (2).jpg") == "mine");

  // The same, and the copy fails: the taken name's file is left alone.
  write_text(s / "src/c.jpg", "mine");
  taken_then_far t2;
  t2.fail_put = true;
  r = run_transfer(drop_verb::move, {utf8(s / "src/c.jpg")}, utf8(s / "dst"), t2, progress);
  CHECK(r.failed() == 1);
  CHECK(text_of(s / "dst/c.jpg") == "theirs");
  CHECK(fs::exists(s / "src/c.jpg"));
}

TEST_CASE("file_transfer: a failed folder copy leaves what another app put in it", "[file_transfer]") {
  scratch_dir s("xfer_foreign");
  write_text(s / "src/trip/1.jpg", "1");
  write_text(s / "src/trip/2.jpg", "2");
  fs::create_directories(s / "dst");
  other_volume target;
  target.fail_after = 1;
  // Something else saves into the new folder while it fills.
  struct saving : transfer_target {
    other_volume* real;
    fs::path foreign;
    bool exists(const std::string& p) override { return real->exists(p); }
    mv::status make_directory(const std::string& p) override { return real->make_directory(p); }
    mv::status put_file(const std::string& a, const std::string& b, bool v, const std::atomic<bool>* c,
                        std::atomic<std::uint64_t>* d) override {
      if (!fs::exists(foreign)) write_text(foreign, "keep me");
      return real->put_file(a, b, v, c, d);
    }
    rename_outcome rename(const std::string& a, const std::string& b) override { return real->rename(a, b); }
    mv::status remove(const std::string& p) override { return real->remove(p); }
  } t;
  t.real = &target;
  t.foreign = s / "dst/trip/notes.txt";
  transfer_progress progress;
  const auto r = run_transfer(drop_verb::move, {utf8(s / "src/trip")}, utf8(s / "dst"), t, progress);
  CHECK(r.failed() == 1);
  CHECK(text_of(s / "dst/trip/notes.txt") == "keep me");  // not this item's: never removed
  CHECK_FALSE(fs::exists(s / "dst/trip/1.jpg"));           // this item's: taken back
  CHECK(text_of(s / "src/trip/1.jpg") == "1");
}
