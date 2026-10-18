// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/meet.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace mv::sharepoint::dht {

namespace {

using clock = std::chrono::steady_clock;

constexpr std::uint8_t kHello = 1, kReply = 2;
constexpr char kTagContext[] = "MediaViewer Sharepoint punch v1";

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

}  // namespace

std::vector<net::endpoint> own_candidates(const udp_hub& hub, const client& dht, bool loopback) {
  std::vector<net::endpoint> out;
  const std::uint16_t port = hub.port();
  if (loopback) return {{"127.0.0.1", port}, {"::1", port}};
  if (const auto ext = dht.external()) out.push_back(*ext);  // the NAT's mapping, as the DHT saw it
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
  const role other = me == role::receiver ? role::sender : role::receiver;

  // 1. Where this side can be reached, published under every salt of its role.
  contents mine;
  mine.candidates = own_candidates(*hub, dht, o.loopback);
  mine.written_ms = unix_ms();
  const std::int64_t seq = mine.written_ms / 1000;
  say("Publishing where this computer can be reached\xE2\x80\xA6");
  {
    std::vector<std::thread> puts;
    for (int i = 0; i < kTargets; ++i) {
      puts.emplace_back([&, i] {
        const std::string salt = salt_of(k, me, i);
        (void)dht.put(k, salt, seq, seal(k, salt, mine), stop);
      });
    }
    for (auto& t : puts) t.join();
  }
  if (stop && stop()) return err(status::cancelled);

  // 2. The other side's item, under any of its salts, looked up apart.
  say("Looking for the other computer\xE2\x80\xA6");
  std::optional<contents> theirs;
  const auto until = clock::now() + std::chrono::milliseconds(o.wait_ms);
  while (!theirs) {
    if ((stop && stop()) || clock::now() >= until) return err(stop && stop() ? status::cancelled : status::timeout);
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
      for (int w = 0; w < o.retry_ms / 100 && !(stop && stop()); ++w) {
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
  const auto tag = tag_of(k);
  // What the hub's reader thread touches lives as long as its last call.
  struct punch {
    std::mutex m;
    std::condition_variable cv;
    std::optional<net::endpoint> answered_by;  // a reply from there: both ways work
  };
  auto ps = std::make_shared<punch>();
  std::weak_ptr<udp_hub> weak_hub = hub;
  hub->on_probe([ps, tag, weak_hub](std::span<const std::uint8_t> d, const net::endpoint& from) {
    if (d.size() != 2 + tag.size() || std::memcmp(d.data() + 2, tag.data(), tag.size()) != 0) return;
    if (d[1] == kHello) {
      std::uint8_t reply[2 + 12];
      reply[0] = kProbe;
      reply[1] = kReply;
      std::memcpy(reply + 2, tag.data(), tag.size());
      if (auto h = weak_hub.lock()) (void)h->send(reply, from);
    } else if (d[1] == kReply) {
      std::lock_guard lock(ps->m);
      if (!ps->answered_by) ps->answered_by = from;
      ps->cv.notify_all();
    }
  });
  std::uint8_t hello[2 + 12];
  hello[0] = kProbe;
  hello[1] = kHello;
  std::memcpy(hello + 2, tag.data(), tag.size());
  const auto punch_until = clock::now() + std::chrono::milliseconds(o.punch_ms);
  std::optional<net::endpoint> path;
  while (clock::now() < punch_until && !(stop && stop())) {
    for (const auto& t : targets) (void)hub->send(hello, t);
    std::unique_lock lock(ps->m);
    ps->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return ps->answered_by.has_value(); });
    if (ps->answered_by) {
      path = ps->answered_by;
      break;
    }
  }
  // Keep the hole warm a moment: the other side may still be waiting for a
  // reply to its hello on the path just found. The probe handler stays (it
  // answers hellos) until the stream carries the session's own pings.
  for (int i = 0; i < 5 && path; ++i) {
    (void)hub->send(hello, *path);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (stop && stop()) return err(status::cancelled);
  if (!path) return err(status::timeout);
  // The sender's choice of path is the one: the receiver's stream takes its
  // peer from the sender's first packet.
  return hub->stream(*path, stream_id_of(tag), me == role::receiver);
}

void withdraw(client& dht, const keys& k, role me, const std::function<bool()>& stop) {
  contents gone;
  gone.gone = true;
  gone.written_ms = unix_ms();
  const std::int64_t seq = gone.written_ms / 1000 + 1;
  std::vector<std::thread> puts;
  for (int i = 0; i < kTargets; ++i) {
    puts.emplace_back([&, i] {
      const std::string salt = salt_of(k, me, i);
      (void)dht.put(k, salt, seq, seal(k, salt, gone), stop);
    });
  }
  for (auto& t : puts) t.join();
}

}  // namespace mv::sharepoint::dht
