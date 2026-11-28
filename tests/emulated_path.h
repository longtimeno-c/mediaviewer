// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A phone's path for the benches: a link with a delay each way and a rate
// each way that every connection over the path shares (path_link), and a TCP
// proxy on loopback that puts each connection through to `to` over it
// (path_proxy). Loopback, so the machine's own load shows.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wire/link.h"
#include "wire/net.h"

namespace mv::test {

using path_clock = std::chrono::steady_clock;

// The phone's one path: a rate each way that every connection over it shares.
struct bottleneck {
  std::mutex m;
  path_clock::time_point up{}, down{};  // when each direction is next free
};

// A link over the emulated path: `one_way_ms` each way, and `bytes_per_s`
// each way (0: no cap) shared through `b`, with up to 256 KB queued per link each way.
class path_link final : public mv::driftwire::link {
 public:
  path_link(std::unique_ptr<mv::driftwire::link> inner, int one_way_ms, double bytes_per_s, std::shared_ptr<bottleneck> b)
      : inner_(std::move(inner)), delay_(std::chrono::milliseconds(one_way_ms)), rate_(bytes_per_s), b_(std::move(b)) {
    tx_ = std::thread([this] { send_loop(); });
    rx_ = std::thread([this] { receive_loop(); });
  }
  ~path_link() override {
    shutdown();
    tx_.join();
    rx_.join();
  }
  mv::expected write_all(std::span<const std::uint8_t> b) override {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return over_ || queued_ < kQueue; });
    if (over_) return mv::err(mv::status::io);
    out_.push_back({due(true, b.size()), {b.begin(), b.end()}});
    queued_ += b.size();
    cv_.notify_all();
    return {};
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    const auto deadline = path_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(m_);
    for (;;) {
      if (!in_.empty() && in_.front().at <= path_clock::now()) {
        auto& c = in_.front();
        const std::size_t n = std::min(into.size(), c.bytes.size() - c.used);
        std::copy_n(c.bytes.begin() + static_cast<std::ptrdiff_t>(c.used), n, into.begin());
        c.used += n;
        if (c.used == c.bytes.size()) {
          in_queued_ -= c.bytes.size();
          in_.pop_front();
          cv_.notify_all();
        }
        return n;
      }
      if (in_.empty() && in_ended_) return std::size_t{0};
      if (over_ && in_.empty()) return mv::err(mv::status::io);
      const auto wake = in_.empty() ? deadline : std::min(deadline, in_.front().at);
      if (path_clock::now() >= deadline) return mv::err(mv::status::timeout);
      cv_.wait_until(lock, wake);
    }
  }
  void shutdown() noexcept override {
    {
      std::lock_guard lock(m_);
      over_ = true;
      cv_.notify_all();
    }
    inner_->shutdown();
  }
  [[nodiscard]] mv::driftwire::transport via() const noexcept override { return inner_->via(); }

 private:
  static constexpr std::size_t kQueue = 256 * 1024;
  struct chunk {
    path_clock::time_point at;
    std::vector<std::uint8_t> bytes;
    std::size_t used = 0;
  };
  // When `n` bytes through a bottleneck free at `free` arrive at the far end.
  path_clock::time_point due(bool up, std::size_t n) {
    std::lock_guard lock(b_->m);
    auto& free = up ? b_->up : b_->down;
    const auto now = path_clock::now();
    if (free < now) free = now;
    if (rate_ > 0) {
      free += std::chrono::duration_cast<path_clock::duration>(std::chrono::duration<double>(n / rate_));
    }
    return free + delay_;
  }
  void send_loop() {
    std::unique_lock lock(m_);
    for (;;) {
      cv_.wait(lock, [&] { return over_ || !out_.empty(); });
      if (over_) return;
      const auto at = out_.front().at;
      if (at > path_clock::now()) {
        cv_.wait_until(lock, at, [&] { return over_.load(); });
        continue;
      }
      auto c = std::move(out_.front());
      out_.pop_front();
      queued_ -= c.bytes.size();
      cv_.notify_all();
      lock.unlock();
      const bool ok = static_cast<bool>(inner_->write_all(c.bytes));
      lock.lock();
      if (!ok) {
        over_ = true;
        cv_.notify_all();
        return;
      }
    }
  }
  void receive_loop() {
    std::vector<std::uint8_t> buf(65536);
    for (;;) {
      if (over_.load()) return;
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return over_ || in_queued_ < kQueue; });
      }
      auto n = inner_->read_some(buf, 100);
      if (!n && n.error() == mv::status::timeout) continue;
      std::lock_guard lock(m_);
      if (n && *n) in_queued_ += *n;
      if (!n || *n == 0) {
        in_ended_ = true;
        cv_.notify_all();
        return;
      }
      in_.push_back({due(false, *n), {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)}});
      cv_.notify_all();
    }
  }

  std::unique_ptr<mv::driftwire::link> inner_;
  path_clock::duration delay_;
  double rate_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<chunk> out_, in_;  // under m_
  std::size_t queued_ = 0, in_queued_ = 0;  // under m_
  std::shared_ptr<bottleneck> b_;
  std::atomic<bool> over_{false};
  bool in_ended_ = false;
  std::thread tx_, rx_;
};

