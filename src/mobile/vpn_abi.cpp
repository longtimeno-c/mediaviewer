// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The extension's half of mv_mobile_vpn.h (docs/plans/phone-vpn.md §1.1): Driftwire's shared VPN
// (vpn::runner over netstack and tunnel::dialer) behind a vpn::device whose packets come and go
// through NEPacketTunnelFlow. The shared code runs unchanged; what is the phone's own is here.
#include <sodium.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "wire/relay_field.h"
#include "addons/vpn/vpn.h"
#include "core/json.h"
#include "mobile/mv_mobile_vpn.h"
#include "wire/serve_wire.h"

namespace dw = mv::driftwire;
namespace vpn = mv::driftwire::vpn;
namespace relay = mv::driftwire::relay;

namespace {

// Packets the interface read, waiting for the runner's pump. Bounded, as the OS's own queue is.
constexpr std::size_t kQueuedPackets = 512;

class flow_queue {
 public:
  mv_vpn_status push(const std::uint8_t* p, std::size_t n) {
    {
      std::lock_guard lock(m_);
      if (down_) return MV_VPN_INVALID;
      if (q_.size() >= kQueuedPackets) {
        ++dropped_;
        return MV_VPN_FULL;
      }
      q_.emplace_back(p, p + n);
    }
    cv_.notify_one();
    return MV_VPN_OK;
  }
  mv::result<std::size_t> pop(std::span<std::uint8_t> into, int timeout_ms) {
    std::unique_lock lock(m_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return down_ || !q_.empty(); });
    if (down_) return mv::err(mv::status::io);
    if (q_.empty()) return mv::err(mv::status::timeout);
    auto p = std::move(q_.front());
    q_.pop_front();
    if (p.size() > into.size()) return mv::err(mv::status::corrupt);
    std::memcpy(into.data(), p.data(), p.size());
    return p.size();
  }
  void shutdown() {
    {
      std::lock_guard lock(m_);
      down_ = true;
      q_.clear();
    }
    cv_.notify_all();
  }
  std::uint64_t dropped() const {
    std::lock_guard lock(m_);
    return dropped_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::vector<std::uint8_t>> q_;  // under m_
  bool down_ = false;                        // under m_
  std::uint64_t dropped_ = 0;                // under m_
};

// NEPacketTunnelFlow as the runner's interface: reads from the queue, writes through the callback.
class flow_device final : public vpn::device {
 public:
  flow_device(std::shared_ptr<flow_queue> q, mv_vpn_packet_fn out, void* ctx) : q_(std::move(q)), out_(out), ctx_(ctx) {}
  mv::result<std::size_t> read(std::span<std::uint8_t> into, int timeout_ms) override { return q_->pop(into, timeout_ms); }
  mv::expected write(std::span<const std::uint8_t> packet) override {
    if (out_) out_(ctx_, packet.data(), packet.size());
    return {};
  }
  void shutdown() noexcept override { q_->shutdown(); }

 private:
  std::shared_ptr<flow_queue> q_;
  mv_vpn_packet_fn out_;
  void* ctx_;
};

// The extension's profile (docs/plans/phone-vpn.md §2.1): a packet tunnel extension's memory
// ceiling is a few tens of MiB, so far fewer streams and smaller buffers than the desktop's.
constexpr vpn::limits kPhoneLimits{MV_VPN_MAX_TCP, MV_VPN_MAX_UDP, MV_VPN_SEND_BUFFER};

mv_vpn_status status_of(relay::forward_answer a) {
  switch (a) {
    case relay::forward_answer::key: return MV_VPN_KEY_REJECTED;
    case relay::forward_answer::off: return MV_VPN_NOT_FORWARDING;
    case relay::forward_answer::failed: return MV_VPN_UNREACHABLE;
    default: return MV_VPN_OK;  // the server answered and took the key; the site is another matter
  }
}

// One datagram flow to the server's own resolver, opened and closed: the server answers and takes
// the key, or why not. Nothing reaches the resolver (no datagram is sent) or anywhere else.
mv_vpn_status check_server(const dw::tunnel::dialer::options& o) {
  auto d = dw::tunnel::dialer::make(o);
  if (!d) return MV_VPN_INVALID;
  relay::forward_answer said = relay::forward_answer::failed;
  auto l = (*d)->open(std::string(relay::kDnsHost), 53, said, nullptr, relay::kForwardUdp);
  if (l) (*l)->shutdown();
  return l ? MV_VPN_OK : status_of(said);
}

}  // namespace

struct mv_vpn {
  dw::tunnel::dialer::options dial;
  std::shared_ptr<flow_queue> queue = std::make_shared<flow_queue>();
  mv_vpn_event_fn event = nullptr;
  void* ctx = nullptr;
  std::mutex event_m;
  mv_vpn_status said = MV_VPN_OK;  // under event_m: the last state the event told
  bool stopping = false;           // under event_m: mv_vpn_stop began; nothing more is told
  std::unique_ptr<vpn::runner> run;
  // What the runner's changed() reads: set once it has started, cleared before it stops, so no
  // event reaches Swift from inside mv_vpn_stop.
  std::atomic<vpn::runner*> live{nullptr};

  void tell(mv_vpn_status now) {
    std::lock_guard lock(event_m);
    if (stopping || now == said) return;
    said = now;
    if (event) event(ctx, now);
  }
  ~mv_vpn() {
    {
      std::lock_guard lock(event_m);
      stopping = true;
    }
    live.store(nullptr);
    run.reset();
    sodium_memzero(dial.relay.data(), dial.relay.size());
    if (dial.knock) sodium_memzero(dial.knock->data(), dial.knock->size());
  }
};

