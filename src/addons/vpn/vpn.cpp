// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/vpn/vpn.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

#include <sodium.h>

namespace mv::driftwire::vpn {

namespace {

using clock = std::chrono::steady_clock;
constexpr std::size_t kUdpQueue = 64;  // datagrams waiting for their flow to open, at most
// The DNS flow: lookups waiting to be sent, and asked but not yet answered,
// at most; an unanswered one is forgotten after kDnsAnswerMs (the app asks
// again). The flow is closed when unused for kDnsPipeIdleMs (before the
// server closes a lookup's flow, relay::kDnsIdleMs), and opened again by the
// next lookup.
constexpr std::size_t kDnsQueue = 256;
constexpr std::size_t kDnsAsked = 1024;
constexpr int kDnsAnswerMs = 10000;
constexpr int kDnsPipeIdleMs = relay::kDnsIdleMs / 2;

// A link shared by the worker list (shut down at the end) and the splice.
struct shared_link final : link {
  std::shared_ptr<link> l;
  explicit shared_link(std::shared_ptr<link> x) : l(std::move(x)) {}
  expected write_all(std::span<const std::uint8_t> b) override { return l->write_all(b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override { return l->read_some(into, ms); }
  void shutdown() noexcept override { l->shutdown(); }
  transport via() const noexcept override { return l->via(); }
};

}  // namespace

namespace {

class framed final : public device {
 public:
  explicit framed(std::unique_ptr<link> l) : l_(std::move(l)) {}
  ~framed() override { l_->shutdown(); }
  result<std::size_t> read(std::span<std::uint8_t> into, int timeout_ms) override {
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<std::uint8_t, 65536 + 2> buf{};
    for (;;) {
      if (part_.size() >= 2) {
        const std::size_t n = static_cast<std::size_t>(part_[0]) << 8 | part_[1];
        if (part_.size() >= 2 + n) {
          if (n > into.size()) return err(status::corrupt);
          std::copy_n(part_.begin() + 2, n, into.begin());
          part_.erase(part_.begin(), part_.begin() + 2 + static_cast<std::ptrdiff_t>(n));
          return n;
        }
      }
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
      if (left <= 0) return err(status::timeout);
      auto got = l_->read_some(buf, static_cast<int>(left));
      if (!got) return err(got.error() == status::timeout ? status::timeout : status::io);
      if (*got == 0) return err(status::io);
      part_.insert(part_.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*got));
    }
  }
  expected write(std::span<const std::uint8_t> p) override {
    if (p.size() > 0xffff) return err(status::invalid_arg);
    std::vector<std::uint8_t> out;
    out.reserve(2 + p.size());
    out.push_back(static_cast<std::uint8_t>(p.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(p.size()));
    out.insert(out.end(), p.begin(), p.end());
    std::lock_guard lock(w_);
    return l_->write_all(out);
  }
  void shutdown() noexcept override { l_->shutdown(); }

 private:
  std::unique_ptr<link> l_;
  std::mutex w_;
  std::vector<std::uint8_t> part_;  // the reader's own
};

}  // namespace

std::unique_ptr<device> framed_device(std::unique_ptr<link> l) { return std::make_unique<framed>(std::move(l)); }

struct runner::udp_flow {
  flow f;
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::vector<std::uint8_t>> queue;  // under m
  bool closed = false;                          // under m
  clock::time_point last = clock::now();        // under m
  std::shared_ptr<tunnel::remote_slot> remote = std::make_shared<tunnel::remote_slot>();
};

struct runner::dns_pipe {
  struct asked {
    flow f;               // the app's
    std::uint16_t id = 0;  // the app's ID
    clock::time_point at;
  };
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::pair<flow, std::vector<std::uint8_t>>> queue;  // under m
  std::map<std::uint16_t, asked> out;                             // under m: by our ID
  bool closed = false;                                            // under m
  std::shared_ptr<tunnel::remote_slot> remote = std::make_shared<tunnel::remote_slot>();
};

runner::runner(options o) : opts_(std::move(o)) {}

result<std::unique_ptr<runner>> runner::start(options o) {
  if (!o.dev) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);  // the DNS flow's IDs (safe to repeat)
  auto d = tunnel::dialer::make(o.dial);
  if (!d) return err(d.error());
  std::unique_ptr<runner> r(new runner(std::move(o)));
  r->dial_ = std::move(*d);
  runner* raw = r.get();
  stack::callbacks cb;
  cb.out = [raw](std::vector<std::uint8_t> p) { (void)raw->opts_.dev->write(p); };
  cb.on_tcp = [raw](std::unique_ptr<link> l, const flow& f) { raw->on_tcp(std::move(l), f); };
  cb.on_udp = [raw](const flow& f, std::span<const std::uint8_t> p) { raw->on_udp(f, p); };
  cb.timer = [raw](std::chrono::steady_clock::time_point due) { raw->kick(due); };
  r->stack_ = std::make_unique<stack>(std::move(cb), r->opts_.lim);
  r->dns_ = std::make_shared<dns_pipe>();
  r->dns_thread_ = std::thread([raw] { raw->serve_dns(); });  // not an app's: outside lim.max_connections
  r->pump_thread_ = std::thread([raw] { raw->pump(); });
  r->tick_thread_ = std::thread([raw] { raw->ticker(); });
  return r;
}

runner::~runner() {
  stop_.store(true);
  kick();
  if (dial_) dial_->stop();
  if (pump_thread_.joinable()) pump_thread_.join();  // reads time out within 250 ms
  if (tick_thread_.joinable()) tick_thread_.join();
  std::vector<worker> all;
  std::map<flow, std::shared_ptr<udp_flow>> flows;
  {
    std::lock_guard lock(m_);
    all = std::move(workers_);
    flows = std::move(udp_);
  }
  for (auto& [f, u] : flows) {
    std::lock_guard lock(u->m);
    u->closed = true;
    u->cv.notify_all();
  }
  if (dns_) {
    {
      std::lock_guard lock(dns_->m);
      dns_->closed = true;
      dns_->cv.notify_all();
    }
    dns_->remote->end();
  }
  if (dns_thread_.joinable()) dns_thread_.join();
  for (auto& w : all) {
    if (w.remote) w.remote->end();
    if (w.local) w.local->shutdown();
  }
  for (auto& w : all) w.t.join();
  all.clear();     // the links go before the stack they belong to
  flows.clear();
  stack_.reset();  // what is left is reset
  opts_.dev->shutdown();
}

runner::snapshot runner::state() const {
  std::lock_guard lock(m_);
  return snap_;
}

void runner::pump() {
  std::vector<std::uint8_t> buf(65536);
  while (!stop_.load()) {
    auto n = opts_.dev->read(buf, 250);
    if (!n) {
      if (n.error() == status::timeout) continue;
      {
        std::lock_guard lock(m_);
        snap_.problem = "The VPN's network interface went away.";
      }
      if (opts_.changed) opts_.changed();
      return;
    }
    stack_->input(std::span<const std::uint8_t>(buf.data(), *n));
  }
}

// Wakes only when there is work: the stack's next resend or idle limit, a
// kick (a timer that moved sooner, a worker to reap), or stop. Idle, with
// nothing outstanding, that is kIdleMs away, not every 100 ms
// (docs/plans/phone-vpn.md §2.2: a phone's battery).
void runner::ticker() {
  std::unique_lock t(tick_m_);
  while (!stop_.load()) {
    const std::uint64_t seen = kicks_;  // before looking: a kick after this wakes the wait
    armed_ = std::chrono::steady_clock::time_point::max();  // awake: every kick counts
    t.unlock();
    {
      std::lock_guard lock(m_);
      ++snap_.ticks;
      reap_locked();
    }
    stack_->tick();
    const auto next = stack_->next_deadline();
    t.lock();
    armed_ = next ? *next : std::chrono::steady_clock::time_point::max();
    const auto woken = [&] { return stop_.load() || kicks_ != seen; };
    if (next) {
      tick_cv_.wait_until(t, *next, woken);
    } else {
      tick_cv_.wait(t, woken);
    }
  }
}

void runner::kick(std::chrono::steady_clock::time_point due) {
  {
    std::lock_guard t(tick_m_);
    if (due >= armed_) return;  // the ticker wakes by then anyway
    ++kicks_;
  }
  tick_cv_.notify_all();
}

void runner::reap_locked() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (it->done->load()) {
      it->t.join();
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
}

void runner::note(relay::forward_answer a, bool opened) {
  {
    std::lock_guard lock(m_);
    if (opened) {
      snap_.problem.clear();
    } else {
      ++snap_.failed;
      if (a == relay::forward_answer::key) {
        snap_.problem = "Your relay server did not take its key: check what follows # in Your own relay server.";
      } else if (a == relay::forward_answer::off) {
        snap_.problem = "Your relay server does not forward: update it (driftwire-relay).";
      } else if (a == relay::forward_answer::failed) {
        snap_.problem = "Your relay server did not answer.";
      }
    }
  }
  if (opts_.changed) opts_.changed();
}

void runner::on_tcp(std::unique_ptr<link> l, const flow& f) {
  std::shared_ptr<link> local(std::move(l));
  std::lock_guard lock(m_);
  reap_locked();
  if (stop_.load() || workers_.size() >= opts_.lim.max_connections) {
    local->shutdown();
    return;
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto remote = std::make_shared<tunnel::remote_slot>();
  const std::string host = net::address_text(f.dst);
  const std::uint16_t port = f.dport;
  const bool dns = !f.v6 && host == opts_.s.dns4;
  try {
    workers_.push_back({std::thread([this, local, done, remote, host, port, dns] {
                          if (dns) {
                            local->shutdown();  // DNS over TCP to the tunnel's resolver: not carried
                          } else {
                            relay::forward_answer said{};
                            auto s = dial_->open(host, port, said, remote);
                            if (!stop_.load()) note(said, static_cast<bool>(s));
                            if (s) {
                              {
                                std::lock_guard g(m_);
                                ++snap_.tcp_open;
                                ++snap_.tcp_made;
                              }
                              if (opts_.changed) opts_.changed();
                              shared_link app(local);
                              relay::limits lim;
                              lim.max_bytes = ~std::uint64_t{0} >> 1;
                              lim.idle_ms = 10 * 60 * 1000;
                              (void)relay::splice(app, **s, lim, [this] { return stop_.load(); });
                              {
                                std::lock_guard g(m_);
                                --snap_.tcp_open;
                              }
                              if (opts_.changed && !stop_.load()) opts_.changed();
                            }
                            local->shutdown();
                          }
                          done->store(true);
                          kick();  // to be reaped now, not at the next deadline
                        }),
                        done, remote, local});
  } catch (...) {
    local->shutdown();
  }
}

void runner::on_udp(const flow& f, std::span<const std::uint8_t> payload) {
  if (!f.v6 && f.dport == 53 && net::address_text(f.dst) == opts_.s.dns4) return on_dns(f, payload);
  std::shared_ptr<udp_flow> u;
  {
    std::lock_guard lock(m_);
    if (stop_.load()) return;
    auto it = udp_.find(f);
    if (it == udp_.end()) {
      if (udp_.size() >= opts_.lim.max_udp_flows) return;  // dropped: UDP's own way
      u = std::make_shared<udp_flow>();
      u->f = f;
      udp_.emplace(f, u);
      ++snap_.udp_flows;
      auto done = std::make_shared<std::atomic<bool>>(false);
      try {
        workers_.push_back({std::thread([this, u, done] {
                              serve_udp(u);
                              done->store(true);
                              kick();  // to be reaped now, not at the next deadline
                            }),
                            done, u->remote, nullptr});
      } catch (...) {
        udp_.erase(f);
        return;
      }
    } else {
      u = it->second;
    }
  }
  std::lock_guard g(u->m);
  if (u->closed || u->queue.size() >= kUdpQueue) return;
  u->queue.emplace_back(payload.begin(), payload.end());
  u->last = clock::now();
  u->cv.notify_all();
}

void runner::serve_udp(std::shared_ptr<udp_flow> u) {
  const auto forget = [&] {
    std::lock_guard lock(m_);
    if (auto it = udp_.find(u->f); it != udp_.end() && it->second == u) udp_.erase(it);
  };
  relay::forward_answer said{};
  auto s = dial_->open(net::address_text(u->f.dst), u->f.dport, said, u->remote, relay::kForwardUdp);
  if (!stop_.load()) note(said, static_cast<bool>(s));
  if (!s) {
    forget();
    return;
  }
  std::shared_ptr<link> stream(std::move(*s));
  std::thread back([this, u, stream] {
    std::vector<std::uint8_t> part, d;
    for (;;) {
      auto r = relay::read_datagram(*stream, part, d, 250);
      if (!r) {
        if (r.error() == status::timeout) {
          std::lock_guard g(u->m);
          if (u->closed) return;
          continue;
        }
        break;
      }
      if (stop_.load()) return;
      stack_->udp_reply(u->f, d);
      std::lock_guard g(u->m);
      u->last = clock::now();
    }
    std::lock_guard g(u->m);
    u->closed = true;
    u->cv.notify_all();
  });
  for (;;) {
    std::vector<std::uint8_t> d;
    {
      std::unique_lock g(u->m);
      u->cv.wait_for(g, std::chrono::milliseconds(250), [&] { return u->closed || !u->queue.empty(); });
      if (u->closed || stop_.load()) break;
      if (u->queue.empty()) {
        if (clock::now() - u->last > std::chrono::milliseconds(relay::kUdpIdleMs)) break;
        continue;
      }
      d = std::move(u->queue.front());
      u->queue.pop_front();
    }
    if (!relay::write_datagram(*stream, d)) break;
  }
  {
    std::lock_guard g(u->m);
    u->closed = true;
  }
  stream->shutdown();
  back.join();
  forget();
}

void runner::on_dns(const flow& f, std::span<const std::uint8_t> query) {
  if (query.size() < 12) return;  // not a DNS message: no header
  std::lock_guard g(dns_->m);
  if (dns_->closed || dns_->queue.size() >= kDnsQueue) return;  // dropped: the app asks again
  dns_->queue.emplace_back(f, std::vector<std::uint8_t>(query.begin(), query.end()));
  dns_->cv.notify_all();
}

// The tunnel's own resolver goes to the relay's (kDnsHost): the network's
// DNS sees no name. One flow for every lookup, opened when one waits.
void runner::serve_dns() {
  dns_pipe& p = *dns_;
  const auto ended = [&] { return p.closed || stop_.load(); };
  for (;;) {
    {
      std::unique_lock g(p.m);
      p.cv.wait(g, [&] { return ended() || !p.queue.empty(); });
      if (ended()) return;
    }
    relay::forward_answer said{};
    auto s = dial_->open(std::string(relay::kDnsHost), 53, said, p.remote, relay::kForwardUdp);
    if (stop_.load()) return;
    note(said, static_cast<bool>(s));
    if (!s) {
      std::lock_guard g(p.m);
      p.queue.clear();  // the apps ask again, and the next lookup tries again
      continue;
    }
    {
      std::lock_guard lock(m_);
      ++snap_.udp_flows;
      ++snap_.dns_flows;
    }
    std::shared_ptr<link> stream(std::move(*s));
    std::atomic<bool> over{false};
    std::atomic<std::int64_t> last{clock::now().time_since_epoch().count()};
    const auto touch = [&] { last.store(clock::now().time_since_epoch().count()); };
    std::thread back([&] {
      std::vector<std::uint8_t> part, d;
      while (!over.load()) {
        auto r = relay::read_datagram(*stream, part, d, 250);
        if (!r) {
          if (r.error() == status::timeout) continue;
          break;
        }
        if (d.size() < 12) continue;
        const std::uint16_t ours = static_cast<std::uint16_t>(d[0] << 8 | d[1]);
        flow to;
        {
          std::lock_guard g(p.m);
          const auto it = p.out.find(ours);
          if (it == p.out.end()) continue;  // late, or not ours
          to = it->second.f;
          d[0] = static_cast<std::uint8_t>(it->second.id >> 8);
          d[1] = static_cast<std::uint8_t>(it->second.id);
          p.out.erase(it);
        }
        touch();
        if (stop_.load()) break;
        stack_->udp_reply(to, d);
      }
      over.store(true);
      std::lock_guard g(p.m);
      p.cv.notify_all();
    });
    for (;;) {
      std::pair<flow, std::vector<std::uint8_t>> q;
      {
        std::unique_lock g(p.m);
        p.cv.wait_for(g, std::chrono::milliseconds(250), [&] { return ended() || over.load() || !p.queue.empty(); });
        if (ended() || over.load()) break;
        const auto now = clock::now();
        for (auto it = p.out.begin(); it != p.out.end();) {
          it = now - it->second.at > std::chrono::milliseconds(kDnsAnswerMs) ? p.out.erase(it) : std::next(it);
        }
        if (p.queue.empty()) {
          const clock::time_point seen{clock::duration(last.load())};
          if (p.out.empty() && now - seen > std::chrono::milliseconds(kDnsPipeIdleMs)) break;
          continue;
        }
        if (p.out.size() >= kDnsAsked) continue;  // answers first
        q = std::move(p.queue.front());
        p.queue.pop_front();
        // Our ID, random (a resolver's answer is matched by it) and not one
        // in flight.
        std::uint16_t ours = 0;
        do {
          ours = static_cast<std::uint16_t>(randombytes_uniform(65536));
        } while (p.out.count(ours));
        p.out[ours] = {q.first, static_cast<std::uint16_t>(q.second[0] << 8 | q.second[1]), now};
        q.second[0] = static_cast<std::uint8_t>(ours >> 8);
        q.second[1] = static_cast<std::uint8_t>(ours);
      }
      touch();
      if (!relay::write_datagram(*stream, q.second)) {
        // The flow is gone: this lookup goes first on the next one.
        std::lock_guard g(p.m);
        const auto it = p.out.find(static_cast<std::uint16_t>(q.second[0] << 8 | q.second[1]));
        if (it != p.out.end()) {
          q.second[0] = static_cast<std::uint8_t>(it->second.id >> 8);
          q.second[1] = static_cast<std::uint8_t>(it->second.id);
          p.out.erase(it);
          p.queue.push_front(std::move(q));
        }
        break;
      }
    }
    over.store(true);
    stream->shutdown();
    back.join();
    std::lock_guard g(p.m);
    p.out.clear();  // asked on a flow that is gone: the apps ask again
  }
}

namespace {

// `a` with every bit past `prefix` cleared.
net::address masked(net::address a, int prefix) {
  const int bits = a.v6 ? 128 : 32;
  for (int i = 0; i < bits / 8; ++i) {
    const int keep = std::clamp(prefix - i * 8, 0, 8);
    a.bytes[static_cast<std::size_t>(i)] &= static_cast<std::uint8_t>(0xff00 >> keep);
  }
  return a;
}

bool holds(const net::subnet& n, const net::address& a) {
  return n.base.v6 == a.v6 && masked(a, n.prefix) == n.base;
}

void add_once(std::vector<net::subnet>& to, net::subnet n) {
  for (const auto& x : to) {
    if (x.base == n.base && x.prefix == n.prefix) return;
  }
  to.push_back(n);
}

}  // namespace

route_plan plan_routes(const settings& s, route_style style) {
  route_plan p;
  const auto at = [](const char* text, int prefix) { return net::subnet{*net::parse_address(text), prefix}; };
  if (style == route_style::halves) {
    p.include4 = {at("0.0.0.0", 1), at("128.0.0.0", 1)};
    p.include6 = {at("::", 1), at("8000::", 1)};
  } else {
    p.include4 = {at("0.0.0.0", 0)};
    p.include6 = {at("::", 0)};
  }
  // The tunnel's own addresses: never left out of it.
  std::vector<net::address> ours;
  for (const auto& t : {s.address4, s.dns4, s.address6}) {
    if (const auto a = net::parse_address(t)) ours.push_back(*a);
  }
  const auto exclude = [&](net::address base, int prefix) {
    const int bits = base.v6 ? 128 : 32;
    if (prefix <= 0 || prefix > bits) return;
    const net::subnet n{masked(base, prefix), prefix};
    for (const auto& a : ours) {
      if (holds(n, a)) return;
    }
    add_once(base.v6 ? p.exclude6 : p.exclude4, n);
  };
  for (const auto& a : s.exclude) exclude(a, a.v6 ? 128 : 32);
  // An IPv6 network wider than a /48 is not a local one (a 6to4 or Teredo
  // adapter's 2002::/16 or 2001::/32 would take a block of the internet
  // around the tunnel).
  for (const auto& n : s.exclude_nets) {
    if (n.base.v6 && n.prefix < kMinLocalPrefix6) continue;
    exclude(n.base, n.prefix);
  }
  return p;
}

}  // namespace mv::driftwire::vpn
