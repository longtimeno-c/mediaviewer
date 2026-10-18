// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/splice.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mv::sharepoint::relay {

namespace {

using clock = std::chrono::steady_clock;
constexpr char kMagic[4] = {'M', 'V', 'R', 'L'};
constexpr std::size_t kSpliceBuffer = 64 * 1024;
constexpr int kPollMs = 250;
// Connections a server reads a hello from at once, beyond those waiting
// and those spliced; past it a new one is closed at once.
constexpr int kMaxHandshakes = 16;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
}

}  // namespace

token random_token() {
  (void)sodium_init();
  token t;
  randombytes_buf(t.data(), t.size());
  return t;
}

std::array<std::uint8_t, kHelloBytes> hello_bytes(const token& t) {
  std::array<std::uint8_t, kHelloBytes> out{};
  std::memcpy(out.data(), kMagic, 4);
  out[4] = kVersion;
  std::memcpy(out.data() + 8, t.data(), t.size());
  return out;
}

std::optional<token> parse_hello(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() != kHelloBytes || std::memcmp(bytes.data(), kMagic, 4) != 0 || bytes[4] != kVersion ||
      bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0) {
    return std::nullopt;
  }
  token t;
  std::memcpy(t.data(), bytes.data() + 8, t.size());
  return t;
}

splice_result splice(link& a, link& b, const limits& lim, const std::function<bool()>& stopped) {
  splice_result out;
  std::atomic<bool> over{false};
  std::atomic<bool> told{false};  // the first reason wins
  std::atomic<ended> why{ended::closed};
  std::atomic<std::int64_t> last_ns{now_ns()};
  const auto end_with = [&](ended e) {
    bool expected = false;
    if (told.compare_exchange_strong(expected, true)) why.store(e);
    over.store(true);
    a.shutdown();
    b.shutdown();
  };
  const auto pump = [&](link& from, link& to, bool a_to_b, std::uint64_t& moved) {
    std::vector<std::uint8_t> buf(kSpliceBuffer);
    const auto start = clock::now();
    while (!over.load()) {
      if (stopped && stopped()) return end_with(ended::stopped);
      auto n = from.read_some(buf, kPollMs);
      if (!n) {
        if (n.error() != status::timeout) return end_with(over.load() ? ended::closed : ended::failed);
        if (now_ns() - last_ns.load() > std::int64_t{lim.idle_ms} * 1'000'000) return end_with(ended::idle);
        continue;
      }
      if (*n == 0) return end_with(ended::closed);
      if (moved + *n > lim.max_bytes) return end_with(ended::capped);
      if (lim.rate > 0) {
        // Paced to `rate` from the start: a burst under it passes at once.
        const auto due = start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                                     static_cast<double>(moved + *n) / static_cast<double>(lim.rate)));
        while (clock::now() < due && !over.load() && !(stopped && stopped())) {
          std::this_thread::sleep_for(std::min<clock::duration>(due - clock::now(), std::chrono::milliseconds(100)));
        }
      }
      if (lim.tap) lim.tap(a_to_b, std::span<const std::uint8_t>(buf.data(), *n));
      if (!to.write_all(std::span<const std::uint8_t>(buf.data(), *n))) {
        return end_with(over.load() ? ended::closed : ended::failed);
      }
      moved += *n;
      last_ns.store(now_ns());
    }
  };
  std::thread back([&] { pump(b, a, false, out.b_to_a); });
  pump(a, b, true, out.a_to_b);
  back.join();
  out.why = why.load();
  return out;
}

result<std::unique_ptr<link>> join(std::unique_ptr<link> l, const token& t, int wait_ms,
                                   const std::function<bool()>& stopped) {
  if (!l) return err(status::invalid_arg);
  const auto hello = hello_bytes(t);
  if (auto w = l->write_all(hello); !w) return err(w.error());
  return await_pair(std::move(l), wait_ms, stopped);
}

result<std::unique_ptr<link>> await_pair(std::unique_ptr<link> l, int wait_ms, const std::function<bool()>& stopped) {
  if (!l) return err(status::invalid_arg);
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  std::uint8_t b = 0;
  while (clock::now() < deadline) {
    if (stopped && stopped()) {
      l->shutdown();
      return err(status::cancelled);
    }
    auto n = l->read_some({&b, 1}, kPollMs);
    if (!n) {
      if (n.error() == status::timeout) continue;
      return err(n.error());
    }
    if (*n == 0 || b != kPaired) {
      l->shutdown();
      return err(status::io);
    }
    return l;
  }
  l->shutdown();
  return err(status::timeout);
}

result<std::size_t> relayed_link::read_some(std::span<std::uint8_t> into, int timeout_ms) {
  if (at_ < first_.size()) {
    const std::size_t n = std::min(into.size(), first_.size() - at_);
    std::memcpy(into.data(), first_.data() + at_, n);
    at_ += n;
    if (at_ == first_.size()) {
      first_.clear();
      first_.shrink_to_fit();
      at_ = 0;
    }
    return n;
  }
  return inner_->read_some(into, timeout_ms);
}

