// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/splice.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "addons/driftwire/cpace.h"

namespace mv::driftwire::relay {

namespace {

using clock = std::chrono::steady_clock;
constexpr char kMagic[4] = {'M', 'V', 'R', 'L'};
constexpr std::size_t kSpliceBuffer = 64 * 1024;
constexpr int kPollMs = 250;
constexpr char kPrologue[] = "MediaViewer Driftwire relay 3";
constexpr char kCpaceChannel[] = "MediaViewer Driftwire relay 3 waiter-server";
constexpr char kIdContext[] = "MediaViewer Driftwire relay token id v3";
constexpr char kSenderPskContext[] = "MediaViewer Driftwire relay sender psk v3";
// A handshake message here is an ephemeral key, a token and a tag at most.
constexpr std::size_t kMaxNoiseBytes = noise::kDhBytes + kTokenBytes + noise::kTagBytes;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
}

}  // namespace

token random_token() {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  token t;
  randombytes_buf(t.data(), t.size());
  return t;
}

std::string random_key() {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
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

token_id_bytes token_id(const token& t) {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  token_id_bytes id;
  crypto_generichash(id.data(), id.size(), reinterpret_cast<const std::uint8_t*>(kIdContext), sizeof kIdContext - 1,
                     t.data(), t.size());
  return id;
}

std::optional<std::uint8_t> parse_head(std::span<const std::uint8_t> head) noexcept {
  if (head.size() != kHeadBytes || std::memcmp(head.data(), kMagic, 4) != 0 || head[4] != kVersion ||
      (head[5] != kSender && head[5] != kWaiter) || head[6] != 0 || head[7] != 0) {
    return std::nullopt;
  }
  return head[5];
}

namespace {

std::array<std::uint8_t, kHeadBytes> make_head(std::uint8_t role) {
  return {static_cast<std::uint8_t>(kMagic[0]), static_cast<std::uint8_t>(kMagic[1]),
          static_cast<std::uint8_t>(kMagic[2]), static_cast<std::uint8_t>(kMagic[3]), kVersion, role, 0, 0};
}

std::array<std::uint8_t, 32> sender_psk(const token& t) {
  std::array<std::uint8_t, 32> k;
  crypto_generichash(k.data(), k.size(), reinterpret_cast<const std::uint8_t*>(kSenderPskContext),
                     sizeof kSenderPskContext - 1, t.data(), t.size());
  return k;
}

// Everything said in the clear before the handshake, in its prologue.
std::vector<std::uint8_t> prologue_of(std::span<const std::uint8_t> clear) {
  std::vector<std::uint8_t> p(kPrologue, kPrologue + sizeof kPrologue - 1);
  p.insert(p.end(), clear.begin(), clear.end());
  return p;
}

// One handshake message on the link: its length (2 bytes) and itself.
expected write_frame(link& l, const std::vector<std::uint8_t>& m) {
  std::vector<std::uint8_t> out;
  out.reserve(2 + m.size());
  out.push_back(static_cast<std::uint8_t>(m.size()));
  out.push_back(static_cast<std::uint8_t>(m.size() >> 8));
  out.insert(out.end(), m.begin(), m.end());
  return l.write_all(out);
}

result<std::vector<std::uint8_t>> read_frame(link& l, clock::time_point deadline) {
  std::uint8_t len[2];
  if (auto r = read_exact_until(l, len, deadline); !r) return err(r.error());
  const std::size_t n = static_cast<std::size_t>(len[0]) | static_cast<std::size_t>(len[1]) << 8;
  if (n < noise::kDhBytes || n > kMaxNoiseBytes) return err(status::corrupt);
  std::vector<std::uint8_t> m(n);
  if (auto r = read_exact_until(l, m, deadline); !r) return err(r.error());
  return m;
}

expected write_word(link& l, noise::cipher_state& to, std::uint8_t word) {
  std::vector<std::uint8_t> sealed;
  if (auto r = to.encrypt({}, {&word, 1}, sealed); !r) return r;
  return l.write_all(sealed);
}

// The server's next word, read in part across calls (`buf`, `got`) until
// `deadline`: status::timeout while it has not all come.
result<std::uint8_t> read_word(link& l, noise::cipher_state& from, std::array<std::uint8_t, kWordBytes>& buf,
                               std::size_t& got, clock::time_point deadline, const std::function<bool()>& stopped) {
  while (got < buf.size()) {
    if (stopped && stopped()) return err(status::cancelled);
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
    if (left <= 0) return err(status::timeout);
    auto n = l.read_some(std::span(buf).subspan(got), static_cast<int>(std::min<long long>(left, kPollMs)));
    if (!n) {
      if (n.error() == status::timeout) continue;
      return err(n.error());
    }
    if (*n == 0) return err(status::io);
    got += *n;
  }
  got = 0;
  std::vector<std::uint8_t> plain;
  if (!from.decrypt({}, buf, plain) || plain.size() != 1) return err(status::corrupt);
  return plain[0];
}

}  // namespace

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
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  const auto fail = [&](status s) -> result<std::unique_ptr<link>> {
    l->shutdown();
    return err(s);
  };
  const auto head = make_head(kSender);
  const auto id = token_id(t);
  std::vector<std::uint8_t> clear(head.begin(), head.end());
  clear.insert(clear.end(), id.begin(), id.end());
  if (!l->write_all(clear)) return fail(status::io);
  // No waiter under the id: the server closes it here.
  auto m1 = read_frame(*l, deadline);
  if (!m1) return fail(m1.error());
  auto psk = sender_psk(t);
  noise::handshake hs(noise::nn_psk0(), false, prologue_of(clear), std::nullopt, psk);
  sodium_memzero(psk.data(), psk.size());
  std::vector<std::uint8_t> payload, out;
  if (!hs.read_message(*m1, payload) || !payload.empty()) return fail(status::permission_denied);
  if (!hs.write_message({}, out)) return fail(status::internal);
  if (!write_frame(*l, out)) return fail(status::io);
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  noise::cipher_state from_server;
  from_server.init(keys->initiator_to_responder);
  std::array<std::uint8_t, kWordBytes> buf{};
  std::size_t got = 0;
  auto w = read_word(*l, from_server, buf, got, deadline, stopped);
  if (!w) return fail(w.error());
  if (*w != kPaired) return fail(status::io);
  return l;
}

