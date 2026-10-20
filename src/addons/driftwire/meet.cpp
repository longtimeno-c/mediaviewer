// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/meet.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace mv::driftwire::dht {

namespace {

using clock = std::chrono::steady_clock;

// A probe (#499): kProbe, its kind, the session's tag (from the code: a
// quick filter, not a proof), and this side's random nonce for this meeting.
// A hello carries a MAC over (kind, nonce); a reply, choose and chosen carry
// the nonce of the side they answer too, and a MAC over (kind, nonce, that
// nonce), all under a key from the code. So only a holder of the code can
// answer a hello, pick a path or take one, and an answer recorded once is
// worth nothing in another meeting. A hello is answered with a reply; the
// sender's choose (on the one path it picked, UDP or TCP) with chosen, and
// the receiver answers only the first path it was offered. A choose and its
// chosen carry the attempt they belong to (1, 2, ...): when the handshake
// over the path agreed on fails, the sender picks the next path that
// answered under the next attempt, and the receiver takes it.
constexpr std::uint8_t kHello = 1, kReply = 2, kChoose = 3, kChosen = 4;
constexpr std::size_t kTagBytes = 12, kNonceBytes = 16, kMacBytes = 16;
constexpr std::size_t kHelloBytes = 2 + kTagBytes + kNonceBytes + kMacBytes;
constexpr std::size_t kAnswerBytes = 2 + kTagBytes + 2 * kNonceBytes + 1 + kMacBytes;  // + the attempt
constexpr char kTagContext[] = "MediaViewer Driftwire punch v1";
constexpr char kMacContext[] = "MediaViewer Driftwire punch MAC v2";
constexpr char kStreamContext[] = "MediaViewer Driftwire stream id v2";
constexpr std::size_t kMaxLearned = 8;  // hello senders not in the item, probed back
constexpr int kTickMs = 100;
constexpr int kMinVotes = 4;  // BEP 42 answers before a NAT is called easy
constexpr int kBirthdayPerTick = kBirthdayRate * kTickMs / 1000;
constexpr int kProbesPerTick = kProbeRate * kTickMs / 1000;
static_assert(kMaxLearned + kBirthdayPerTick < kProbesPerTick, "room left for the item's addresses");
constexpr int kTcpAfterMs = 1500;   // the sender picks TCP only when UDP found nothing by then
constexpr int kTcpHelloMs = 2000;   // a connection's hello, at most this long after it opens
constexpr std::uint8_t kMaxAttempts = 3;  // paths whose handshake is tried, at most (#499)
constexpr int kFallbackMs = 8000;   // the sender: to agree on the next path after a handshake failed
constexpr int kLaterMs = 60000;     // the receiver: a later pick is taken this long after its meeting
constexpr std::size_t kMaxReplies = 16;  // paths a reply came back on, kept for a fallback

std::int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

using nonce = std::array<std::uint8_t, kNonceBytes>;

// What this side signs its probes with: the session's tag and MAC key, from
// the code (both sides compute them), and its own nonce for this meeting.
struct prober {
  std::array<std::uint8_t, kTagBytes> tag{};
  std::array<std::uint8_t, 32> key{};
  nonce mine{};
  explicit prober(const keys& k) {
    crypto_generichash(tag.data(), tag.size(), reinterpret_cast<const std::uint8_t*>(kTagContext),
                       sizeof kTagContext - 1, k.salt_base.data(), k.salt_base.size());
    crypto_generichash(key.data(), key.size(), reinterpret_cast<const std::uint8_t*>(kMacContext),
                       sizeof kMacContext - 1, k.salt_base.data(), k.salt_base.size());
    randombytes_buf(mine.data(), mine.size());
  }
  prober(const prober&) = default;
  ~prober() { sodium_memzero(key.data(), key.size()); }