result<std::unique_ptr<link>> first_bytes(std::unique_ptr<link> l, int wait_ms, const std::function<bool()>& stopped,
                                          const char* through) {
  std::vector<std::uint8_t> first(4096);
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  while (clock::now() < deadline) {
    if (stopped && stopped()) {
      l->shutdown();
      return err(status::cancelled);
    }
    auto n = l->read_some(first, kPollMs);
    if (!n) {
      if (n.error() == status::timeout) continue;
      return err(n.error());
    }
    if (*n == 0) return err(status::io);
    first.resize(*n);
    return std::unique_ptr<link>(std::make_unique<relayed_link>(std::move(l), std::move(first), through));
  }
  l->shutdown();
  return err(status::timeout);
}

// ---- the server -------------------------------------------------------------------------

struct server::waiter {
  std::shared_ptr<link> partner;  // under server::m_
  bool refused = false;           // under server::m_
};

result<std::unique_ptr<server>> server::start(options o) {
  auto l = o.loopback_only ? net::listen(true) : net::listen_internet(o.port, false);
  if (!l) return err(l.error());
  std::unique_ptr<server> s(new server(std::move(o)));
  s->listener_ = std::move(*l);
  server* raw = s.get();
  s->accept_thread_ = std::thread([raw] { raw->accept_loop(); });
  return s;
}

server::~server() {
  stop_.store(true);
  if (listener_) listener_->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<worker> all;
  {
    std::lock_guard lock(m_);
    all = std::move(workers_);
    for (auto& w : all) w.l->shutdown();
    cv_.notify_all();
  }
  for (auto& w : all) w.t.join();
}

std::uint16_t server::port() const noexcept { return listener_ ? listener_->port() : 0; }

server::counts server::snapshot() const {
  std::lock_guard lock(m_);
  counts c = counts_;
  c.waiting = waiting_.size();
  return c;
}

void server::reap_locked() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (it->done->load()) {
      it->t.join();  // returned already
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
}

void server::accept_loop() {
  while (!stop_.load()) {
    auto c = listener_->accept(500);
    if (!c) {
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    std::shared_ptr<link> l(std::move(*c));
    std::lock_guard lock(m_);
    reap_locked();
    if (static_cast<int>(workers_.size()) >= opts_.max_waiting + 2 * opts_.max_pairs + kMaxHandshakes) {
      ++counts_.refused;
      l->shutdown();
      continue;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back({std::thread([this, l, done] {
                          try {
                            handle(l);
                          } catch (...) {
                            l->shutdown();
                          }
                          done->store(true);
                        }),
                        done, l});
  }
}

void server::handle(std::shared_ptr<link> l) {
  std::array<std::uint8_t, kHelloBytes> hello{};
  const auto said = read_exact(*l, hello, kHelloMs);
  const auto t = said ? parse_hello(hello) : std::nullopt;
  std::shared_ptr<waiter> me;
  {
    std::lock_guard lock(m_);
    if (!t || stop_.load()) {
      ++counts_.refused;
      l->shutdown();
      return;
    }
    if (auto it = waiting_.find(*t); it != waiting_.end()) {
      // The second with this token: hand this link to the one waiting (its
      // thread runs the splice), unless the server is full.
      auto w = it->second;
      waiting_.erase(it);
      if (static_cast<int>(counts_.active) >= opts_.max_pairs) {
        w->refused = true;
        ++counts_.refused;
        l->shutdown();
      } else {
        w->partner = l;
        ++counts_.active;  // reserved now, so a burst cannot overrun max_pairs
      }
      cv_.notify_all();
      return;
    }
    if (static_cast<int>(waiting_.size()) >= opts_.max_waiting) {
      ++counts_.refused;
      l->shutdown();
      return;
    }
    me = std::make_shared<waiter>();
    waiting_.emplace(*t, me);
  }
  // Waiting: nothing may arrive before the partner (a byte, or a close,
  // ends the wait).
  const auto deadline = clock::now() + std::chrono::milliseconds(opts_.wait_ms);
  std::shared_ptr<link> partner;
  bool gone = false;
  for (;;) {
    {
      std::unique_lock lock(m_);
      if (me->partner || me->refused || stop_.load() || clock::now() >= deadline || gone) {
        partner = me->partner;
        if (!partner) {
          // Still listed only if nobody took it.
          if (auto it = waiting_.find(*t); it != waiting_.end() && it->second == me) waiting_.erase(it);
        } else if (gone || stop_.load()) {
          partner->shutdown();
          --counts_.active;
          partner.reset();
        }
        break;
      }
    }
    std::uint8_t b = 0;
    auto n = l->read_some({&b, 1}, 100);
    if (n || n.error() != status::timeout) gone = true;  // spoke first, or closed
  }
  if (!partner) {
    l->shutdown();
    return;
  }
  const std::uint8_t ok = kPaired;
  splice_result r;
  if (l->write_all({&ok, 1}) && partner->write_all({&ok, 1})) {
    {
      std::lock_guard lock(m_);
      ++counts_.paired;
    }
    r = splice(*l, *partner, opts_.lim, [this] { return stop_.load(); });
  }
  l->shutdown();
  partner->shutdown();
  std::lock_guard lock(m_);
  --counts_.active;
  counts_.bytes += r.a_to_b + r.b_to_a;
}

}  // namespace mv::sharepoint::relay