result<std::unique_ptr<waiting>> register_waiter(std::unique_ptr<link> l, const token& t, const std::string& key,
                                                 int wait_ms) {
  if (!l) return err(status::invalid_arg);
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  const auto fail = [&](status s) -> result<std::unique_ptr<waiting>> {
    l->shutdown();
    return err(s);
  };
  // 1. CPace on the server key: the session id ours, a message each.
  std::array<std::uint8_t, kSessionIdBytes> sid;
  randombytes_buf(sid.data(), sid.size());
  const std::span<const std::uint8_t> ci(reinterpret_cast<const std::uint8_t*>(kCpaceChannel),
                                         sizeof kCpaceChannel - 1);
  cpace::party pake(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()), ci, sid);
  if (!pake.ok()) return fail(status::internal);
  const auto head = make_head(kWaiter);
  std::vector<std::uint8_t> clear(head.begin(), head.end());
  clear.insert(clear.end(), sid.begin(), sid.end());
  clear.insert(clear.end(), pake.message().begin(), pake.message().end());
  if (!l->write_all(clear)) return fail(status::io);
  std::array<std::uint8_t, cpace::kPointBytes> yb{};
  if (auto r = read_exact_until(*l, yb, deadline); !r) return fail(r.error());
  clear.insert(clear.end(), yb.begin(), yb.end());
  auto isk = pake.finish(yb, true, {}, {});
  if (!isk) return fail(status::corrupt);
  std::array<std::uint8_t, 32> psk;
  std::memcpy(psk.data(), isk->data(), psk.size());
  sodium_memzero(isk->data(), isk->size());
  // 2. Noise, the server first: its message opens only if it ran CPace on
  // the same key. Only then does the token go, sealed.
  noise::handshake hs(noise::nn_psk0(), false, prologue_of(clear), std::nullopt, psk);
  sodium_memzero(psk.data(), psk.size());
  auto m1 = read_frame(*l, deadline);
  if (!m1) return fail(m1.error());
  std::vector<std::uint8_t> payload, out;
  if (!hs.read_message(*m1, payload) || !payload.empty()) return fail(status::permission_denied);
  if (!hs.write_message(t, out)) return fail(status::internal);
  if (!write_frame(*l, out)) return fail(status::io);
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  auto w = std::make_unique<waiting>(std::move(l));
  w->from_server_.init(keys->initiator_to_responder);
  // 3. Waiting there only once the server says so.
  auto said = read_word(*w->l_, w->from_server_, w->word_, w->got_, deadline, {});
  if (!said || *said != kRegistered) {
    w->shutdown();
    return err(said ? status::io : said.error());
  }
  return w;
}