  [[nodiscard]] std::array<std::uint8_t, kMacBytes> mac(std::uint8_t kind, const nonce& a, const nonce* b,
                                                        std::uint8_t attempt = 0) const {
    crypto_generichash_state st;
    crypto_generichash_init(&st, key.data(), key.size(), kMacBytes);
    crypto_generichash_update(&st, &kind, 1);
    crypto_generichash_update(&st, a.data(), a.size());
    if (b) {
      crypto_generichash_update(&st, b->data(), b->size());
      crypto_generichash_update(&st, &attempt, 1);
    }
    std::array<std::uint8_t, kMacBytes> out;
    crypto_generichash_final(&st, out.data(), out.size());
    return out;
  }
  [[nodiscard]] std::array<std::uint8_t, kHelloBytes> hello() const {
    std::array<std::uint8_t, kHelloBytes> p;
    p[0] = kProbe;
    p[1] = kHello;
    std::memcpy(p.data() + 2, tag.data(), kTagBytes);
    std::memcpy(p.data() + 2 + kTagBytes, mine.data(), kNonceBytes);
    const auto m = mac(kHello, mine, nullptr);
    std::memcpy(p.data() + 2 + kTagBytes + kNonceBytes, m.data(), kMacBytes);
    return p;
  }
  // A reply, choose or chosen to the side whose nonce is `theirs` (a choose
  // and chosen: of `attempt`).
  [[nodiscard]] std::array<std::uint8_t, kAnswerBytes> answer(std::uint8_t kind, const nonce& theirs,
                                                               std::uint8_t attempt = 0) const {
    std::array<std::uint8_t, kAnswerBytes> p;
    p[0] = kProbe;
    p[1] = kind;
    std::memcpy(p.data() + 2, tag.data(), kTagBytes);
    std::memcpy(p.data() + 2 + kTagBytes, mine.data(), kNonceBytes);
    std::memcpy(p.data() + 2 + kTagBytes + kNonceBytes, theirs.data(), kNonceBytes);
    p[2 + kTagBytes + 2 * kNonceBytes] = attempt;
    const auto m = mac(kind, mine, &theirs, attempt);
    std::memcpy(p.data() + 2 + kTagBytes + 2 * kNonceBytes + 1, m.data(), kMacBytes);
    return p;
  }
  // A probe that verifies: its kind and the nonce of the side that sent it.
  // An answer must be to this side's nonce; a hello must not be this side's
  // own (sent back to it).
  struct heard {
    std::uint8_t kind = 0;
    nonce from{};
    std::uint8_t attempt = 0;
  };
  [[nodiscard]] std::optional<heard> check(std::span<const std::uint8_t> d) const {
    if (d.size() < 2 || d[0] != kProbe) return std::nullopt;
    const std::uint8_t kind = d[1];
    const bool hi = kind == kHello;
    if (!hi && kind != kReply && kind != kChoose && kind != kChosen) return std::nullopt;
    if (d.size() != (hi ? kHelloBytes : kAnswerBytes) || sodium_memcmp(d.data() + 2, tag.data(), kTagBytes) != 0) {
      return std::nullopt;
    }
    heard h;
    h.kind = kind;
    std::memcpy(h.from.data(), d.data() + 2 + kTagBytes, kNonceBytes);
    nonce to{};
    if (!hi) {
      std::memcpy(to.data(), d.data() + 2 + kTagBytes + kNonceBytes, kNonceBytes);
      if (sodium_memcmp(to.data(), mine.data(), kNonceBytes) != 0) return std::nullopt;
      h.attempt = d[2 + kTagBytes + 2 * kNonceBytes];
    } else if (sodium_memcmp(h.from.data(), mine.data(), kNonceBytes) == 0) {
      return std::nullopt;
    }
    const auto want = mac(kind, h.from, hi ? nullptr : &to, h.attempt);
    if (crypto_verify_16(d.data() + d.size() - kMacBytes, want.data()) != 0) return std::nullopt;
    return h;
  }
  // The stream's id, from both nonces and the attempt (#499: not from the
  // tag, which every probe shows).
  [[nodiscard]] std::uint32_t stream_id(const nonce& sender, const nonce& receiver, std::uint8_t attempt) const {
    crypto_generichash_state st;
    crypto_generichash_init(&st, key.data(), key.size(), 4);
    crypto_generichash_update(&st, reinterpret_cast<const std::uint8_t*>(kStreamContext), sizeof kStreamContext - 1);
    crypto_generichash_update(&st, sender.data(), sender.size());
    crypto_generichash_update(&st, receiver.data(), receiver.size());
    crypto_generichash_update(&st, &attempt, 1);
    std::uint8_t id[4];
    crypto_generichash_final(&st, id, sizeof id);
    return static_cast<std::uint32_t>(id[0]) | static_cast<std::uint32_t>(id[1]) << 8 |
           static_cast<std::uint32_t>(id[2]) << 16 | static_cast<std::uint32_t>(id[3]) << 24;
  }
};

struct punch_path {
  net::endpoint at;
  std::uint32_t sock = 0;  // the hub's socket (0: the main one; else a birthday extra)
  friend bool operator==(const punch_path& a, const punch_path& b) {
    return a.sock == b.sock && a.at.port == b.at.port && a.at.address == b.at.address;
  }
};

// What the hub's reader thread and the TCP thread touch lives as long as the
// probe handler (it outlives meet(): it answers a late choose).
struct punch {
  std::mutex m;
  std::condition_variable cv;
  std::optional<punch_path> answered;  // a reply came back on it: both ways work
  nonce answered_by{};                 // the nonce of the side that replied
  std::optional<punch_path> chosen;    // the receiver: the sender's pick, answered
  nonce chosen_by{};                   // the sender's nonce, in its choose
  bool acked = false;            // the sender: its pick was answered
  std::optional<punch_path> nominee;   // the sender: what it picked (UDP)
  nonce nominee_by{};                  // the receiver's nonce on that path
  bool nominated = false;        // the sender: picked, UDP or TCP (once only)
  bool tcp_taken = false;        // the receiver: chose TCP; UDP chooses go unanswered
  std::unique_ptr<link> tcp;     // the TCP link both agreed on
  std::vector<punch_path> learned;     // hellos from paths the item did not name
  // #499, after a handshake failed: every path a reply came back on (the
  // sender's next picks), and the attempt each side is at.
  std::vector<std::pair<punch_path, nonce>> replies;
  std::uint8_t nominee_attempt = 1;    // the sender: the attempt its pick is under
  std::uint8_t chosen_attempt = 0;     // the receiver: the attempt of the pick it took
  // The receiver, once meet() has returned: a later pick becomes a stream
  // handed to `later`, until `later_until`.
  bool returned = false;
  std::chrono::steady_clock::time_point later_until{};
  std::function<void(std::unique_ptr<link>)> later;
};

bool contains(const std::vector<net::endpoint>& v, const net::endpoint& e) {
  return std::any_of(v.begin(), v.end(), [&](const net::endpoint& x) { return x.port == e.port && x.address == e.address; });
}

// The other side's public IPv4 addresses (the one the DHT saw comes first in
// its item): where its NAT faces the internet. With `loopback`, every IPv4
// (the tests' stand-in NATs).
std::vector<net::endpoint> public_v4(const contents& c, bool loopback) {
  std::vector<net::endpoint> out;
  for (const auto& e : c.candidates) {
    const auto ip = net::parse_address(e.address);
    if (!ip || ip->v6 || (!loopback && !net::is_public(*ip))) continue;
    if (!contains(out, e)) out.push_back(e);
  }
  return out;
}

}  // namespace

