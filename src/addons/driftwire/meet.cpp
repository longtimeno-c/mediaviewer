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

// A probe: kProbe, its kind, the session's tag. A hello is answered with a
// reply; the sender's choose (on the one path it picked, UDP or TCP) with
// chosen, and the receiver answers only the first path it was offered.
constexpr std::uint8_t kHello = 1, kReply = 2, kChoose = 3, kChosen = 4;
constexpr std::size_t kProbeBytes = 2 + 12;
constexpr char kTagContext[] = "MediaViewer Driftwire punch v1";
constexpr std::size_t kMaxLearned = 8;  // hello senders not in the item, probed back
constexpr int kTickMs = 100;
constexpr int kMinVotes = 4;  // BEP 42 answers before a NAT is called easy
constexpr int kBirthdayPerTick = kBirthdayRate * kTickMs / 1000;
constexpr int kProbesPerTick = kProbeRate * kTickMs / 1000;
static_assert(kMaxLearned + kBirthdayPerTick < kProbesPerTick, "room left for the item's addresses");
constexpr int kTcpAfterMs = 1500;   // the sender picks TCP only when UDP found nothing by then

std::int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The session's probe tag and stream id, from the code (both sides compute them).
std::array<std::uint8_t, 12> tag_of(const keys& k) {
  std::array<std::uint8_t, 12> t;
  crypto_generichash(t.data(), t.size(), reinterpret_cast<const std::uint8_t*>(kTagContext), sizeof kTagContext - 1,
                     k.salt_base.data(), k.salt_base.size());
  return t;
}

std::uint32_t stream_id_of(const std::array<std::uint8_t, 12>& tag) {
  return static_cast<std::uint32_t>(tag[8]) | static_cast<std::uint32_t>(tag[9]) << 8 |
         static_cast<std::uint32_t>(tag[10]) << 16 | static_cast<std::uint32_t>(tag[11]) << 24;
}

struct punch_path {
  net::endpoint at;
  std::uint32_t sock = 0;  // the hub's socket (0: the main one; else a birthday extra)
  friend bool operator==(const punch_path& a, const punch_path& b) {
    return a.sock == b.sock && a.at.port == b.at.port && a.at.address == b.at.address;
  }
};

std::array<std::uint8_t, kProbeBytes> probe_of(std::uint8_t kind, const std::array<std::uint8_t, 12>& tag) {
  std::array<std::uint8_t, kProbeBytes> p;
  p[0] = kProbe;
  p[1] = kind;
  std::memcpy(p.data() + 2, tag.data(), tag.size());
  return p;
}

// What the hub's reader thread and the TCP thread touch lives as long as the
// probe handler (it outlives meet(): it answers a late choose).
struct punch {
  std::mutex m;
  std::condition_variable cv;
  std::optional<punch_path> answered;  // a reply came back on it: both ways work
  std::optional<punch_path> chosen;    // the receiver: the sender's pick, answered
  bool acked = false;            // the sender: its pick was answered
  std::optional<punch_path> nominee;   // the sender: what it picked (UDP)
  bool nominated = false;        // the sender: picked, UDP or TCP (once only)
  bool tcp_taken = false;        // the receiver: chose TCP; UDP chooses go unanswered
  std::unique_ptr<link> tcp;     // the TCP link both agreed on
  std::vector<punch_path> learned;     // hellos from paths the item did not name
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
  say("Connecting through both networks\xE2\x80\xA6");
  std::vector<net::endpoint> targets = theirs->candidates;
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
  const std::vector<net::endpoint> their_public = public_v4(*theirs, o.loopback);
  const bool birthday_hard = mine.hard && !theirs->hard && !their_public.empty();
  const bool birthday_easy = theirs->hard && !mine.hard && !their_public.empty();
  std::vector<std::uint32_t> extras;
  if (birthday_hard) extras = hub->open_extras(kBirthdaySockets);

  const auto tag = tag_of(k);
  auto ps = std::make_shared<punch>();
  std::weak_ptr<udp_hub> weak_hub = hub;
  hub->on_probe([ps, tag, weak_hub, sender, known = targets](std::span<const std::uint8_t> d, const net::endpoint& from,
                                                             std::uint32_t sock) {
    if (d.size() != kProbeBytes || std::memcmp(d.data() + 2, tag.data(), tag.size()) != 0) return;
    auto h = weak_hub.lock();
    if (!h) return;
    const punch_path here{from, sock};
    if (d[1] == kHello) {
      (void)h->send(probe_of(kReply, tag), from, sock);
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
      if (fresh) (void)h->send(probe_of(kHello, tag), from, sock);
    } else if (d[1] == kReply) {
      std::lock_guard lock(ps->m);
      if (!ps->answered) ps->answered = here;
      ps->cv.notify_all();
    } else if (d[1] == kChoose && !sender) {
      bool answer = false;
      {
        std::lock_guard lock(ps->m);
        if (!ps->chosen && !ps->tcp_taken) ps->chosen = here;
        answer = ps->chosen && *ps->chosen == here;  // a choose sent again: its answer was lost
        ps->cv.notify_all();
      }
      if (answer) (void)h->send(probe_of(kChosen, tag), from, sock);
    } else if (d[1] == kChosen && sender) {
      std::lock_guard lock(ps->m);
      if (ps->nominee && *ps->nominee == here) ps->acked = true;
      ps->cv.notify_all();
    }
  });