extern "C" mv_vpn_status mv_vpn_start(const mv_vpn_config* c, mv_vpn** out) {
  if (!c || !out || !c->relay || !c->key || !c->packet_out) return MV_VPN_INVALID;
  *out = nullptr;
  // The extension never derives the knock key (64 MiB of Argon2id, §2.1): with the HTTPS look, the app's.
  if (c->tls && !c->knock_key) return MV_VPN_INVALID;
  try {
    std::string host;
    std::uint16_t port = 0;
    if (!dw::serve::parse_relay_address(c->relay, host, port)) return MV_VPN_INVALID;
    auto v = std::make_unique<mv_vpn>();
    v->dial.relay = std::string(c->relay) + "#" + c->key;
    v->dial.tls = c->tls != 0;
    v->dial.tls_port = c->tls_port != 0 ? c->tls_port : dw::tls::kDefaultPort;
    if (c->tls) {
      dw::tls::knock_key k{};
      std::memcpy(k.data(), c->knock_key, k.size());
      v->dial.knock = k;
      sodium_memzero(k.data(), k.size());
    }
    v->event = c->event;
    v->ctx = c->ctx;
    if (const auto s = check_server(v->dial); s != MV_VPN_OK) return s;
    vpn::runner::options o;
    o.dial = v->dial;
    o.lim = kPhoneLimits;
    o.dev = std::make_unique<flow_device>(v->queue, c->packet_out, c->ctx);
    mv_vpn* raw = v.get();
    o.changed = [raw] {
      // changed() never runs under the runner's lock, so its state may be read here. The runner
      // says only that the relay had a problem (in the desktop window's words), so a problem
      // while running is told as UNREACHABLE; mv_vpn_check names it.
      if (auto* r = raw->live.load()) raw->tell(r->state().problem.empty() ? MV_VPN_OK : MV_VPN_UNREACHABLE);
    };
    auto r = vpn::runner::start(std::move(o));
    if (!r) return MV_VPN_INVALID;
    v->run = std::move(*r);
    v->live.store(v->run.get());
    *out = v.release();
    return MV_VPN_OK;
  } catch (...) {
    return MV_VPN_INTERNAL;
  }
}

extern "C" mv_vpn_status mv_vpn_packet_in(mv_vpn* v, const uint8_t* packet, size_t size) {
  if (!v || !packet || size == 0 || size > 0xffff) return MV_VPN_INVALID;
  try {
    return v->queue->push(packet, size);
  } catch (...) {
    return MV_VPN_INTERNAL;
  }
}

extern "C" mv_vpn_status mv_vpn_check(mv_vpn* v) {
  if (!v) return MV_VPN_INVALID;
  try {
    const auto s = check_server(v->dial);
    v->tell(s);
    return s;
  } catch (...) {
    return MV_VPN_INTERNAL;
  }
}

extern "C" void mv_vpn_get_state(mv_vpn* v, mv_vpn_state* out) {
  if (!out) return;
  *out = {};
  if (!v || !v->run) return;
  try {
    const auto s = v->run->state();
    out->tcp_open = s.tcp_open;
    out->tcp_made = s.tcp_made;
    out->udp_flows = s.udp_flows;
    out->failed = s.failed;
    out->dropped = v->queue->dropped();
    std::lock_guard lock(v->event_m);
    out->problem = v->said;
  } catch (...) {
    out->problem = MV_VPN_INTERNAL;
  }
}

extern "C" void mv_vpn_stop(mv_vpn* v) {
  try {
    delete v;
  } catch (...) {
  }
}

extern "C" char* mv_vpn_settings_json(const char* relay_at) {
  if (!relay_at) return nullptr;
  try {
    std::string host;
    std::uint16_t port = 0;
    if (!dw::serve::parse_relay_address(relay_at, host, port)) return nullptr;
    vpn::settings s;
    s.exclude = dw::relay_addresses(relay_at);
    if (s.exclude.empty()) return nullptr;  // not resolved: the tunnel would carry its own server
    // The local networks, as the desktop leaves them out (engine.cpp): the NAS at home, AirPlay.
    for (const auto& n : dw::net::local_subnets()) {
      if (!dw::net::is_public(n.base) || n.prefix >= 8) s.exclude_nets.push_back(n);
    }
    const auto plan = vpn::plan_routes(s, vpn::route_style::default_route);
    mv::json::writer w;
    w.begin_object();
    w.key("address4").string(s.address4).key("prefix4").integer(s.prefix4).key("dns4").string(s.dns4);
    // The tunnel's remote address as iOS shows it: the server's first address, not its name.
    w.key("remote").string(dw::net::address_text(s.exclude.front()));
    w.key("address6").string(s.address6).key("prefix6").integer(s.prefix6).key("mtu").integer(s.mtu);
    const auto routes = [&w](const char* name, const std::vector<dw::net::subnet>& all) {
      w.key(name).begin_array();
      for (const auto& n : all) {
        w.begin_object().key("base").string(dw::net::address_text(n.base)).key("prefix").integer(n.prefix).end_object();
      }
      w.end_array();
    };
    routes("include4", plan.include4);
    routes("exclude4", plan.exclude4);
    routes("include6", plan.include6);
    routes("exclude6", plan.exclude6);
    w.end_object();
    const std::string text = w.take();
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
  } catch (...) {
    return nullptr;
  }
}
