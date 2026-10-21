// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #529: an emulated internet path for udp_hub's stream, in process
// (udp_hub::socket_wrap). Each wrapped socket's sends go through one
// direction of a path: a bottleneck of `mbit` with a drop-tail queue of
// `queue_ms` (what punishes an unpaced burst, as a home router does), then a
// one-way delay of rtt/2 with `jitter_ms` (uniform, in order), and a
// random loss. A thread per direction hands each datagram to the real socket
// at its arrival time. Seeded, so a run repeats.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include "addons/driftwire/net.h"

namespace mv::driftwire::test {

struct path_profile {
  const char* name = "";
  double rtt_ms = 0;
  double mbit = 0;        // the bottleneck, each way (0: none)
  double queue_ms = 0;    // its drop-tail queue, in time at the bottleneck rate
  double loss = 0;        // random loss, 0..1, each way
  double jitter_ms = 0;   // uniform 0..jitter added to the one-way delay
};

struct path_counters {
  std::atomic<std::uint64_t> sent{0}, queue_drops{0}, random_drops{0}, delivered{0};
  std::atomic<std::int64_t> max_queue_us{0};
};

class path_direction {
 public:
  path_direction(const path_profile& p, std::uint32_t seed, path_counters& c) : p_(p), c_(c), rng_(seed) {
    thread_ = std::thread([this] { run(); });
  }
  ~path_direction() {
    {
      std::lock_guard l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  // Returns at once; the datagram reaches `out` later (or never).
  void send(std::shared_ptr<net::udp_socket> out, std::span<const std::uint8_t> d, const net::endpoint& to) {
    using namespace std::chrono;
    const auto now = clock::now();
    std::lock_guard l(m_);
    ++c_.sent;
    if (p_.loss > 0 && uni_(rng_) < p_.loss) {
      ++c_.random_drops;
      return;
    }
    auto depart = now;
    if (p_.mbit > 0) {
      const auto backlog = link_free_ > now ? link_free_ - now : clock::duration::zero();
      const auto backlog_us = duration_cast<microseconds>(backlog).count();
      if (static_cast<double>(backlog_us) > p_.queue_ms * 1000.0) {
        ++c_.queue_drops;
        return;
      }
      c_.max_queue_us.store(std::max<std::int64_t>(c_.max_queue_us.load(), backlog_us));
      const double tx_s = static_cast<double>(d.size() + 28) * 8.0 / (p_.mbit * 1e6);  // + IP and UDP headers
      link_free_ = std::max(link_free_, now) + duration_cast<clock::duration>(duration<double>(tx_s));
      depart = link_free_;
    }
    double one_way = p_.rtt_ms / 2;
    if (p_.jitter_ms > 0) one_way += uni_(rng_) * p_.jitter_ms;
    // Jitter delays, it does not reorder: a link is a queue (Wi-Fi's
    // retries hold the packets behind them too).
    const auto at = std::max(last_at_, depart + duration_cast<clock::duration>(duration<double, std::milli>(one_way)));
    last_at_ = at;
    q_.push(item{at, seq_++, std::move(out), std::vector<std::uint8_t>(d.begin(), d.end()), to});
    cv_.notify_all();
  }

 private:
  using clock = std::chrono::steady_clock;
  struct item {
    clock::time_point at;
    std::uint64_t order;
    std::shared_ptr<net::udp_socket> out;
    std::vector<std::uint8_t> bytes;
    net::endpoint to;
    bool operator>(const item& o) const { return at != o.at ? at > o.at : order > o.order; }
  };

  void run() {
    std::unique_lock l(m_);
    while (!stop_) {
      if (q_.empty()) {
        cv_.wait(l);
        continue;
      }
      const auto at = q_.top().at;
      if (clock::now() < at) {
        cv_.wait_until(l, at);
        continue;
      }
      item it = std::move(const_cast<item&>(q_.top()));
      q_.pop();
      l.unlock();
      (void)it.out->send_to(it.bytes, it.to);
      ++c_.delivered;
      l.lock();
    }
  }

  path_profile p_;
  path_counters& c_;
  std::mt19937 rng_;
  std::uniform_real_distribution<double> uni_{0.0, 1.0};
  std::mutex m_;
  std::condition_variable cv_;
  std::priority_queue<item, std::vector<item>, std::greater<>> q_;
  clock::time_point link_free_{};
  clock::time_point last_at_{};
  std::uint64_t seq_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

// A socket whose sends go through `dir`; receives are the real socket's.
class path_socket final : public net::udp_socket {
 public:
  path_socket(std::shared_ptr<path_direction> dir, std::unique_ptr<net::udp_socket> in)
      : dir_(std::move(dir)), in_(std::move(in)) {}
  expected send_to(std::span<const std::uint8_t> d, const net::endpoint& to) override {
    dir_->send(in_, d, to);
    return {};
  }
  result<std::size_t> receive_from(std::span<std::uint8_t> into, int timeout_ms, net::endpoint& from) override {
    return in_->receive_from(into, timeout_ms, from);
  }
  void shutdown() noexcept override { in_->shutdown(); }
  std::uint16_t port() const noexcept override { return in_->port(); }
  std::intptr_t handle() const noexcept override { return in_->handle(); }
  void set_buffers(int bytes) noexcept override { in_->set_buffers(bytes); }

 private:
  std::shared_ptr<path_direction> dir_;
  std::shared_ptr<net::udp_socket> in_;
};

}  // namespace mv::driftwire::test
