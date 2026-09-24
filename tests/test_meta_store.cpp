// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9: the host cache in front of meta::read, and the sort orders that use it.
// The reader is injected, so these prove *when* the file is read, not what is in it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/job_system.h"
#include "shell/meta_store.h"
#include "shell/sort_order.h"

namespace {

namespace io = mv::io;

using namespace std::chrono_literals;

io::dir_entry entry(std::string name, std::int64_t mtime = 1, std::uint64_t size = 10) {
  io::dir_entry e;
  e.name_utf8 = name;
  e.path_utf8 = "/dir/" + name;
  e.mtime_unix = mtime;
  e.size = size;
  return e;
}

// A one-shot gate the test waits on while the pool does the read.
struct gate {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
  void open() {
    std::lock_guard<std::mutex> l(m);
    ++count;
    cv.notify_all();
  }
  bool wait_for(int n) {
    std::unique_lock<std::mutex> l(m);
    return cv.wait_for(l, 5s, [&] { return count >= n; });
  }
};

mv::result<mv::meta::metadata> fake_read(std::string_view path) {
  mv::meta::metadata m;
  m.s.camera = std::string(path);
  m.s.width = 100;
  return m;
}

}  // namespace

TEST_CASE("a record is read once; toggling overlays is a lookup", "[meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  gate g;
  mv::shell::meta_store store(fake_read, [](std::string_view) { return std::nullopt; });
  const auto a = entry("a.jpg");

  REQUIRE(store.get(a, jobs, [&](std::string) { g.open(); }) == nullptr);  // miss: loading
  REQUIRE(g.wait_for(1));
  REQUIRE(store.reads() == 1);

  // The info overlay, the AF quads and the pane all read the same record.
  for (int i = 0; i < 100; ++i) {
    const auto rec = store.get(a, jobs, {});
    REQUIRE(rec != nullptr);
    CHECK(rec->s.width == 100);
    CHECK(store.peek(a) == rec);
  }
  CHECK(store.reads() == 1);
}

TEST_CASE("a second request while loading does not read twice", "[meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  gate release, started, done;
  mv::shell::meta_store store(
      [&](std::string_view p) {
        started.open();
        release.wait_for(1);
        return fake_read(p);
      },
      [](std::string_view) { return std::nullopt; });
  const auto a = entry("a.jpg");
  REQUIRE(store.get(a, jobs, [&](std::string) { done.open(); }) == nullptr);
  REQUIRE(started.wait_for(1));
  CHECK(store.get(a, jobs, {}) == nullptr);  // in flight
  release.open();
  REQUIRE(done.wait_for(1));
  CHECK(store.reads() == 1);
}

TEST_CASE("a file rewritten in place is read again", "[meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  gate g;
  mv::shell::meta_store store(fake_read, [](std::string_view) { return std::nullopt; });
  (void)store.get(entry("a.jpg", 1, 10), jobs, [&](std::string) { g.open(); });
  REQUIRE(g.wait_for(1));
  (void)store.get(entry("a.jpg", 2, 10), jobs, [&](std::string) { g.open(); });
  REQUIRE(g.wait_for(2));
  CHECK(store.reads() == 2);
}

TEST_CASE("an unreadable file caches an empty record instead of retrying", "[meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  gate g;
  mv::shell::meta_store store(
      [](std::string_view) -> mv::result<mv::meta::metadata> { return mv::err(mv::status::io); },
      [](std::string_view) { return std::nullopt; });
  const auto a = entry("gone.jpg");
  (void)store.get(a, jobs, [&](std::string) { g.open(); });
  REQUIRE(g.wait_for(1));
  const auto rec = store.get(a, jobs, {});
  REQUIRE(rec != nullptr);
  CHECK(rec->properties.empty());
  CHECK(rec->s.camera.empty());
  CHECK(store.reads() == 1);
}

TEST_CASE("the cache is bounded", "[meta][store]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  gate g;
  mv::shell::meta_store store(fake_read, [](std::string_view) { return std::nullopt; });
  const int n = static_cast<int>(mv::shell::meta_store::kCapacity) + 10;
  for (int i = 0; i < n; ++i) {
    (void)store.get(entry("f" + std::to_string(i) + ".jpg"), jobs, [&](std::string) { g.open(); });
  }
  REQUIRE(g.wait_for(n));
  CHECK(store.peek(entry("f0.jpg")) == nullptr);                       // evicted
  CHECK(store.peek(entry("f" + std::to_string(n - 1) + ".jpg")) != nullptr);
}

