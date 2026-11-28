// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A long, lossy internet path for a TCP link, in-process (the phone's library copy bench). Loopback
// TCP never loses a packet and the tests have no root for dummynet, so the service's side of each
// connection is wrapped: what it writes reaches the phone as a TCP sender's would over the path.
//
// The model, per connection, server to phone: a CUBIC sender (RFC 9438: beta 0.7, C 0.4, the
// Reno-friendly region, slow start from 10 segments) moving 1448-byte segments in rounds of one
// RTT, each segment lost with probability `loss`. A round that loses one reduces the window once,
// and everything sent after the lost segment arrives one RTT later (it waits for the
// retransmission: in order, as TCP hands it over). The window never exceeds a 4 MB receive window
// (iOS's autotuning ceiling) and grows only when the round used at least half of it. Optional
// bottleneck shared by every link on it: a queue past `queue_ms` counts as a loss. Phone to server
// (requests, credit, acks: small) is delayed by half the RTT, never lost. Sleeps: benches only.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "wire/link.h"

namespace mv::driftwire::test {

struct tcp_profile {
  const char* name = "";
  double rtt_ms = 150;
  double loss = 0.001;    // per segment, server to phone
  double mbit = 0;        // a bottleneck shared by the links on it; 0: none
  double queue_ms = 60;   // its queue, in time at that rate
};

class tcp_bottleneck {
 public:
  explicit tcp_bottleneck(const tcp_profile& p) : p_(p) {}
  // When `n` bytes offered at `now` leave the bottleneck, and whether its queue dropped them.
  std::chrono::steady_clock::time_point pass(std::size_t n, std::chrono::steady_clock::time_point now, bool& dropped) {
    using namespace std::chrono;
    dropped = false;
    if (p_.mbit <= 0) return now;
    std::lock_guard l(m_);
    if (free_at_ < now) free_at_ = now;
    if (duration<double, std::milli>(free_at_ - now).count() > p_.queue_ms) dropped = true;
    free_at_ += duration_cast<steady_clock::duration>(duration<double>(static_cast<double>(n) * 8 / (p_.mbit * 1e6)));
    return free_at_;
  }

 private:
  tcp_profile p_;
  std::mutex m_;
  std::chrono::steady_clock::time_point free_at_{};
};

class tcp_path_link final : public link {
 public:
  using clock = std::chrono::steady_clock;
  static constexpr std::size_t kMss = 1448;
  static constexpr double kMaxWindow = 4.0 * 1024 * 1024 / kMss;