  // TCP simultaneous open, beside the UDP punch: the other side's TCP port at
  // each of its addresses. The sender's choose goes on the first connection
  // that opens (if UDP has not picked first); the receiver answers it.
  std::vector<net::endpoint> tcp_peers;
  if (rv && theirs->tcp_port != 0) {
    for (const auto& c : theirs->candidates) {
      const net::endpoint e{c.address, theirs->tcp_port};
      if (!contains(tcp_peers, e)) tcp_peers.push_back(e);
    }
  }
  std::atomic<bool> done{false};
  const auto tcp_after = clock::now() + std::chrono::milliseconds(std::min(kTcpAfterMs, o.punch_ms / 2));
  std::thread tcp_thread;
  if (!tcp_peers.empty()) {
    tcp_thread = std::thread([&, ps] {
      // The receiver: connections waiting for the sender's choose (it may
      // start punching seconds after this side, and picks only one).
      struct waiting {
        std::unique_ptr<link> l;
        std::array<std::uint8_t, kProbeBytes> got{};
        std::size_t have = 0;
      };
      std::vector<waiting> held;
      while (!done.load()) {
        auto c = rv->next(tcp_peers, held.empty() ? 200 : 20);
        if (!c && c.error() != status::timeout) return;
        if (c && sender) {
          std::unique_ptr<link> l = std::move(*c);
          {
            // UDP first: TCP is picked only when no UDP path came back by then.
            std::unique_lock lock(ps->m);
            ps->cv.wait_until(lock, tcp_after, [&] { return ps->nominated || done.load(); });
            if (ps->nominated || done.load()) return;  // UDP picked first
            ps->nominated = true;
          }
          if (!l->write_all(probe_of(kChoose, tag))) {
            // Not all of it went: the receiver cannot have taken it.
            std::lock_guard lock(ps->m);
            ps->nominated = false;
            continue;
          }
          // Offered: the receiver may have taken it, so this side never falls
          // back to UDP. The answer is waited for here until the punch ends.
          std::array<std::uint8_t, kProbeBytes> got{};
          std::size_t have = 0;
          while (have < got.size() && !done.load()) {
            auto n = l->read_some(std::span(got).subspan(have), 100);
            if (!n && n.error() == status::timeout) continue;
            if (!n || *n == 0) break;
            have += *n;
          }
          if (have == got.size() && got == probe_of(kChosen, tag)) {
            std::lock_guard lock(ps->m);
            ps->tcp = std::move(l);
            ps->cv.notify_all();
          }
          return;
        }
        if (c && held.size() < kMaxLearned) held.push_back({std::move(*c)});
        for (auto it = held.begin(); it != held.end();) {
          auto n = it->l->read_some(std::span(it->got).subspan(it->have), 1);
          if (!n && n.error() == status::timeout) {
            ++it;
            continue;
          }
          if (!n || *n == 0) {
            it = held.erase(it);  // closed: the sender went another way
            continue;
          }
          it->have += *n;
          if (it->have < kProbeBytes) {
            ++it;
            continue;
          }
          if (it->got != probe_of(kChoose, tag)) {
            it = held.erase(it);
            continue;
          }
          {
            std::lock_guard lock(ps->m);
            if (ps->chosen || ps->tcp_taken) return;  // UDP was offered first
            ps->tcp_taken = true;
          }
          if (it->l->write_all(probe_of(kChosen, tag))) {
            std::lock_guard lock(ps->m);
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

  const auto hello = probe_of(kHello, tag);
  const auto choose = probe_of(kChoose, tag);
  const auto punch_until = clock::now() + std::chrono::milliseconds(o.punch_ms);
  const int span_ports = std::max(1, static_cast<int>(o.birthday_hi) - static_cast<int>(o.birthday_lo) + 1);
  std::size_t rr = 0;           // the hard side's round robin over its extras
  std::size_t next_target = 0;  // the item's addresses, in turn
  std::optional<punch_path> path_out;
  std::unique_ptr<link> tcp_out;
  while (clock::now() < punch_until && !stopped()) {
    std::vector<punch_path> learned;
    std::optional<punch_path> nominee;
    {
      std::lock_guard lock(ps->m);
      learned = ps->learned;
      nominee = ps->nominee;
    }
    if (nominee) {
      // The sender's pick, offered until the receiver answers.
      (void)hub->send(choose, nominee->at, nominee->sock);
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
    }
    if (sender ? ps->acked : ps->chosen.has_value()) {
      path_out = sender ? ps->nominee : ps->chosen;
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
  // Every birthday socket but the one the stream runs on.
  if (!extras.empty()) {
    if (path_out && path_out->sock != 0) extras.erase(std::remove(extras.begin(), extras.end(), path_out->sock), extras.end());
    hub->close_extras(extras);
  }
  if (tcp_out) return std::unique_ptr<link>(std::make_unique<bearer_as>(std::move(tcp_out), transport::internet));
  if (stopped()) return err(status::cancelled);
  if (!path_out) return err(status::timeout);
  // The probe handler stays (it answers hellos, and a choose sent again) until
  // the stream carries the session's own pings.
  return hub->stream(path_out->at, stream_id_of(tag), false, path_out->sock);
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