expected wait_paired(waiting& w, int wait_ms, const std::function<bool()>& stopped) {
  if (!w.l_) return err(status::invalid_arg);
  auto said = read_word(*w.l_, w.from_server_, w.word_, w.got_, clock::now() + std::chrono::milliseconds(wait_ms),
                        stopped);
  if (!said) return err(said.error());
  if (*said != kPaired) return err(status::io);
  return {};
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
  token t{};
  std::shared_ptr<link> partner;                         // under server::m_
  std::unique_ptr<noise::cipher_state> to_partner;      // under server::m_, with partner
  bool replaced = false;                                 // under server::m_
};

net::address server::source_of(const net::address& a) noexcept {
  net::address s = a;
  const std::size_t keep = a.v6 ? 8 : 3;
  std::fill(s.bytes.begin() + static_cast<std::ptrdiff_t>(keep), s.bytes.end(), std::uint8_t{0});
  return s;
}

result<std::unique_ptr<server>> server::start(options o) {
  if (o.key.size() < kMinKeyChars) return err(status::invalid_arg);
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
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
    net::address peer;
    auto c = listener_->accept_from(500, peer);
    if (!c) {
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    const net::address from = source_of(peer);
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
  const auto refuse = [&] {
    std::lock_guard lock(m_);
    ++counts_.refused;
    l->shutdown();
  };
  const auto deadline = clock::now() + std::chrono::milliseconds(kHelloMs);
  std::vector<std::uint8_t> clear(kHeadBytes);
  if (!read_exact_until(*l, clear, deadline)) return refuse();
  const auto role = parse_head(clear);
  if (!role) return refuse();
  std::array<std::uint8_t, 32> psk{};
  struct wipe {
    std::array<std::uint8_t, 32>& k;
    ~wipe() { sodium_memzero(k.data(), k.size()); }
  } wiped{psk};
  token sender_token{};
  if (*role == kWaiter) {
    // CPace on the key: one live guess per connection, nothing to take away.
    std::array<std::uint8_t, kSessionIdBytes + cpace::kPointBytes> said{};
    if (!read_exact_until(*l, said, deadline)) return refuse();
    clear.insert(clear.end(), said.begin(), said.end());
    const std::span<const std::uint8_t> ci(reinterpret_cast<const std::uint8_t*>(kCpaceChannel),
                                           sizeof kCpaceChannel - 1);
    cpace::party pake(std::span(reinterpret_cast<const std::uint8_t*>(opts_.key.data()), opts_.key.size()), ci,
                      std::span<const std::uint8_t>(said).first(kSessionIdBytes));
    if (!pake.ok()) return refuse();
    const auto& yb = pake.message();
    auto isk = pake.finish(std::span<const std::uint8_t>(said).subspan(kSessionIdBytes), false, {}, {});
    if (!isk) return refuse();
    std::memcpy(psk.data(), isk->data(), psk.size());
    sodium_memzero(isk->data(), isk->size());
    if (!l->write_all(yb)) return refuse();
    clear.insert(clear.end(), yb.begin(), yb.end());
  } else {
    // A sender names its token by its id; with nobody waiting under it,
    // it is closed before anything is made for it.
    token_id_bytes id{};
    if (!read_exact_until(*l, id, deadline)) return refuse();
    clear.insert(clear.end(), id.begin(), id.end());
    {
      std::lock_guard lock(m_);
      const auto it = waiting_.find(id);
      if (it == waiting_.end()) {
        ++counts_.refused;
        l->shutdown();
        return;
      }
      sender_token = it->second->t;
    }
    psk = sender_psk(sender_token);
  }
  // Noise, this side first: the client's answer is made against this
  // connection's fresh key, so nothing recorded opens here again.
  noise::handshake hs(noise::nn_psk0(), true, prologue_of(clear), std::nullopt, psk);
  std::vector<std::uint8_t> out, payload;
  if (!hs.write_message({}, out) || !write_frame(*l, out)) return refuse();
  auto m2 = read_frame(*l, deadline);
  if (!m2 || !hs.read_message(*m2, payload)) return refuse();
  if (payload.size() != (*role == kWaiter ? kTokenBytes : 0)) return refuse();
  auto keys = hs.split();
  if (!keys) return refuse();
  auto to_client = std::make_unique<noise::cipher_state>();
  to_client->init(keys->initiator_to_responder);
  slot.release();
  if (stop_.load()) return refuse();
  if (*role == kSender) {
    // Paired with the waiter for this token, or closed: a sender never waits.
    std::lock_guard lock(m_);
    const auto it = waiting_.find(token_id(sender_token));
    if (it == waiting_.end() || it->second->t != sender_token || static_cast<int>(counts_.active) >= opts_.max_pairs) {
      ++counts_.refused;
      l->shutdown();
      return;
    }
    it->second->partner = l;
    it->second->to_partner = std::move(to_client);
    ++counts_.active;  // reserved now (the waiter's thread gives it back), so a burst cannot overrun max_pairs
    waiting_.erase(it);
    cv_.notify_all();
    return;
  }
  token t;
  std::memcpy(t.data(), payload.data(), t.size());
  sodium_memzero(payload.data(), payload.size());
  serve_waiter(std::move(l), t, std::move(to_client), from);
}

void server::serve_waiter(std::shared_ptr<link> l, const token& t, std::unique_ptr<noise::cipher_state> to_waiter,
                          const net::address& from) {
  auto me = std::make_shared<waiter>();
  me->t = t;
  const auto id = token_id(t);
  {
    std::lock_guard lock(m_);
    const auto it = waiting_.find(id);
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
      waiting_.emplace(id, me);
    }
    ++waiting_from_[from];
  }
  // Given back however this ends: the place from this source, the token's
  // entry if still ours, and the pair's reservation.
  struct waiter_guard {
    server& s;
    const token_id_bytes& id;
    const net::address& from;
    std::shared_ptr<waiter>& me;
    std::shared_ptr<link> partner;
    std::unique_ptr<noise::cipher_state> to_partner;
    ~waiter_guard() {
      std::lock_guard lock(s.m_);
      if (--s.waiting_from_[from] <= 0) s.waiting_from_.erase(from);
      if (auto it = s.waiting_.find(id); it != s.waiting_.end() && it->second == me) s.waiting_.erase(it);
      if (partner) {
        partner->shutdown();
        --s.counts_.active;
      }
    }
  } guard{*this, id, from, me, nullptr, nullptr};
  // Registered: the receiver closes its old wait only once it hears this.
  if (!write_word(*l, *to_waiter, kRegistered)) {
    l->shutdown();
    return;
  }
  // Waiting: nothing may arrive before the partner (a byte, or a close,
  // ends the wait).
  const auto deadline = clock::now() + std::chrono::milliseconds(opts_.wait_ms);
  bool gone = false;
  for (;;) {
    {
      std::lock_guard lock(m_);
      if (me->partner) {
        guard.partner = me->partner;
        guard.to_partner = std::move(me->to_partner);
      }
      if (guard.partner || me->replaced || stop_.load() || clock::now() >= deadline || gone) break;
    }
    std::uint8_t b = 0;
    auto n = l->read_some({&b, 1}, 100);
    if (n || n.error() != status::timeout) gone = true;  // spoke first, or closed
  }
  if (!guard.partner || !guard.to_partner || gone || stop_.load()) {
    l->shutdown();
    return;
  }
  splice_result r;
  if (write_word(*l, *to_waiter, kPaired) && write_word(*guard.partner, *guard.to_partner, kPaired)) {
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
