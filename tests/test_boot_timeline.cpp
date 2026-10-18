// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/boot_timeline.h"
#include "core/json.h"

using mv::boot::stage;

namespace {

double at(const mv::boot::snapshot& s, stage st) { return s.ms[static_cast<std::size_t>(st)]; }

}  // namespace

TEST_CASE("boot timeline: unset stages read -1, first mark wins", "[core][boot]") {
  mv::boot::reset_for_test();
  mv::boot::set_process_age(50'000'000);  // 50 ms old
  mv::boot::mark(stage::host_entry);
  const auto a = mv::boot::read();
  REQUIRE(a.from_process_start);
  CHECK(at(a, stage::host_entry) >= 50.0);
  CHECK(at(a, stage::first_pixel) == -1.0);

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  mv::boot::mark(stage::host_entry);  // ignored
  CHECK(at(mv::boot::read(), stage::host_entry) == at(a, stage::host_entry));

  mv::boot::set_process_age(1'000'000'000);  // ignored: the origin is set once
  CHECK(at(mv::boot::read(), stage::host_entry) == at(a, stage::host_entry));
}

TEST_CASE("boot timeline: without a process age the origin is the first mark", "[core][boot]") {
  mv::boot::reset_for_test();
  mv::boot::mark(stage::window_created);
  const auto s = mv::boot::read();
  CHECK_FALSE(s.from_process_start);
  CHECK(at(s, stage::window_created) >= 0.0);
  CHECK(at(s, stage::window_created) < 5.0);
}

TEST_CASE("boot timeline: marks from many threads keep one value each", "[core][boot]") {
  mv::boot::reset_for_test();
  mv::boot::set_process_age(0);
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([] {
      for (std::size_t s = 0; s < mv::boot::stage_count; ++s) mv::boot::mark(static_cast<stage>(s));
    });
  }
  for (auto& t : threads) t.join();
  const auto s = mv::boot::read();
  for (std::size_t i = 0; i < mv::boot::stage_count; ++i) CHECK(s.ms[i] >= 0.0);
}

TEST_CASE("boot timeline: the JSON report parses and names every stage", "[core][boot]") {
  mv::boot::reset_for_test();
  mv::boot::set_process_age(10'000'000);
  mv::boot::mark(stage::host_entry);
  mv::boot::mark(stage::window_shown);
  const auto s = mv::boot::read();
  char buf[4096];
  const std::size_t n = mv::boot::write_json(s, buf, sizeof buf);
  REQUIRE(n > 0);
  REQUIRE(std::strlen(buf) == n);
  const auto doc = mv::json::parse(std::string_view(buf, n));
  REQUIRE(doc.has_value());
  CHECK(doc->find("schema")->i == 1);
  CHECK(doc->find("origin")->s == "process_start");
  const mv::json::value* stages = doc->find("stages_ms");
  REQUIRE(stages);
  CHECK(stages->o.size() == mv::boot::stage_count);
  for (std::size_t i = 0; i < mv::boot::stage_count; ++i) {
    CHECK(stages->find(mv::boot::name(static_cast<stage>(i))) != nullptr);
  }
  CHECK(stages->find("first_pixel")->d == -1.0);
  // Only reached stages get a gap; the first gap is from the origin.
  const mv::json::value* gaps = doc->find("gaps_ms");
  REQUIRE(gaps);
  CHECK(gaps->o.size() == 2);
  CHECK(gaps->find("host_entry")->d >= 10.0);

  // Too small a buffer is refused, not truncated.
  char tiny[32];
  CHECK(mv::boot::write_json(s, tiny, sizeof tiny) == 0);
  CHECK(tiny[0] == '\0');

  char line[512];
  REQUIRE(mv::boot::write_line(s, line, sizeof line) > 0);
  CHECK(std::string(line).find("window_shown=") != std::string::npos);
  CHECK(std::string(line).find("first_pixel") == std::string::npos);
  mv::boot::reset_for_test();
}
