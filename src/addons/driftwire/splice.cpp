// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/splice.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mv::driftwire::relay {

namespace {

using clock = std::chrono::steady_clock;
constexpr char kMagic[4] = {'M', 'V', 'R', 'L'};
constexpr std::size_t kSpliceBuffer = 64 * 1024;
constexpr int kPollMs = 250;

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

std::string random_key() {
  (void)sodium_init();
  static constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford
  std::array<std::uint8_t, 17> b{};
  randombytes_buf(b.data(), b.size());
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const std::uint8_t x : b) {
    acc = acc << 8 | x;
    bits += 8;
    while (bits >= 5 && out.size() < 26) {
      out.push_back(kAlphabet[(acc >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  sodium_memzero(b.data(), b.size());
  return out;
}

proof prove(const std::string& key, const token& t) {
  (void)sodium_init();
  std::uint8_t k[32];
  crypto_generichash(k, sizeof k, reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), nullptr, 0);
  static constexpr char kContext[] = "MediaViewer Driftwire relay waiter v2";
  std::uint8_t msg[sizeof kContext - 1 + kTokenBytes];
  std::memcpy(msg, kContext, sizeof kContext - 1);
  std::memcpy(msg + sizeof kContext - 1, t.data(), t.size());
  proof p;
  crypto_generichash(p.data(), p.size(), msg, sizeof msg, k, sizeof k);
  sodium_memzero(k, sizeof k);
  return p;
}

namespace {
void put_head(std::uint8_t* out, std::uint8_t role, const token& t) {
  std::memcpy(out, kMagic, 4);
  out[4] = kVersion;
  out[5] = role;
  out[6] = out[7] = 0;
  std::memcpy(out + 8, t.data(), t.size());
}
}  // namespace

std::array<std::uint8_t, kHelloBytes> sender_hello(const token& t) {
  std::array<std::uint8_t, kHelloBytes> out{};
  put_head(out.data(), kSender, t);
  return out;
}

std::array<std::uint8_t, kWaiterHelloBytes> waiter_hello(const token& t, const std::string& key) {
  std::array<std::uint8_t, kWaiterHelloBytes> out{};
  put_head(out.data(), kWaiter, t);
  const auto p = prove(key, t);
  std::memcpy(out.data() + kHelloBytes, p.data(), p.size());
  return out;
}

std::optional<hello> parse_hello(std::span<const std::uint8_t> head) noexcept {
  if (head.size() != kHelloBytes || std::memcmp(head.data(), kMagic, 4) != 0 || head[4] != kVersion ||
      (head[5] != kSender && head[5] != kWaiter) || head[6] != 0 || head[7] != 0) {
    return std::nullopt;
  }
  hello h;
  h.role = head[5];
  std::memcpy(h.t.data(), head.data() + 8, h.t.size());
  return h;
}

splice_result splice(link& a, link& b, const limits& lim, const std::function<bool()>& stopped) noexcept {
  splice_result out;
  std::atomic<bool> over{false};
  std::atomic<bool> told{false};  // the first reason wins
  std::atomic<ended> why{ended::closed};
  std::atomic<std::int64_t> last_ns{now_ns()};
  const auto end_with = [&](ended e) noexcept {
    bool expected = false;
    if (told.compare_exchange_strong(expected, true)) why.store(e);
    over.store(true);
    a.shutdown();
    b.shutdown();
  };
  const auto pump = [&](link& from, link& to, bool a_to_b, std::uint64_t& moved) noexcept {
    try {
      std::vector<std::uint8_t> buf(kSpliceBuffer);
      // A token bucket: `rate` a second, at most kBurstSeconds of it saved up.
      const double rate = static_cast<double>(lim.rate);
      const double burst = std::max(rate * kBurstSeconds, static_cast<double>(kSpliceBuffer));
      double tokens = burst;
      auto refilled = clock::now();
      const auto refill = [&] {
        const auto now = clock::now();
        tokens = std::min(burst, tokens + rate * std::chrono::duration<double>(now - refilled).count());
        refilled = now;
      };
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
          refill();
          tokens -= static_cast<double>(*n);
          while (tokens < 0 && !over.load() && !(stopped && stopped())) {
            std::this_thread::sleep_for(std::min(std::chrono::duration<double>(-tokens / rate),
                                                 std::chrono::duration<double>(0.1)));
            refill();
          }
        }
        if (lim.tap) lim.tap(a_to_b, std::span<const std::uint8_t>(buf.data(), *n));
        if (!to.write_all(std::span<const std::uint8_t>(buf.data(), *n))) {
          return end_with(over.load() ? ended::closed : ended::failed);
        }
        moved += *n;
        last_ns.store(now_ns());
      }
    } catch (...) {
      end_with(ended::failed);
    }
  };
  std::thread back;
  try {
    back = std::thread([&] { pump(b, a, false, out.b_to_a); });
  } catch (...) {
    end_with(ended::failed);
    out.why = why.load();
    return out;
  }
  pump(a, b, true, out.a_to_b);
  back.join();
  out.why = why.load();
  return out;
}

result<std::unique_ptr<link>> join(std::unique_ptr<link> l, const token& t, int wait_ms,
                                   const std::function<bool()>& stopped) {
  if (!l) return err(status::invalid_arg);
  const auto hello = sender_hello(t);
  if (auto w = l->write_all(hello); !w) return err(w.error());
  if (auto r = wait_paired(*l, wait_ms, stopped); !r) {
    l->shutdown();
    return err(r.error());
  }
  return l;
}

expected register_waiter(link& l, const token& t, const std::string& key) {
  const auto hello = waiter_hello(t, key);
  return l.write_all(hello);
}

expected wait_paired(link& l, int wait_ms, const std::function<bool()>& stopped) {
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  std::uint8_t b = 0;
  while (clock::now() < deadline) {
    if (stopped && stopped()) return err(status::cancelled);
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
    auto n = l.read_some({&b, 1}, static_cast<int>(std::clamp<long long>(left, 1, kPollMs)));
    if (!n) {
      if (n.error() == status::timeout) continue;
      return err(n.error());
    }
    if (*n == 0 || b != kPaired) return err(status::io);
    return {};
  }
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
  bool replaced = false;          // under server::m_
};

result<std::unique_ptr<server>> server::start(options o) {
  if (o.key.size() < kMinKeyChars) return err(status::invalid_arg);
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
    net::address from;
    auto c = listener_->accept_from(500, from);
    if (!c) {
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    std::shared_ptr<link> l(std::move(*c));
    std::lock_guard lock(m_);
    reap_locked();
    const bool full = static_cast<int>(workers_.size()) >= opts_.max_waiting + 2 * opts_.max_pairs + kMaxHandshakes;
    if (full || handshaking_ >= kMaxHandshakes || handshaking_from_[from] >= kPerSource) {
      ++counts_.refused;
      if (handshaking_from_[from] == 0) handshaking_from_.erase(from);
      l->shutdown();
      continue;
    }
    ++handshaking_;
    ++handshaking_from_[from];
    auto done = std::make_shared<std::atomic<bool>>(false);
    try {
      workers_.push_back({std::thread([this, l, done, from] {
                            try {
                              handle(l, from);
                            } catch (...) {
                              l->shutdown();
                            }
                            done->store(true);
                          }),
                          done, l});
    } catch (...) {
      // No thread: its handshake never starts.
      --handshaking_;
      if (--handshaking_from_[from] <= 0) handshaking_from_.erase(from);
      l->shutdown();
    }
  }
}

void server::handle(std::shared_ptr<link> l, const net::address& from) {
  // The handshake's place in the caps, given back however this ends.
  struct handshake_slot {
    server& s;
    net::address from;
    bool held = true;
    void release() {
      if (!held) return;
      held = false;
      std::lock_guard lock(s.m_);
      --s.handshaking_;
      if (--s.handshaking_from_[from] <= 0) s.handshaking_from_.erase(from);
    }
    ~handshake_slot() { release(); }
  } slot{*this, from};
  std::array<std::uint8_t, kWaiterHelloBytes> bytes{};
  const auto deadline = clock::now() + std::chrono::milliseconds(kHelloMs);
  auto h = read_exact_until(*l, std::span(bytes).first(kHelloBytes), deadline)
               ? parse_hello(std::span<const std::uint8_t>(bytes).first(kHelloBytes))
               : std::nullopt;
  bool proven = false;
  if (h && h->role == kWaiter && read_exact_until(*l, std::span(bytes).subspan(kHelloBytes), deadline)) {
    const auto want = prove(opts_.key, h->t);
    proven = sodium_memcmp(want.data(), bytes.data() + kHelloBytes, kProofBytes) == 0;
  }
  slot.release();
  if (!h || (h->role == kWaiter && !proven) || stop_.load()) {
    std::lock_guard lock(m_);
    ++counts_.refused;
    l->shutdown();
    return;
  }
  if (h->role == kSender) {
    // Paired with the waiter for this token, or closed: a sender never waits.
    std::lock_guard lock(m_);
    const auto it = waiting_.find(h->t);
    if (it == waiting_.end() || static_cast<int>(counts_.active) >= opts_.max_pairs) {
      ++counts_.refused;
      l->shutdown();
      return;
    }
    it->second->partner = l;
    ++counts_.active;  // reserved now (the waiter's thread gives it back), so a burst cannot overrun max_pairs
    waiting_.erase(it);
    cv_.notify_all();
    return;
  }
  serve_waiter(std::move(l), h->t, from);
}

void server::serve_waiter(std::shared_ptr<link> l, const token& t, const net::address& from) {
  auto me = std::make_shared<waiter>();
  {
    std::lock_guard lock(m_);
    const auto it = waiting_.find(t);
    const bool replacing = it != waiting_.end();
    if ((!replacing && static_cast<int>(waiting_.size()) >= opts_.max_waiting) ||
        waiting_from_[from] >= kPerSource + (replacing ? 1 : 0)) {
      if (waiting_from_[from] == 0) waiting_from_.erase(from);
      ++counts_.refused;
      l->shutdown();
      return;
    }
    if (replacing) {
      it->second->replaced = true;  // its thread closes it
      it->second = me;
    } else {
      waiting_.emplace(t, me);
    }
    ++waiting_from_[from];
  }
  // Given back however this ends: the place from this source, the token's
  // entry if still ours, and the pair's reservation.
  struct waiter_guard {
    server& s;
    const token& t;
    const net::address& from;
    std::shared_ptr<waiter>& me;
    std::shared_ptr<link> partner;
    ~waiter_guard() {
      std::lock_guard lock(s.m_);
      if (--s.waiting_from_[from] <= 0) s.waiting_from_.erase(from);
      if (auto it = s.waiting_.find(t); it != s.waiting_.end() && it->second == me) s.waiting_.erase(it);
      if (partner) {
        partner->shutdown();
        --s.counts_.active;
      }
    }
  } guard{*this, t, from, me, nullptr};
  // Waiting: nothing may arrive before the partner (a byte, or a close,
  // ends the wait).
  const auto deadline = clock::now() + std::chrono::milliseconds(opts_.wait_ms);
  bool gone = false;
  for (;;) {
    {
      std::lock_guard lock(m_);
      guard.partner = me->partner;
      if (guard.partner || me->replaced || stop_.load() || clock::now() >= deadline || gone) break;
    }
    std::uint8_t b = 0;
    auto n = l->read_some({&b, 1}, 100);
    if (n || n.error() != status::timeout) gone = true;  // spoke first, or closed
  }
  if (!guard.partner || gone || stop_.load()) {
    l->shutdown();
    return;
  }
  const std::uint8_t ok = kPaired;
  splice_result r;
  if (l->write_all({&ok, 1}) && guard.partner->write_all({&ok, 1})) {
    {
      std::lock_guard lock(m_);
      ++counts_.paired;
    }
    r = splice(*l, *guard.partner, opts_.lim, [this] { return stop_.load(); });
  }
  l->shutdown();
  std::lock_guard lock(m_);
  counts_.bytes += r.a_to_b + r.b_to_a;
}

}  // namespace mv::driftwire::relay