bool seen_hard(const client& dht) { return dht.looks_hard(); }

std::vector<net::endpoint> own_candidates(const udp_hub& hub, const client& dht, bool loopback) {
  std::vector<net::endpoint> out;
  const std::uint16_t port = hub.port();
  const auto ext = dht.external();  // the NAT's mapping, as the DHT saw it
  if (loopback) {
    out = {{"127.0.0.1", port}, {"::1", port}};
    // The tests' stand-in NATs: the mapping the DHT saw, as on the internet.
    if (ext && ext->port != port) out.insert(out.begin(), *ext);
    return out;
  }
  if (ext) out.push_back(*ext);
  for (const auto& a : net::local_addresses()) {
    if (a.v6 && !net::is_public(a)) continue;  // link-local and ULA: not worth a probe
    out.push_back({net::address_text(a), port});
    if (out.size() >= kMaxCandidates) break;
  }
  return out;
}

result<std::unique_ptr<link>> meet(const std::shared_ptr<udp_hub>& hub, client& dht, const keys& k, role me,
                                   const meet_options& o, const std::function<bool()>& stop,
                                   const progress_fn& progress) {
  auto say = [&](const char* s) {
    if (progress) progress(s);
  };
  auto stopped = [&] { return stop && stop(); };
  const role other = me == role::receiver ? role::sender : role::receiver;
  const bool sender = me == role::sender;

  // The TCP port for a simultaneous open, bound before it is published.
  std::unique_ptr<net::tcp_rendezvous> rv;
  if (o.tcp) {
    if (auto r = net::open_rendezvous(o.loopback)) rv = std::move(*r);
  }

  // Whether this side's NAT is hard shows in how DHT nodes saw it; a client
  // that has heard too few asks once more first (a lookup it needs anyway).
  if (dht.vote_count() < kMinVotes && !stopped()) (void)dht.get(k.public_key, salt_of(k, other, 0), stop);

  // 1. Where this side can be reached, published under every salt of its role.
  contents mine;
  mine.candidates = own_candidates(*hub, dht, o.loopback);
  mine.hard = seen_hard(dht);
  mine.tcp_port = rv ? rv->port() : 0;
  mine.written_ms = unix_ms();
  std::int64_t seq = std::max(mine.written_ms / 1000, o.seq_floor);
  say("Publishing where this computer can be reached\xE2\x80\xA6");
  auto publish = [&] {
    std::vector<std::thread> puts;
    for (int i = 0; i < kTargets; ++i) {
      puts.emplace_back([&, i] {
        const std::string salt = salt_of(k, me, i);
        (void)dht.put(k, salt, seq, seal(k, salt, mine), stop);
      });
    }
    for (auto& t : puts) t.join();
  };
  publish();
  // The puts asked many nodes how they see us: a NAT that maps each
  // destination apart may show only now. Said again, so the other side
  // birthday-punches.
  if (!mine.hard && !stopped() && seen_hard(dht)) {
    mine.hard = true;
    mine.written_ms = unix_ms();
    seq = std::max(seq + 1, mine.written_ms / 1000);
    publish();
  }
  if (stopped()) return err(status::cancelled);

  // 2. The other side's item, under any of its salts, looked up apart.
  say("Looking for the other computer\xE2\x80\xA6");
  std::optional<contents> theirs;
  const auto until = clock::now() + std::chrono::milliseconds(o.wait_ms);
  while (!theirs) {
    if (stopped() || clock::now() >= until) return err(stopped() ? status::cancelled : status::timeout);
    std::mutex m;
    std::vector<std::thread> gets;
    for (int i = 0; i < kTargets; ++i) {
      gets.emplace_back([&, i] {
        const std::string salt = salt_of(k, other, i);
        auto got = dht.get(k.public_key, salt, stop);
        if (!got) return;
        auto c = open(k, salt, got->v, unix_ms());
        if (!c || c->gone) return;
        std::lock_guard lock(m);
        if (!theirs || c->written_ms > theirs->written_ms) theirs = std::move(*c);
      });
    }
    for (auto& t : gets) t.join();
    if (!theirs) {
      // Not there yet (the other side has not typed the code, or its put is
      // still on its way): again shortly; our item stays published.
      for (int w = 0; w < std::max(1, o.retry_ms / 100) && !stopped(); ++w) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  // 3. Punch: probes to every address the other gave (and, for its public
  // address, a few ports around it: a NAT that hands ports out in sequence).
  // Its addresses on its own network only when both sides face the internet
  // from the same public address (#499: behind one router; elsewhere that
  // address is some stranger's on this side's network).
  say("Connecting through both networks\xE2\x80\xA6");
  const std::vector<net::endpoint> their_public = public_v4(*theirs, o.loopback);
  const auto own_ext = dht.external();
  const bool same_router =
      own_ext && !their_public.empty() && net::parse_address(own_ext->address) == net::parse_address(their_public[0].address);
  std::vector<net::endpoint> addresses;  // the other side's, worth a probe or a connect
  for (const auto& e : theirs->candidates) {
    const auto ip = net::parse_address(e.address);
    if (!ip) continue;
    if (o.loopback || net::is_public(*ip) || (same_router && net::is_private_v4(*ip))) addresses.push_back(e);
  }
  std::vector<net::endpoint> targets = addresses;
  {
    const std::size_t n = targets.size();
    for (std::size_t i = 0; i < n; ++i) {
      const auto ip = net::parse_address(targets[i].address);
      if (!ip || !net::is_public(*ip) || ip->v6) continue;
      for (int d = 1; d <= kPredictPorts; ++d) {
        const int p = targets[i].port + d;
        if (p <= 65535) targets.push_back({targets[i].address, static_cast<std::uint16_t>(p)});
      }
    }
  }
  // The birthday punch, when exactly one side's NAT maps each destination
  // apart: the hard side probes the easy side's public address from many
  // sockets (each a new mapping), the easy side probes random ports of the
  // hard side's address; one probe that lands both ways opens the path.
  const bool birthday_hard = mine.hard && !theirs->hard && !their_public.empty();
  const bool birthday_easy = theirs->hard && !mine.hard && !their_public.empty();
  std::vector<std::uint32_t> extras;
  if (birthday_hard) extras = hub->open_extras(kBirthdaySockets);

  const prober pr(k);
  auto ps = std::make_shared<punch>();
  ps->later = o.later;
  std::weak_ptr<udp_hub> weak_hub = hub;
  hub->on_probe([ps, pr, weak_hub, sender, known = targets](std::span<const std::uint8_t> d, const net::endpoint& from,
                                                            std::uint32_t sock) {
    // Only what a holder of the code sent (#499): a stranger's probe, or one
    // recorded from another meeting, is dropped here.
    const auto heard = pr.check(d);
    if (!heard) return;
    auto h = weak_hub.lock();
    if (!h) return;
    const punch_path here{from, sock};
    if (heard->kind == kHello) {
      (void)h->send(pr.answer(kReply, heard->from), from, sock);
      bool fresh = false;
      {
        std::lock_guard lock(ps->m);
        if (!(sock == 0 && contains(known, from)) && ps->learned.size() < kMaxLearned &&
            std::find(ps->learned.begin(), ps->learned.end(), here) == ps->learned.end()) {
          ps->learned.push_back(here);
          fresh = true;
        }
      }
      // A path the item did not name (a NAT's new mapping): probed back at
      // once, so this side's own hello crosses it too.
      if (fresh) (void)h->send(pr.hello(), from, sock);
    } else if (heard->kind == kReply) {
      std::lock_guard lock(ps->m);
      if (!ps->answered) {
        ps->answered = here;
        ps->answered_by = heard->from;
      }
      if (ps->replies.size() < kMaxReplies &&
          std::none_of(ps->replies.begin(), ps->replies.end(), [&](const auto& r) { return r.first == here; })) {
        ps->replies.emplace_back(here, heard->from);
      }
      ps->cv.notify_all();
    } else if (heard->kind == kChoose && !sender) {
      bool answer = false;
      std::unique_ptr<link> late;
      std::function<void(std::unique_ptr<link>)> later;
      {
        std::lock_guard lock(ps->m);
        const std::uint8_t n = heard->attempt;
        if (n == 1 && !ps->chosen && !ps->tcp_taken) {
          ps->chosen = here;
          ps->chosen_by = heard->from;
          ps->chosen_attempt = 1;
        } else if (n > ps->chosen_attempt && n <= kMaxAttempts && ps->chosen_attempt > 0 && ps->returned &&
                   ps->later && std::chrono::steady_clock::now() < ps->later_until) {
          // The sender's handshake over the last pick failed: its next pick,
          // once this side's meeting has handed on the last (until then the
          // sender offers it again).
          ps->chosen = here;
          ps->chosen_by = heard->from;
          ps->chosen_attempt = n;
          late = h->stream(from, pr.stream_id(heard->from, pr.mine, n), false, sock);
          later = ps->later;
        }
        // A choose sent again: its answer was lost.
        answer = ps->chosen && *ps->chosen == here && ps->chosen_by == heard->from && ps->chosen_attempt == n;
        ps->cv.notify_all();
      }
      if (answer) (void)h->send(pr.answer(kChosen, heard->from, heard->attempt), from, sock);
      if (late && later) later(std::move(late));
    } else if (heard->kind == kChosen && sender) {
      std::lock_guard lock(ps->m);
      if (ps->nominee && *ps->nominee == here && ps->nominee_by == heard->from &&
          ps->nominee_attempt == heard->attempt) {
        ps->acked = true;
      }
      ps->cv.notify_all();
    }
  });

  // TCP simultaneous open, beside the UDP punch: the other side's TCP port at
  // each of its addresses. On each connection both sides first write a hello
  // (its nonce); the sender's choose goes on the first connection whose hello
  // verifies (if UDP has not picked first), and the receiver answers it.
  std::vector<net::endpoint> tcp_peers;
  if (rv && theirs->tcp_port != 0) {
    for (const auto& c : addresses) {
      const net::endpoint e{c.address, theirs->tcp_port};
      if (!contains(tcp_peers, e)) tcp_peers.push_back(e);
    }
  }
  std::atomic<bool> done{false};
  const auto tcp_after = clock::now() + std::chrono::milliseconds(std::min(kTcpAfterMs, o.punch_ms / 2));
  std::thread tcp_thread;
  if (!tcp_peers.empty()) {
    tcp_thread = std::thread([&, ps] {
      // All of `into`, until the punch ends or `until`.
      const auto read_exact = [&](link& l, std::span<std::uint8_t> into,
                                  clock::time_point until = clock::time_point::max()) {
        std::size_t have = 0;
        while (have < into.size() && !done.load() && clock::now() < until) {
          auto n = l.read_some(into.subspan(have), 100);
          if (!n && n.error() == status::timeout) continue;
          if (!n || *n == 0) return false;
          have += *n;
        }
        return have == into.size();
      };
      // The receiver: connections waiting for the sender's choose (it may
      // start punching seconds after this side, and picks only one).
      struct waiting {
        std::unique_ptr<link> l;
        std::array<std::uint8_t, kHelloBytes + kAnswerBytes> got{};
        std::size_t have = 0;
        nonce peer{};
        clock::time_point since = clock::now();
      };
      std::vector<waiting> held;
      while (!done.load()) {
        auto c = rv->next(tcp_peers, held.empty() ? 200 : 20);
        if (!c && c.error() != status::timeout) return;
        if (c && sender) {
          std::unique_ptr<link> l = std::move(*c);
          // Its hello first: only a holder of the code is offered the pick
          // (#499: whoever answers at an address on this network is not).
          if (!l->write_all(pr.hello())) continue;
          std::array<std::uint8_t, kHelloBytes> hi{};
          if (!read_exact(*l, hi, clock::now() + std::chrono::milliseconds(kTcpHelloMs))) continue;
          const auto heard = pr.check(hi);
          if (!heard || heard->kind != kHello) continue;
          {
            // UDP first: TCP is picked only when no UDP path came back by then.
            std::unique_lock lock(ps->m);
            ps->cv.wait_until(lock, tcp_after, [&] { return ps->nominated || done.load(); });
            if (ps->nominated || done.load()) return;  // UDP picked first
            ps->nominated = true;
          }
          bool taken = false;
          if (l->write_all(pr.answer(kChoose, heard->from, 1))) {
            std::array<std::uint8_t, kAnswerBytes> got{};
            const auto chosen = read_exact(*l, got) ? pr.check(got) : std::nullopt;
            taken = chosen && chosen->kind == kChosen && chosen->from == heard->from && chosen->attempt == 1;
          }
          std::lock_guard lock(ps->m);
          if (taken) {
            ps->tcp = std::move(l);
            ps->cv.notify_all();
            return;
          }
          // Not answered by the side whose hello verified: it did not take
          // this connection (its chosen is not lost: TCP), so the next path
          // that answers, UDP or TCP, may still be picked.
          ps->nominated = false;
          ps->cv.notify_all();
          continue;
        }
        if (c && held.size() < kMaxLearned) {
          waiting w{std::move(*c)};
          if (w.l->write_all(pr.hello())) held.push_back(std::move(w));
        }
        for (auto it = held.begin(); it != held.end();) {
          auto n = it->l->read_some(std::span(it->got).subspan(it->have), 1);
          if (!n && n.error() == status::timeout) {
            // No hello in time: not a holder of the code; its place is freed.
            if (it->have < kHelloBytes && clock::now() - it->since > std::chrono::milliseconds(kTcpHelloMs)) {
              it = held.erase(it);
            } else {
              ++it;
            }
            continue;
          }
          if (!n || *n == 0) {
            it = held.erase(it);  // closed: the sender went another way
            continue;
          }
          const std::size_t before = it->have;
          it->have += *n;
          if (before < kHelloBytes && it->have >= kHelloBytes) {
            const auto heard = pr.check(std::span(it->got).first(kHelloBytes));
            if (!heard || heard->kind != kHello) {
              it = held.erase(it);  // not a holder of the code
              continue;
            }
            it->peer = heard->from;
          }
          if (it->have < it->got.size()) {
            ++it;
            continue;
          }
          const auto choose = pr.check(std::span(it->got).subspan(kHelloBytes));
          if (!choose || choose->kind != kChoose || choose->from != it->peer || choose->attempt != 1) {
            it = held.erase(it);
            continue;
          }
          {
            std::lock_guard lock(ps->m);
            if (ps->chosen || ps->tcp_taken) return;  // UDP was offered first
            ps->tcp_taken = true;
          }
          if (it->l->write_all(pr.answer(kChosen, it->peer, 1))) {
            std::lock_guard lock(ps->m);
            ps->chosen_attempt = 1;
            ps->chosen_by = it->peer;
            ps->tcp = std::move(it->l);
            ps->cv.notify_all();
            return;
          }
          {
            // Not answered: the sender cannot take it; a UDP offer may still come.
            std::lock_guard lock(ps->m);
            ps->tcp_taken = false;
          }
          it = held.erase(it);
        }
      }
    });
  }

  const auto hello = pr.hello();
  std::optional<std::array<std::uint8_t, kAnswerBytes>> choose;
  const auto punch_until = clock::now() + std::chrono::milliseconds(o.punch_ms);
  const int span_ports = std::max(1, static_cast<int>(o.birthday_hi) - static_cast<int>(o.birthday_lo) + 1);
  std::size_t rr = 0;           // the hard side's round robin over its extras
  std::size_t next_target = 0;  // the item's addresses, in turn
  std::optional<punch_path> path_out;
  nonce peer_nonce{};
  std::unique_ptr<link> tcp_out;
  while (clock::now() < punch_until && !stopped()) {
    std::vector<punch_path> learned;
    std::optional<punch_path> nominee;
    {
      std::lock_guard lock(ps->m);
      learned = ps->learned;
      nominee = ps->nominee;
    }
    if (nominee && choose) {
      // The sender's pick, offered until the receiver answers.
      (void)hub->send(*choose, nominee->at, nominee->sock);
    } else {
      // This tick's probes, kProbesPerTick in all: the learned paths (live
      // mappings) first, the birthday's share, then the item's addresses in
      // turn with what is left.
      const int birthday = birthday_easy || (birthday_hard && !extras.empty()) ? kBirthdayPerTick : 0;
      int left = kProbesPerTick - birthday;
      for (const auto& l : learned) {
        (void)hub->send(hello, l.at, l.sock);
        --left;
      }
      for (int i = 0; i < left && !targets.empty() && i < static_cast<int>(targets.size()); ++i, ++next_target) {
        (void)hub->send(hello, targets[next_target % targets.size()]);
      }
      if (birthday_easy) {
        for (int i = 0; i < kBirthdayPerTick; ++i) {
          const auto port = static_cast<std::uint16_t>(o.birthday_lo + randombytes_uniform(static_cast<std::uint32_t>(span_ports)));
          (void)hub->send(hello, {their_public[static_cast<std::size_t>(i) % their_public.size()].address, port});
        }
      }
      for (int i = 0; birthday_hard && !extras.empty() && i < kBirthdayPerTick; ++i, ++rr) {
        (void)hub->send(hello, their_public[rr % their_public.size()], extras[rr % extras.size()]);
      }
    }
    std::unique_lock lock(ps->m);
    ps->cv.wait_for(lock, std::chrono::milliseconds(kTickMs), [&] {
      return ps->tcp != nullptr || (sender ? ps->acked || (ps->answered && !ps->nominated) : ps->chosen.has_value());
    });
    if (ps->tcp) {
      tcp_out = std::move(ps->tcp);
      break;
    }
    if (sender && ps->answered && !ps->nominated) {
      ps->nominated = true;  // the first path found both ways; TCP no longer picks
      ps->nominee = ps->answered;
      ps->nominee_by = ps->answered_by;
      choose = pr.answer(kChoose, ps->nominee_by, 1);
    }
    if (sender ? ps->acked : ps->chosen.has_value()) {
      path_out = sender ? ps->nominee : ps->chosen;
      peer_nonce = sender ? ps->nominee_by : ps->chosen_by;
      break;
    }
  }
  {
    std::lock_guard lock(ps->m);  // wakes the TCP thread's wait
    done.store(true);
    ps->cv.notify_all();
  }
  if (rv) rv->shutdown();
  if (tcp_thread.joinable()) tcp_thread.join();
  {
    // A TCP link agreed on in the last moment still counts.
    std::lock_guard lock(ps->m);
    if (!tcp_out && !path_out && ps->tcp) tcp_out = std::move(ps->tcp);
  }
  // Every birthday socket but the one the stream runs on, when the meeting
  // is over (a fallback may still pick a path on one of them).
  const auto close_extras = [&](const std::optional<punch_path>& kept) {
    if (extras.empty()) return;
    if (kept && kept->sock != 0) extras.erase(std::remove(extras.begin(), extras.end(), kept->sock), extras.end());
    hub->close_extras(extras);
    extras.clear();
  };
  // The receiver: from now on a later pick (the sender's handshake over this
  // one failed) is handed to o.later.
  const auto returning = [&] {
    std::lock_guard lock(ps->m);
    ps->returned = true;
    ps->later_until = clock::now() + std::chrono::milliseconds(kLaterMs);
  };
  if (tcp_out) {
    close_extras(std::nullopt);
    if (!sender || !o.open) {
      returning();
      return std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(tcp_out), transport::internet));
    }
  } else if (stopped() || !path_out) {
    close_extras(std::nullopt);
    if (stopped()) return err(status::cancelled);
    return err(status::timeout);
  } else if (!sender || !o.open) {
    close_extras(path_out);
    returning();
    // The probe handler stays (it answers hellos, a choose sent again, and a
    // later pick) until the stream carries the session's own pings.
    const std::uint32_t id = sender ? pr.stream_id(pr.mine, peer_nonce, 1) : pr.stream_id(peer_nonce, pr.mine, 1);
    return hub->stream(path_out->at, id, false, path_out->sock);
  }

  // 4. The sender with o.open (#499): nothing is kept before the handshake
  // over it holds. Each path agreed on goes to o.open; when it does not take
  // it, the next path a reply came back on is picked under the next attempt
  // (the receiver takes it as a later pick), at most kMaxAttempts.
  std::vector<punch_path> tried;
  std::uint8_t attempt = 1;
  for (;;) {
    std::unique_ptr<link> l;
    if (tcp_out) {
      l = std::make_unique<bearer_as>(std::move(tcp_out), transport::internet);
    } else {
      tried.push_back(*path_out);
      l = hub->stream(path_out->at, pr.stream_id(pr.mine, peer_nonce, attempt), false, path_out->sock);
    }
    if (o.open(l)) {
      close_extras(path_out);
      return std::unique_ptr<link>{};  // o.open took it
    }
    if (l) l->shutdown();
    if (stopped()) {
      close_extras(std::nullopt);
      return err(status::cancelled);
    }
    if (++attempt > kMaxAttempts) break;
    // The next path: one a reply came back on and not yet tried, with fresh
    // hellos meanwhile (they keep the mappings open and draw replies).
    path_out.reset();
    const auto next_until = clock::now() + std::chrono::milliseconds(kFallbackMs);
    std::optional<std::pair<punch_path, nonce>> next;
    std::optional<std::array<std::uint8_t, kAnswerBytes>> again;
    while (clock::now() < next_until && !stopped()) {
      std::vector<std::pair<punch_path, nonce>> replies;
      std::vector<punch_path> learned;
      {
        std::lock_guard lock(ps->m);
        replies = ps->replies;
        learned = ps->learned;
      }
      if (!next) {
        for (const auto& r : replies) {
          if (std::find(tried.begin(), tried.end(), r.first) != tried.end()) continue;
          next = r;
          break;
        }
        if (next) {
          std::lock_guard lock(ps->m);
          ps->acked = false;
          ps->nominee = next->first;
          ps->nominee_by = next->second;
          ps->nominee_attempt = attempt;
          again = pr.answer(kChoose, next->second, attempt);
        }
      }
      if (next) {
        (void)hub->send(*again, next->first.at, next->first.sock);
      } else {
        for (const auto& r : replies) (void)hub->send(hello, r.first.at, r.first.sock);
        for (const auto& p : learned) (void)hub->send(hello, p.at, p.sock);
        for (std::size_t i = 0; i < targets.size() && i < static_cast<std::size_t>(kProbesPerTick); ++i) {
          (void)hub->send(hello, targets[(next_target + i) % targets.size()]);
        }
        next_target += static_cast<std::size_t>(kProbesPerTick);
      }
      std::unique_lock lock(ps->m);
      ps->cv.wait_for(lock, std::chrono::milliseconds(kTickMs), [&] { return next && ps->acked; });
      if (next && ps->acked) {
        path_out = next->first;
        peer_nonce = next->second;
        break;
      }
    }
    if (!path_out) break;  // no other path agreed on in time
  }
  close_extras(std::nullopt);
  if (stopped()) return err(status::cancelled);
  return err(status::permission_denied);  // the handshake failed on every path agreed on
}