TEST_CASE("date keys are read once per file and dateless files are remembered", "[meta][sort]") {
  mv::job_system jobs;
  REQUIRE(mv::ok(jobs.start(2)));
  std::atomic<int> reads{0};
  gate g;
  mv::shell::meta_store store(fake_read, [&](std::string_view p) -> std::optional<std::int64_t> {
    ++reads;
    if (p.find("nodate") != std::string_view::npos) return std::nullopt;
    return 1000;
  });
  std::vector<io::dir_entry> files{entry("a.jpg"), entry("nodate.jpg"), entry("b.jpg")};
  CHECK_FALSE(store.date_key(files[0]).has_value());
  store.resolve_date_keys(files, jobs, [&] { g.open(); });
  REQUIRE(g.wait_for(1));
  CHECK(reads == 3);
  CHECK(store.date_key(files[0]) == 1000);
  CHECK_FALSE(store.date_key(files[1]).has_value());
  CHECK(store.consume_dates_changed());
  CHECK_FALSE(store.consume_dates_changed());

  store.resolve_date_keys(files, jobs, [&] { g.open(); });  // a re-sort: nothing to read
  REQUIRE(g.wait_for(2));
  CHECK(reads == 3);
}

TEST_CASE("sort orders", "[sort]") {
  using mv::shell::sort_entries;
  using mv::shell::sort_key;
  using mv::shell::sort_order;
  const auto names = [](const std::vector<io::dir_entry>& v) {
    std::string s;
    for (const auto& e : v) s += e.name_utf8 + " ";
    return s;
  };
  std::vector<io::dir_entry> v{entry("B.jpg", 30, 5), entry("a.png", 10, 50), entry("c.JPG", 20, 5)};

  sort_entries(v, {sort_key::name, false});
  CHECK(names(v) == "a.png B.jpg c.JPG ");
  sort_entries(v, {sort_key::name, true});
  CHECK(names(v) == "c.JPG B.jpg a.png ");
  sort_entries(v, {sort_key::modified, false});
  CHECK(names(v) == "a.png c.JPG B.jpg ");
  sort_entries(v, {sort_key::size, false});
  CHECK(names(v) == "B.jpg c.JPG a.png ");  // equal sizes fall back to name
  sort_entries(v, {sort_key::type, false});
  CHECK(names(v) == "B.jpg c.JPG a.png ");  // jpg == JPG, then name; png last
}

TEST_CASE("date taken sorts by the stamp and falls back to mtime", "[sort]") {
  using mv::shell::sort_key;
  std::vector<io::dir_entry> v{entry("late.jpg", 5), entry("early.jpg", 9), entry("nodate.jpg", 7)};
  const auto lookup = [](const io::dir_entry& e) -> std::optional<std::int64_t> {
    if (e.name_utf8 == "late.jpg") return 900;
    if (e.name_utf8 == "early.jpg") return 100;
    return std::nullopt;  // no stamp: sorts by its mtime, 7
  };
  mv::shell::sort_entries(v, {sort_key::date_taken, false}, lookup);
  CHECK(v[0].name_utf8 == "nodate.jpg");  // 7 < 100 < 900
  CHECK(v[1].name_utf8 == "early.jpg");
  CHECK(v[2].name_utf8 == "late.jpg");
  mv::shell::sort_entries(v, {sort_key::date_taken, true}, lookup);
  CHECK(v[0].name_utf8 == "late.jpg");
}

TEST_CASE("sort order round-trips through settings", "[sort]") {
  using namespace mv::shell;
  for (int k = 0; k < static_cast<int>(sort_key::count); ++k) {
    for (bool d : {false, true}) {
      const sort_order o{static_cast<sort_key>(k), d};
      const sort_order back = unpack_sort(pack_sort(o));
      CHECK(back.key == o.key);
      CHECK(back.descending == d);
    }
  }
  CHECK(unpack_sort(7).key == sort_key::name);  // out-of-range key is name, not UB
}