  tcp_path_link(std::unique_ptr<link> inner, const tcp_profile& p, std::uint32_t seed, std::shared_ptr<tcp_bottleneck> bn)
      : inner_(std::move(inner)), p_(p), rng_(seed), bn_(std::move(bn)) {
    rtt_ = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(p_.rtt_ms));
    send_ = std::thread([this] { send_loop(); });
    deliver_ = std::thread([this] { deliver_loop(); });
    up_ = std::thread([this] { up_loop(); });
  }
  ~tcp_path_link() override {
    shutdown();
    send_.join();
    deliver_.join();
    up_.join();
  }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::unique_lock l(m_);
    // A socket's send buffer: the writer waits while it is full.
    const auto cap = [&] { return std::max<std::size_t>(512 * 1024, static_cast<std::size_t>(2 * cwnd_ * kMss)); };
    cv_.wait(l, [&] { return stop_ || failed_ || pending_.size() < cap(); });
    if (stop_ || failed_) return err(status::io);
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    cv_.notify_all();
    return {};
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    std::unique_lock l(m_);
    const auto deadline = timeout_ms < 0 ? clock::time_point::max() : clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (!in_.empty() && in_.front().at <= clock::now()) {
        auto& f = in_.front();
        const std::size_t n = std::min(into.size(), f.bytes.size() - f.used);
        std::memcpy(into.data(), f.bytes.data() + f.used, n);
        f.used += n;
        if (f.used == f.bytes.size()) in_.pop_front();
        return n;
      }
      if (in_.empty() && up_closed_) return std::size_t{0};
      if (stop_) return err(status::cancelled);
      const auto wake = in_.empty() ? deadline : std::min(deadline, in_.front().at);
      if (wake == clock::time_point::max()) {
        cv_.wait(l);
      } else if (cv_.wait_until(l, wake) == std::cv_status::timeout && clock::now() >= deadline) {
        if (!in_.empty() && in_.front().at <= clock::now()) continue;
        return err(status::timeout);
      }
    }
  }

  void shutdown() noexcept override {
    {
      std::lock_guard l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    inner_->shutdown();
  }
  [[nodiscard]] transport via() const noexcept override { return inner_->via(); }

 private:
  struct timed {
    clock::time_point at;
    std::vector<std::uint8_t> bytes;
    std::size_t used = 0;
  };

  // Rounds of one RTT: what the window allows leaves now; the window changes at each round's end.
  void send_loop() {
    std::unique_lock l(m_);
    auto round_start = clock::now();
    double budget = cwnd_ * kMss, sent = 0;
    bool lost = false;
    double w_max = 0;
    auto epoch = clock::now();
    const double beta = 0.7, c = 0.4, rtt_s = p_.rtt_ms / 1000;
    std::uniform_real_distribution<double> u(0, 1);
    while (!stop_) {
      const auto round_end = round_start + rtt_;
      if (pending_.empty() || budget < 1) cv_.wait_until(l, round_end, [&] { return stop_ || (!pending_.empty() && budget >= 1); });
      if (stop_) break;
      const auto now = clock::now();
      if (now >= round_end) {
        if (lost) {
          w_max = cwnd_;
          cwnd_ = std::max(2.0, cwnd_ * beta);
          ssthresh_ = cwnd_;
          epoch = now;
        } else if (sent >= cwnd_ * kMss / 2) {
          if (cwnd_ < ssthresh_) {
            cwnd_ *= 2;
          } else {
            const double t = std::chrono::duration<double>(now - epoch).count() + rtt_s;
            const double k = std::cbrt(w_max * (1 - beta) / c);
            const double cubic = c * std::pow(t - k, 3) + w_max;
            const double reno = w_max * beta + 3 * (1 - beta) / (1 + beta) * (t / rtt_s);
            cwnd_ = std::min(cwnd_ * 1.5, std::max({cubic, reno, cwnd_ + 1}));
          }
        }
        cwnd_ = std::min(cwnd_, kMaxWindow);
        round_start = now;
        budget = cwnd_ * kMss;
        sent = 0;
        lost = false;
        cv_.notify_all();  // the send buffer's size follows the window
        continue;
      }
      if (pending_.empty() || budget < 1) continue;
      const std::size_t n = std::min(pending_.size(), static_cast<std::size_t>(budget));
      std::vector<std::uint8_t> bytes(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
      budget -= static_cast<double>(n);
      sent += static_cast<double>(n);
      cv_.notify_all();
      // The first lost segment of these, if any.
      const std::size_t segs = (n + kMss - 1) / kMss;
      std::size_t first_lost = segs;
      if (p_.loss > 0) {
        for (std::size_t s = 0; s < segs; ++s) {
          if (u(rng_) < p_.loss) {
            first_lost = s;
            break;
          }
        }
      }
      bool queue_drop = false;
      const auto left_bottleneck = bn_ ? bn_->pass(n, now, queue_drop) : now;
      if (queue_drop && first_lost == segs) first_lost = segs - 1;
      const auto arrive = left_bottleneck + rtt_ / 2;
      const std::size_t clean = std::min(n, first_lost * kMss);
      if (clean > 0) queue_out(arrive, std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(clean)));
      if (clean < n) {
        lost = true;
        queue_out(arrive + rtt_, std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(clean), bytes.end()));
      }
    }
  }

  // m_ held. In order: nothing overtakes what waits for a retransmission.
  void queue_out(clock::time_point at, std::vector<std::uint8_t> bytes) {
    if (!out_.empty() && out_.back().at > at) at = out_.back().at;
    out_.push_back({at, std::move(bytes)});
    cv_.notify_all();
  }

  void deliver_loop() {
    std::unique_lock l(m_);
    while (!stop_) {
      if (out_.empty()) {
        cv_.wait(l);
        continue;
      }
      if (out_.front().at > clock::now()) {
        cv_.wait_until(l, out_.front().at);
        continue;
      }
      auto bytes = std::move(out_.front().bytes);
      out_.pop_front();
      l.unlock();
      const bool ok = inner_->write_all(bytes).has_value();
      l.lock();
      if (!ok) {
        failed_ = true;
        cv_.notify_all();
        break;
      }
    }
  }

  void up_loop() {
    std::vector<std::uint8_t> buf(64 * 1024);
    for (;;) {
      auto n = inner_->read_some(buf, 200);
      std::lock_guard l(m_);
      if (stop_) break;
      if (!n) {
        if (n.error() == status::timeout) continue;
        up_closed_ = true;
        cv_.notify_all();
        break;
      }
      if (*n == 0) {
        up_closed_ = true;
        cv_.notify_all();
        break;
      }
      in_.push_back({clock::now() + rtt_ / 2, std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n))});
      cv_.notify_all();
    }
  }

  std::unique_ptr<link> inner_;
  tcp_profile p_;
  std::mt19937 rng_;
  std::shared_ptr<tcp_bottleneck> bn_;
  clock::duration rtt_{};
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::uint8_t> pending_;  // written, not yet sent
  std::deque<timed> out_;             // sent, arriving at .at
  std::deque<timed> in_;              // from the phone, readable at .at
  double cwnd_ = 10, ssthresh_ = std::numeric_limits<double>::infinity();
  bool stop_ = false, failed_ = false, up_closed_ = false;
  std::thread send_, deliver_, up_;
};

}  // namespace mv::driftwire::test
