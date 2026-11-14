// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/splice.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "wire/cpace.h"

namespace mv::driftwire::relay {

namespace {

using clock = std::chrono::steady_clock;
constexpr char kMagic[4] = {'M', 'V', 'R', 'L'};
constexpr std::size_t kSpliceBuffer = 64 * 1024;
constexpr int kPollMs = 250;
constexpr char kPrologue[] = "MediaViewer Driftwire relay 3";
constexpr char kCpaceChannel[] = "MediaViewer Driftwire relay 3 waiter-server";
constexpr char kCpaceForwardChannel[] = "MediaViewer Driftwire relay 3 forwarder-server";
constexpr char kIdContext[] = "MediaViewer Driftwire relay token id v3";
constexpr char kSenderPskContext[] = "MediaViewer Driftwire relay sender psk v3";
// A handshake message here is an ephemeral key, a payload (a token, or a
// forward's port, host length and host) and a tag at most.
constexpr std::size_t kMaxNoiseBytes = noise::kDhBytes + 4 + kMaxHostBytes + noise::kTagBytes;

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
      (head[5] != kSender && head[5] != kWaiter && head[5] != kForwarder && head[5] != kAsk) || head[6] != 0 ||
      head[7] != 0) {
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

result<net::endpoint> ask_seen(link& l, int wait_ms) {
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  const auto head = make_head(kAsk);
  if (!l.write_all(head)) return err(status::io);
  std::array<std::uint8_t, kSeenBytes> seen{};
  if (auto r = read_exact_until(l, seen, deadline); !r) return err(r.error() == status::timeout ? status::timeout : status::io);
  if (seen[0] != 4 && seen[0] != 6) return err(status::corrupt);
  net::address a;
  a.v6 = seen[0] == 6;
  std::memcpy(a.bytes.data(), seen.data() + 1, 16);
  const std::uint16_t port = static_cast<std::uint16_t>(seen[17] << 8 | seen[18]);
  if (port == 0) return err(status::corrupt);
  return net::endpoint{net::address_text(a), port};
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

// ---- forwarding (the Tunnel) -----------------------------------------------------------

expected sealed_link::write_all(std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> sealed, out;
  while (!bytes.empty()) {
    const auto chunk = bytes.first(std::min(bytes.size(), kSealedChunk));
    bytes = bytes.subspan(chunk.size());
    if (auto r = send_->encrypt({}, chunk, sealed); !r) return r;
    out.clear();
    out.push_back(static_cast<std::uint8_t>(sealed.size()));
    out.push_back(static_cast<std::uint8_t>(sealed.size() >> 8));
    out.insert(out.end(), sealed.begin(), sealed.end());
    if (auto r = inner_->write_all(out); !r) return r;
  }
  return {};
}

result<std::size_t> sealed_link::read_some(std::span<std::uint8_t> into, int timeout_ms) {
  const auto deadline = timeout_ms < 0 ? clock::time_point::max()
                                       : clock::now() + std::chrono::milliseconds(timeout_ms);
  const auto left = [&]() -> int {
    if (timeout_ms < 0) return -1;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
    return static_cast<int>(std::max<long long>(ms, 0));
  };
  while (plain_at_ >= plain_.size()) {
    // The length, then the frame, each read in part across calls.
    std::span<std::uint8_t> want = len_got_ < len_.size() ? std::span<std::uint8_t>(len_).subspan(len_got_)
                                                          : std::span<std::uint8_t>(frame_).subspan(frame_got_);
    const int ms = left();
    if (ms == 0) return err(status::timeout);
    auto n = inner_->read_some(want, ms);
    if (!n) return err(n.error());
    if (*n == 0) {
      if (len_got_ == 0) return std::size_t{0};  // closed between frames
      return err(status::io);
    }
    if (len_got_ < len_.size()) {
      len_got_ += *n;
      if (len_got_ < len_.size()) continue;
      const std::size_t size = static_cast<std::size_t>(len_[0]) | static_cast<std::size_t>(len_[1]) << 8;
      if (size <= noise::kTagBytes || size > kSealedChunk + noise::kTagBytes) {
        inner_->shutdown();
        return err(status::corrupt);
      }
      frame_.assign(size, 0);
      frame_got_ = 0;
      continue;
    }
    frame_got_ += *n;
    if (frame_got_ < frame_.size()) continue;
    plain_.clear();
    plain_at_ = 0;
    len_got_ = 0;
    if (!recv_->decrypt({}, frame_, plain_)) {
      inner_->shutdown();
      return err(status::corrupt);
    }
  }
  const std::size_t n = std::min(into.size(), plain_.size() - plain_at_);
  std::memcpy(into.data(), plain_.data() + plain_at_, n);
  plain_at_ += n;
  return n;
}

const char* forward_answer_name(forward_answer a) noexcept {
  switch (a) {
    case forward_answer::ok: return "ok";
    case forward_answer::key: return "key";
    case forward_answer::off: return "off";
    case forward_answer::port: return "port";
    case forward_answer::busy: return "busy";
    case forward_answer::unreachable: return "unreachable";
    case forward_answer::not_public: return "not_public";
    case forward_answer::kind: return "kind";
    case forward_answer::failed: return "failed";
  }
  return "failed";
}

bool forward_host_ok(std::string_view host) noexcept {
  if (host.empty() || host.size() > kMaxHostBytes) return false;
  for (const char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '-' || c == '_' || c == ':';
    if (!ok) return false;
  }
  return true;
}

namespace {

std::uint8_t word_of(forward_answer a) {
  switch (a) {
    case forward_answer::ok: return kForwarding;
    case forward_answer::off: return kForwardOff;
    case forward_answer::port: return kForwardPort;
    case forward_answer::busy: return kForwardBusy;
    case forward_answer::not_public: return kForwardNotPublic;
    case forward_answer::kind: return kForwardKind;
    default: return kForwardUnreachable;
  }
}

forward_answer answer_of(std::uint8_t w) {
  switch (w) {
    case kForwarding: return forward_answer::ok;
    case kForwardOff: return forward_answer::off;
    case kForwardPort: return forward_answer::port;
    case kForwardBusy: return forward_answer::busy;
    case kForwardNotPublic: return forward_answer::not_public;
    case kForwardUnreachable: return forward_answer::unreachable;
    case kForwardKind: return forward_answer::kind;
    default: return forward_answer::failed;
  }
}

}  // namespace

result<std::unique_ptr<link>> forward(std::unique_ptr<link> l, const std::string& key, const std::string& host,
                                      std::uint16_t port, int wait_ms, forward_answer& said, std::uint8_t kind) {
  said = forward_answer::failed;
  if (!l || !forward_host_ok(host) || port == 0 || key.size() < kMinKeyChars) return err(status::invalid_arg);
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  const auto deadline = clock::now() + std::chrono::milliseconds(wait_ms);
  const auto fail = [&](status s) -> result<std::unique_ptr<link>> {
    l->shutdown();
    return err(s);
  };
  // 1. CPace on the server key, as a waiter's (its own channel name).
  std::array<std::uint8_t, kSessionIdBytes> sid;
  randombytes_buf(sid.data(), sid.size());
  const std::span<const std::uint8_t> ci(reinterpret_cast<const std::uint8_t*>(kCpaceForwardChannel),
                                         sizeof kCpaceForwardChannel - 1);
  cpace::party pake(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()), ci, sid);
  if (!pake.ok()) return fail(status::internal);
  const auto head = make_head(kForwarder);
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
  // 2. Noise, the server first; the host and port go only once it has
  // proved the key, sealed against its fresh ephemeral.
  noise::handshake hs(noise::nn_psk0(), false, prologue_of(clear), std::nullopt, psk);
  sodium_memzero(psk.data(), psk.size());
  auto m1 = read_frame(*l, deadline);
  if (!m1) return fail(m1.error());
  std::vector<std::uint8_t> payload, out;
  if (!hs.read_message(*m1, payload) || !payload.empty()) {
    said = forward_answer::key;
    return fail(status::permission_denied);
  }
  std::vector<std::uint8_t> ask;
  ask.push_back(kind);
  ask.push_back(static_cast<std::uint8_t>(port >> 8));
  ask.push_back(static_cast<std::uint8_t>(port));
  ask.push_back(static_cast<std::uint8_t>(host.size()));
  ask.insert(ask.end(), host.begin(), host.end());
  if (!hs.write_message(ask, out)) return fail(status::internal);
  if (!write_frame(*l, out)) return fail(status::io);
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  auto from_server = std::make_unique<noise::cipher_state>();
  from_server->init(keys->initiator_to_responder);
  auto to_server = std::make_unique<noise::cipher_state>();
  to_server->init(keys->responder_to_initiator);
  // 3. Its word: forwarding, or why not.
  std::array<std::uint8_t, kWordBytes> buf{};
  std::size_t got = 0;
  auto w = read_word(*l, *from_server, buf, got, deadline, {});
  if (!w) return fail(w.error());
  said = answer_of(*w);
  if (said != forward_answer::ok) return fail(status::permission_denied);
  return std::unique_ptr<link>(std::make_unique<sealed_link>(std::move(l), std::move(to_server), std::move(from_server)));
}

expected write_datagram(link& l, std::span<const std::uint8_t> d) {
  if (d.size() > kMaxDatagram) return err(status::invalid_arg);
  std::vector<std::uint8_t> out;
  out.reserve(2 + d.size());
  out.push_back(static_cast<std::uint8_t>(d.size() >> 8));
  out.push_back(static_cast<std::uint8_t>(d.size()));
  out.insert(out.end(), d.begin(), d.end());
  return l.write_all(out);
}

expected read_datagram(link& l, std::vector<std::uint8_t>& part, std::vector<std::uint8_t>& out, int timeout_ms) {
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  std::array<std::uint8_t, 2048> buf{};
  for (;;) {
    if (part.size() >= 2) {
      const std::size_t n = static_cast<std::size_t>(part[0]) << 8 | part[1];
      if (n > kMaxDatagram) return err(status::corrupt);
      if (part.size() >= 2 + n) {
        out.assign(part.begin() + 2, part.begin() + 2 + static_cast<std::ptrdiff_t>(n));
        part.erase(part.begin(), part.begin() + 2 + static_cast<std::ptrdiff_t>(n));
        return {};
      }
    }
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
    if (left <= 0) return err(status::timeout);
    auto got = l.read_some(buf, static_cast<int>(left));
    if (!got) return err(got.error());
    if (*got == 0) return err(status::io);
    part.insert(part.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*got));
  }
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

// A link two owners can shut down (the server's worker list, and the TLS or
// sealed link over it).
struct shared_link final : link {
  std::shared_ptr<link> l;
  explicit shared_link(std::shared_ptr<link> x) : l(std::move(x)) {}
  expected write_all(std::span<const std::uint8_t> b) override { return l->write_all(b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override { return l->read_some(into, ms); }
  void shutdown() noexcept override { l->shutdown(); }
  transport via() const noexcept override { return l->via(); }
};

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
  if (s->opts_.tls_port != 0) {
    // Slice 4: the certificate, the knock's key (Argon2id, once), the port.
    auto ctx = tls::make_server_context(s->opts_.tls_cert, s->opts_.tls_key, s->opts_.tls_name);
    if (!ctx) return err(ctx.error());
    const auto k = tls::derive_knock_key(s->opts_.key);
    if (!k) return err(status::out_of_memory);
    auto tl = s->opts_.loopback_only ? net::listen(true) : net::listen_internet(s->opts_.tls_port, false);
    if (!tl) return err(tl.error());
    s->tls_ctx_ = std::move(*ctx);
    s->knock_ = *k;
    s->tls_listener_ = std::move(*tl);
  }
  server* raw = s.get();
  s->accept_thread_ = std::thread([raw] { raw->accept_loop(*raw->listener_, false); });
  if (s->tls_listener_) s->tls_accept_thread_ = std::thread([raw] { raw->accept_loop(*raw->tls_listener_, true); });
  return s;
}

server::~server() {
  stop_.store(true);
  if (listener_) listener_->shutdown();
  if (tls_listener_) tls_listener_->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  if (tls_accept_thread_.joinable()) tls_accept_thread_.join();
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
std::uint16_t server::tls_port() const noexcept { return tls_listener_ ? tls_listener_->port() : 0; }

void server::release_handshake(const net::address& from) {
  std::lock_guard lock(m_);
  --handshaking_;
  if (--handshaking_from_[from] <= 0) handshaking_from_.erase(from);
}

void server::handle_tls(std::shared_ptr<link> l, const net::address& from, const net::address& peer,
                        std::uint16_t peer_port) {
  tls::accepted_as as = tls::accepted_as::failed;
  auto t = tls::accept(*tls_ctx_, std::make_unique<shared_link>(l), knock_, opts_.decoy, as);
  if (as != tls::accepted_as::relay || !t) {
    release_handshake(from);
    std::lock_guard lock(m_);
    if (as == tls::accepted_as::website) ++counts_.website;
    l->shutdown();
    return;
  }
  // The hop, as on the plain port (its handshake slot still held).
  handle(std::shared_ptr<link>(std::move(t)), from, peer, peer_port);
}

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

void server::accept_loop(net::listener& from_listener, bool tls) {
  while (!stop_.load()) {
    net::address peer;
    std::uint16_t peer_port = 0;
    auto c = from_listener.accept_endpoint(500, peer, peer_port);
    if (!c) {
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    const net::address from = source_of(peer);
    std::shared_ptr<link> l(std::move(*c));
    std::lock_guard lock(m_);
    reap_locked();
    const bool full = static_cast<int>(workers_.size()) >= opts_.max_waiting + 2 * opts_.max_pairs + kMaxHandshakes + opts_.max_forwards;
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
      workers_.push_back({std::thread([this, l, done, from, peer, peer_port, tls] {
                            try {
                              tls ? handle_tls(l, from, peer, peer_port) : handle(l, from, peer, peer_port);
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

void server::handle(std::shared_ptr<link> l, const net::address& from, const net::address& peer, std::uint16_t peer_port) {
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
  if (*role == kAsk) {
    // What anyone on the path sees, said back; then closed. Nothing kept.
    if (peer_port == 0) return refuse();  // a listener that cannot say
    std::array<std::uint8_t, kSeenBytes> seen{};
    seen[0] = peer.v6 ? 6 : 4;
    std::memcpy(seen.data() + 1, peer.bytes.data(), 16);
    seen[17] = static_cast<std::uint8_t>(peer_port >> 8);
    seen[18] = static_cast<std::uint8_t>(peer_port & 0xff);
    (void)l->write_all(seen);
    {
      std::lock_guard lock(m_);
      ++counts_.asked;
    }
    l->shutdown();
    return;
  }
  std::array<std::uint8_t, 32> psk{};
  struct wipe {
    std::array<std::uint8_t, 32>& k;
    ~wipe() { sodium_memzero(k.data(), k.size()); }
  } wiped{psk};
  token sender_token{};
  if (*role == kWaiter || *role == kForwarder) {
    // CPace on the key: one live guess per connection, nothing to take away.
    std::array<std::uint8_t, kSessionIdBytes + cpace::kPointBytes> said{};
    if (!read_exact_until(*l, said, deadline)) return refuse();
    clear.insert(clear.end(), said.begin(), said.end());
    const char* const channel = *role == kWaiter ? kCpaceChannel : kCpaceForwardChannel;
    const std::span<const std::uint8_t> ci(reinterpret_cast<const std::uint8_t*>(channel), std::strlen(channel));
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
  std::string fwd_host;
  std::uint16_t fwd_port = 0;
  std::uint8_t fwd_kind = kForwardTcp;
  if (*role == kForwarder) {
    // The kind, the port, the host's length and the host, sealed: nothing else.
    if (payload.size() < 5 || payload.size() != 4u + payload[3]) return refuse();
    fwd_kind = payload[0];
    fwd_port = static_cast<std::uint16_t>(payload[1] << 8 | payload[2]);
    fwd_host.assign(reinterpret_cast<const char*>(payload.data()) + 4, payload[3]);
    if (fwd_port == 0 || !forward_host_ok(fwd_host)) return refuse();
  } else if (payload.size() != (*role == kWaiter ? kTokenBytes : 0)) {
    return refuse();
  }
  auto keys = hs.split();
  if (!keys) return refuse();
  auto to_client = std::make_unique<noise::cipher_state>();
  to_client->init(keys->initiator_to_responder);
  slot.release();
  if (stop_.load()) return refuse();
  if (*role == kForwarder) {
    auto from_client = std::make_unique<noise::cipher_state>();
    from_client->init(keys->responder_to_initiator);
    if (fwd_kind == kForwardUdp) {
      return serve_udp(std::move(l), std::move(fwd_host), fwd_port, std::move(to_client), std::move(from_client));
    }
    if (fwd_kind != kForwardTcp) {
      // A kind this server does not carry: said, sealed.
      (void)write_word(*l, *to_client, kForwardKind);
      l->shutdown();
      std::lock_guard lock(m_);
      ++counts_.forward_refused;
      return;
    }
    return serve_forward(std::move(l), std::move(fwd_host), fwd_port, std::move(to_client), std::move(from_client));
  }
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

namespace {

// net::resolve, then net::connect to each address in turn within
// kForwardConnectMs: public addresses only unless `any`.
result<std::unique_ptr<link>> connect_host(const std::string& host, std::uint16_t port, int timeout_ms, bool any,
                                           forward_answer& why) {
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  std::vector<net::address> addrs;
  if (const auto lit = net::parse_address(host)) {
    addrs.push_back(*lit);
  } else {
    addrs = net::resolve(host);
  }
  why = forward_answer::unreachable;
  bool any_allowed = false;
  for (const auto& a : addrs) {
    if (!any && !net::is_public(a)) continue;
    any_allowed = true;
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
    if (left <= 0) break;
    auto c = net::connect({net::address_text(a), port}, static_cast<int>(left));
    if (c) {
      why = forward_answer::ok;
      return std::move(*c);
    }
  }
  if (!addrs.empty() && !any_allowed) why = forward_answer::not_public;
  return err(status::io);
}

}  // namespace

namespace {

// The system's first resolver (POSIX: /etc/resolv.conf), else 1.1.1.1.
net::endpoint default_dns() {
  if (std::FILE* f = std::fopen("/etc/resolv.conf", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      char addr[128] = {};
      if (std::sscanf(line, " nameserver %127s", addr) == 1 && net::parse_address(addr)) {
        std::fclose(f);
        return {addr, 53};
      }
    }
    std::fclose(f);
  }
  return {"1.1.1.1", 53};
}

}  // namespace

void server::serve_udp(std::shared_ptr<link> l, std::string host, std::uint16_t port,
                       std::unique_ptr<noise::cipher_state> to_client,
                       std::unique_ptr<noise::cipher_state> from_client) {
  const auto say = [&](forward_answer a) { return write_word(*l, *to_client, word_of(a)); };
  const auto no = [&](forward_answer a) {
    (void)say(a);
    l->shutdown();
    std::lock_guard lock(m_);
    ++counts_.forward_refused;
  };
  // Where: the server's resolver, or a public address of the host.
  std::optional<net::endpoint> to;
  if (host == kDnsHost) {
    to = opts_.dns_upstream ? *opts_.dns_upstream : default_dns();
  } else {
    if (opts_.forward_udp_ports.empty()) return no(forward_answer::off);
    if (std::find(opts_.forward_udp_ports.begin(), opts_.forward_udp_ports.end(), port) ==
        opts_.forward_udp_ports.end()) {
      return no(forward_answer::port);
    }
    std::vector<net::address> addrs;
    if (const auto lit = net::parse_address(host)) {
      addrs.push_back(*lit);
    } else {
      addrs = net::resolve(host);
    }
    for (const auto& a : addrs) {
      if (opts_.forward_private || net::is_public(a)) {
        to = net::endpoint{net::address_text(a), port};
        break;
      }
    }
    if (!to) return no(addrs.empty() ? forward_answer::unreachable : forward_answer::not_public);
  }
  {
    std::lock_guard lock(m_);
    if (static_cast<int>(counts_.forwarding) >= opts_.max_forwards) {
      ++counts_.forward_refused;
      (void)say(forward_answer::busy);
      l->shutdown();
      return;
    }
    ++counts_.forwarding;
  }
  struct give_back {
    server& s;
    ~give_back() {
      std::lock_guard lock(s.m_);
      --s.counts_.forwarding;
    }
  } given{*this};
  const auto target = net::parse_address(to->address);
  auto sock = target ? net::open_udp(target->v6) : result<std::unique_ptr<net::udp_socket>>(err(status::invalid_arg));
  if (!sock) return no(forward_answer::unreachable);
  if (stop_.load() || !say(forward_answer::ok)) {
    l->shutdown();
    return;
  }
  {
    std::lock_guard lock(m_);
    ++counts_.forwarded;
  }
  sealed_link client(std::make_unique<shared_link>(l), std::move(to_client), std::move(from_client));
  std::atomic<bool> over{false};
  std::atomic<std::int64_t> last{now_ns()};
  std::atomic<std::uint64_t> moved{0};
  // The answers: only from where the flow goes.
  std::thread back([&] {
    std::vector<std::uint8_t> buf(kMaxDatagram);
    while (!over.load() && !stop_.load()) {
      net::endpoint from;
      auto n = (*sock)->receive_from(buf, 250, from);
      if (!n) {
        if (n.error() != status::timeout) break;
        continue;
      }
      const auto fa = net::parse_address(from.address);
      if (!fa || *fa != *target || from.port != to->port) continue;
      if (!write_datagram(client, std::span<const std::uint8_t>(buf.data(), *n))) break;
      moved += *n;
      last.store(now_ns());
    }
    over.store(true);
    client.shutdown();
  });
  std::vector<std::uint8_t> part, d;
  while (!over.load() && !stop_.load()) {
    auto r = read_datagram(client, part, d, 250);
    if (!r) {
      if (r.error() != status::timeout) break;
      if (now_ns() - last.load() > std::int64_t{kUdpIdleMs} * 1'000'000) break;
      continue;
    }
    if (!(*sock)->send_to(d, *to)) break;
    moved += d.size();
    last.store(now_ns());
  }
  over.store(true);
  (*sock)->shutdown();
  back.join();
  client.shutdown();
  std::lock_guard lock(m_);
  counts_.bytes += moved.load();
}

void server::serve_forward(std::shared_ptr<link> l, std::string host, std::uint16_t port,
                           std::unique_ptr<noise::cipher_state> to_client,
                           std::unique_ptr<noise::cipher_state> from_client) {
  const auto say = [&](forward_answer a) { return write_word(*l, *to_client, word_of(a)); };
  const auto no = [&](forward_answer a) {
    (void)say(a);
    l->shutdown();
    std::lock_guard lock(m_);
    ++counts_.forward_refused;
  };
  if (opts_.forward_ports.empty()) return no(forward_answer::off);
  if (std::find(opts_.forward_ports.begin(), opts_.forward_ports.end(), port) == opts_.forward_ports.end()) {
    return no(forward_answer::port);
  }
  {
    std::lock_guard lock(m_);
    if (static_cast<int>(counts_.forwarding) >= opts_.max_forwards) {
      ++counts_.forward_refused;
      (void)say(forward_answer::busy);
      l->shutdown();
      return;
    }
    ++counts_.forwarding;  // reserved now, given back below
  }
  struct give_back {
    server& s;
    ~give_back() {
      std::lock_guard lock(s.m_);
      --s.counts_.forwarding;
    }
  } given{*this};
  forward_answer why = forward_answer::unreachable;
  auto target = opts_.forward_connect
                    ? opts_.forward_connect(host, port, kForwardConnectMs, why)
                    : connect_host(host, port, kForwardConnectMs, opts_.forward_private, why);
  if (!target) return no(why == forward_answer::ok ? forward_answer::unreachable : why);
  if (stop_.load() || !say(forward_answer::ok)) {
    (*target)->shutdown();
    l->shutdown();
    return;
  }
  {
    std::lock_guard lock(m_);
    ++counts_.forwarded;
  }
  // The client's side sealed, the host's as it is (the browser's own TLS).
  sealed_link client(std::make_unique<shared_link>(l), std::move(to_client), std::move(from_client));
  const auto r = splice(client, **target, opts_.forward_lim, [this] { return stop_.load(); });
  std::lock_guard lock(m_);
  counts_.bytes += r.a_to_b + r.b_to_a;
}

}  // namespace mv::driftwire::relay