// Accepts on loopback and puts each connection through to `to` over a path
// of `rtt_ms` and `mbps` (0: no cap), every connection sharing it.
class path_proxy {
 public:
  path_proxy(mv::driftwire::net::endpoint to, int rtt_ms, double mbps)
      : to_(std::move(to)), rtt_(rtt_ms), rate_(mbps * 1e6 / 8), path_(std::make_shared<bottleneck>()) {
    auto l = mv::driftwire::net::listen(true);
    if (l) l_ = std::move(*l);
    if (l_) t_ = std::thread([this] { accept_loop(); });
  }
  ~path_proxy() {
    stop_.store(true);
    if (l_) l_->shutdown();
    if (t_.joinable()) t_.join();
    std::vector<std::thread> all;
    {
      std::lock_guard lock(m_);
      for (auto& l : links_) l->shutdown();
      all = std::move(conns_);
    }
    for (auto& c : all) c.join();
  }
  [[nodiscard]] bool ok() const { return static_cast<bool>(l_); }
  [[nodiscard]] std::uint16_t port() const { return l_->port(); }

 private:
  void accept_loop() {
    while (!stop_.load()) {
      auto c = l_->accept(200);
      if (!c) {
        if (c.error() == mv::status::cancelled) return;
        continue;
      }
      std::shared_ptr<mv::driftwire::link> near(std::move(*c));
      std::lock_guard lock(m_);
      links_.push_back(near);
      conns_.emplace_back([this, near] { carry(near); });
    }
  }
  void carry(std::shared_ptr<mv::driftwire::link> near) {
    // TCP's own handshake, a round trip.
    std::this_thread::sleep_for(std::chrono::milliseconds(rtt_));
    auto c = mv::driftwire::net::connect(to_, 5000);
    if (!c) return near->shutdown();
    std::shared_ptr<mv::driftwire::link> far = std::make_shared<path_link>(std::move(*c), rtt_ / 2, rate_, path_);
    {
      std::lock_guard lock(m_);
      links_.push_back(far);
    }
    const auto pump = [this](mv::driftwire::link& from, mv::driftwire::link& to) {
      std::vector<std::uint8_t> buf(65536);
      while (!stop_.load()) {
        auto n = from.read_some(buf, 200);
        if (!n && n.error() == mv::status::timeout) continue;
        if (!n || *n == 0) break;
        if (!to.write_all(std::span<const std::uint8_t>(buf.data(), *n))) break;
      }
      from.shutdown();
      to.shutdown();
    };
    std::thread back([&] { pump(*far, *near); });
    pump(*near, *far);
    back.join();
  }

  mv::driftwire::net::endpoint to_;
  int rtt_;
  double rate_;
  std::shared_ptr<bottleneck> path_;
  std::unique_ptr<mv::driftwire::net::listener> l_;
  std::thread t_;
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::vector<std::shared_ptr<mv::driftwire::link>> links_;  // under m_
  std::vector<std::thread> conns_;                           // under m_
};

}  // namespace mv::test