std::optional<std::size_t> published_under(client& dht, const std::vector<const keys*>& candidates, role other,
                                           int wait_ms, int retry_ms, const std::function<bool()>& stop) {
  const auto until = clock::now() + std::chrono::milliseconds(wait_ms);
  auto stopped = [&] { return stop && stop(); };
  while (!stopped() && clock::now() < until) {
    std::atomic<int> found{-1};
    std::vector<std::thread> gets;
    for (std::size_t c = 0; c < candidates.size(); ++c) {
      for (int i = 0; i < kTargets; ++i) {
        gets.emplace_back([&, c, i] {
          const keys& k = *candidates[c];
          const std::string salt = salt_of(k, other, i);
          auto got = dht.get(k.public_key, salt, [&] { return stopped() || found.load() >= 0; });
          if (!got) return;
          auto item = open(k, salt, got->v, unix_ms());
          if (!item || item->gone) return;
          int none = -1;
          (void)found.compare_exchange_strong(none, static_cast<int>(c));
        });
      }
    }
    for (auto& t : gets) t.join();
    if (found.load() >= 0) return static_cast<std::size_t>(found.load());
    for (int w = 0; w < std::max(1, retry_ms / 100) && !stopped(); ++w) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  return std::nullopt;
}

std::int64_t withdraw(client& dht, const keys& k, role me, const std::function<bool()>& stop, std::int64_t seq_floor) {
  contents gone;
  gone.gone = true;
  gone.written_ms = unix_ms();
  const std::int64_t seq = std::max(gone.written_ms / 1000 + 1, seq_floor + 1);
  std::vector<std::thread> puts;
  for (int i = 0; i < kTargets; ++i) {
    puts.emplace_back([&, i] {
      const std::string salt = salt_of(k, me, i);
      (void)dht.put(k, salt, seq, seal(k, salt, gone), stop);
    });
  }
  for (auto& t : puts) t.join();
  return seq;
}

}  // namespace mv::driftwire::dht
