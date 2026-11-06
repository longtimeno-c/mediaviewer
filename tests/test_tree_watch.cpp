// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io::tree_watcher (docs/plans/index-watcher.md): a whole tree, the folder
// that changed, a folder moved in as a subtree, roots that cannot be watched.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "io/tree_watch.h"

namespace fs = std::filesystem;

namespace {

struct temp_dir {
  fs::path root;
  temp_dir() {
    root = fs::temp_directory_path() / ("mv_tree_watch_" + std::to_string(std::random_device{}()));
    fs::create_directories(root);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

struct seen {
  std::uint32_t root;
  std::string dir;
  std::uint32_t flags;
};

struct event_log {
  std::mutex m;
  std::vector<seen> events;
  std::atomic<int> calls{0};
  static void cb(void* user, std::uint32_t root, std::string_view dir, std::uint32_t flags) {
    auto* l = static_cast<event_log*>(user);
    std::lock_guard lock(l->m);
    l->events.push_back(seen{root, std::string(dir), flags});
    ++l->calls;
  }
  // Waits up to 10 s for an event that `match` accepts.
  bool wait_for(const std::function<bool(const seen&)>& match) {
    for (int i = 0; i < 200; ++i) {
      {
        std::lock_guard lock(m);
        for (const seen& e : events) {
          if (match(e)) return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }
};

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << "x";
}

bool same_dir(const std::string& reported, const fs::path& expected) {
  std::error_code ec;
  return fs::equivalent(fs::path(reported), expected, ec);
}

}  // namespace

TEST_CASE("tree_watcher names the folder a file landed in, however deep", "[io][tree_watch]") {
  temp_dir t;
  fs::create_directories(t.root / "a" / "b");
  event_log l;
  mv::io::tree_watcher w;
  std::vector<bool> ok;
  REQUIRE(w.start({t.root.string()}, &event_log::cb, &l, ok));
  REQUIRE(ok == std::vector<bool>{true});
  touch(t.root / "a" / "b" / "photo.jpg");
  CHECK(l.wait_for([&](const seen& e) { return e.root == 0 && same_dir(e.dir, t.root / "a" / "b"); }));
  // Spelled from the root as it was given (on the Mac, /var, not /private/var).
  std::lock_guard lock(l.m);
  for (const seen& e : l.events) CHECK(e.dir.rfind(t.root.string(), 0) == 0);
}

TEST_CASE("tree_watcher reports a folder moved in as a subtree", "[io][tree_watch]") {
  temp_dir t;
  fs::create_directories(t.root / "watched");
  touch(t.root / "outside" / "trip" / "day2" / "photo.jpg");
  event_log l;
  mv::io::tree_watcher w;
  std::vector<bool> ok;
  REQUIRE(w.start({(t.root / "watched").string()}, &event_log::cb, &l, ok));
  fs::rename(t.root / "outside" / "trip", t.root / "watched" / "trip");
  CHECK(l.wait_for([&](const seen& e) {
    return (e.flags & mv::io::tree_watcher::subtree) != 0 && same_dir(e.dir, t.root / "watched" / "trip");
  }));
}

TEST_CASE("tree_watcher watches the roots it can and says which it cannot", "[io][tree_watch]") {
  temp_dir t;
  fs::create_directories(t.root / "one");
  fs::create_directories(t.root / "two");
  event_log l;
  mv::io::tree_watcher w;
  std::vector<bool> ok;
  REQUIRE(w.start({(t.root / "one").string(), (t.root / "missing").string(), (t.root / "two").string()}, &event_log::cb,
                  &l, ok));
  CHECK(ok == std::vector<bool>{true, false, true});
  touch(t.root / "two" / "photo.jpg");
  CHECK(l.wait_for([&](const seen& e) { return e.root == 2 && same_dir(e.dir, t.root / "two"); }));

  std::vector<bool> none;
  CHECK_FALSE(w.start({(t.root / "missing").string()}, &event_log::cb, &l, none));
  CHECK(none == std::vector<bool>{false});
}

TEST_CASE("tree_watcher calls nothing once stopped", "[io][tree_watch]") {
  temp_dir t;
  event_log l;
  mv::io::tree_watcher w;
  std::vector<bool> ok;
  REQUIRE(w.start({t.root.string()}, &event_log::cb, &l, ok));
  touch(t.root / "first.jpg");
  REQUIRE(l.wait_for([&](const seen&) { return true; }));
  w.stop();
  const int calls = l.calls.load();
  touch(t.root / "second.jpg");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(l.calls.load() == calls);
}
