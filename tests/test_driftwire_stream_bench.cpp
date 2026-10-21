// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #529: udp_hub's stream (the transfer over a path met on the DHT) over
// emulated internet paths (driftwire_path.h), one JSON line per profile.
//
//   mv_import_tests "[.driftwire-stream-bench]"
//     MV_STREAM_BENCH_S         seconds per profile (default 15)
//     MV_STREAM_BENCH_PROFILES  comma-separated names (default all)
//     MV_STREAM_BENCH_SEED      the paths' seed (default 1)
//
// "util" is goodput over the bottleneck's payload rate (1200 of 1238 bytes
// per packet on the wire). Loopback, so the machine's own load shows: run
// base and new alternated, as docs/DEVELOPMENT.md's performance suite says.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/udp_hub.h"
#include "driftwire_path.h"

namespace sp = mv::driftwire;
namespace pt = mv::driftwire::test;

namespace {

constexpr pt::path_profile kProfiles[] = {
    {"lan-ish", 4, 300, 20, 0, 0},
    {"home-fibre", 25, 100, 50, 0, 0},
    {"cross-country", 80, 50, 40, 0.001, 0},
    {"intercontinental", 160, 30, 30, 0.003, 0},
    {"wifi-jitter", 30, 80, 60, 0.005, 15},
    {"bufferbloat", 40, 20, 500, 0, 0},
};

double env_num(const char* name, double fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atof(v) : fallback;
}

bool wanted(const char* name) {
  const char* v = std::getenv("MV_STREAM_BENCH_PROFILES");
  if (!v || !*v) return true;
  const std::string list = std::string(",") + v + ",";
  return list.find(std::string(",") + name + ",") != std::string::npos;
}

struct emulated_pair {
  pt::path_counters ca, cb;
  std::shared_ptr<pt::path_direction> ab, ba;
  std::shared_ptr<sp::udp_hub> a, b;
  emulated_pair(const pt::path_profile& p, std::uint32_t seed)
      : ab(std::make_shared<pt::path_direction>(p, seed, ca)),
        ba(std::make_shared<pt::path_direction>(p, seed * 7919u + 1, cb)) {
    auto wrap = [](std::shared_ptr<pt::path_direction> d) {
      return [d](std::unique_ptr<sp::net::udp_socket> s) -> std::unique_ptr<sp::net::udp_socket> {
        return std::make_unique<pt::path_socket>(d, std::move(s));
      };
    };
    auto x = sp::udp_hub::open(0, true, wrap(ab));
    auto y = sp::udp_hub::open(0, true, wrap(ba));
    REQUIRE(x);
    REQUIRE(y);
    a = std::move(*x);
    b = std::move(*y);
  }
  ~emulated_pair() {
    a->stop();
    b->stop();
  }
};

}  // namespace

TEST_CASE("driftwire: the UDP stream over emulated internet paths", "[.driftwire-stream-bench]") {
  const double seconds = env_num("MV_STREAM_BENCH_S", 15);
  const auto seed = static_cast<std::uint32_t>(env_num("MV_STREAM_BENCH_SEED", 1));
  for (const auto& p : kProfiles) {
    if (!wanted(p.name)) continue;
    emulated_pair h(p, seed);
    auto sa = h.a->stream({"127.0.0.1", h.b->port()}, 529);
    auto sb = h.b->stream({"127.0.0.1", h.a->port()}, 529);
    std::atomic<bool> stop{false};
    std::thread w([&] {
      std::vector<std::uint8_t> chunk(256 * 1024, 0x5a);
      while (!stop.load()) {
        if (!sa->write_all(chunk)) break;
      }
    });
    std::vector<std::uint8_t> buf(256 * 1024);
    std::uint64_t got = 0, got_at_5s = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto end = t0 + std::chrono::duration<double>(seconds);
    bool marked = false;
    double cwnd_sum = 0;
    int cwnd_n = 0;
    auto next_sample = t0;
    while (std::chrono::steady_clock::now() < end) {
      auto n = sb->read_some(buf, 50);
      if (n && *n > 0) got += *n;
      if (!n && n.error() != mv::status::timeout) break;
      const auto now = std::chrono::steady_clock::now();
      if (!marked && now - t0 >= std::chrono::seconds(5)) {
        got_at_5s = got;
        marked = true;
      }
      if (now >= next_sample) {
        if (auto st = h.a->stats(529)) {
          cwnd_sum += st->cwnd;
          ++cwnd_n;
        }
        next_sample = now + std::chrono::milliseconds(100);
      }
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto st = h.a->stats(529).value_or(sp::udp_hub::stream_stats{});
    stop.store(true);
    sa->shutdown();
    sb->shutdown();
    w.join();
    const double mb_s = static_cast<double>(got) / 1e6 / took;
    const double steady = took > 5 ? static_cast<double>(got - got_at_5s) / 1e6 / (took - 5) : mb_s;
    const double payload_rate = p.mbit * 1e6 / 8 / 1e6 * (1200.0 / 1238.0);
    std::printf(
        "{\"case\":\"stream_path\",\"profile\":\"%s\",\"rtt_ms\":%.0f,\"mbit\":%.0f,\"queue_ms\":%.0f,\"loss\":%.4f,"
        "\"jitter_ms\":%.0f,\"seconds\":%.1f,\"mb_s\":%.2f,\"mb_s_after_5s\":%.2f,\"util\":%.2f,\"util_after_5s\":%.2f,"
        "\"packets\":%llu,\"retransmits\":%llu,\"rtos\":%llu,\"probes\":%llu,\"episodes\":%llu,\"srtt_ms\":%.1f,"
        "\"mean_cwnd_kb\":%.0f,\"max_cwnd_kb\":%.0f,\"queue_drops\":%llu,\"random_drops\":%llu,\"max_queue_ms\":%.1f}\n",
        p.name, p.rtt_ms, p.mbit, p.queue_ms, p.loss, p.jitter_ms, took, mb_s, steady, mb_s / payload_rate,
        steady / payload_rate, static_cast<unsigned long long>(st.packets),
        static_cast<unsigned long long>(st.retransmits), static_cast<unsigned long long>(st.rtos),
        static_cast<unsigned long long>(st.probes), static_cast<unsigned long long>(st.episodes), st.srtt_ms,
        cwnd_n ? cwnd_sum / cwnd_n / 1024 : 0, st.max_cwnd / 1024,
        static_cast<unsigned long long>(h.ca.queue_drops.load()), static_cast<unsigned long long>(h.ca.random_drops.load()),
        static_cast<double>(h.ca.max_queue_us.load()) / 1000.0);
    std::fflush(stdout);
    CHECK(got > 0);
  }
}
