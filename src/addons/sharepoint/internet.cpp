// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/internet.h"

#include <algorithm>

namespace mv::sharepoint {

namespace {

using clock = std::chrono::steady_clock;

std::optional<net::endpoint> parse_hostport(const std::string& text) {
  // "203.0.113.5:41000" or "[2001:db8::5]:41000".
  std::string host;
  std::string port;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
    host = text.substr(1, close - 1);
    port = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
  }
  if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  const unsigned long p = std::stoul(port);
  const auto ip = net::parse_address(host);
  if (!ip || p == 0 || p > 65535) return std::nullopt;
  return net::endpoint{net::address_text(*ip), static_cast<std::uint16_t>(p)};
}

}  // namespace

internet_receiver::internet_receiver(options o, link_fn on_link, std::function<void()> changed)
    : opts_(std::move(o)), on_link_(std::move(on_link)), changed_(std::move(changed)) {
  thread_ = std::thread([this] { run(); });
}

internet_receiver::~internet_receiver() {
  {
    std::lock_guard lock(m_);
    stop_ = true;
    cv_.notify_all();
  }
  thread_.join();
}

void internet_receiver::notify() {
  if (changed_) changed_();
}

void internet_receiver::make_code() {
  std::lock_guard lock(m_);
  want_code_ = true;
  cv_.notify_all();
}

void internet_receiver::set_forwarded(std::string forwarded) {
  std::lock_guard lock(m_);
  opts_.forwarded = std::move(forwarded);
}

void internet_receiver::drop_code() {
  std::lock_guard lock(m_);
  want_drop_ = true;
  cv_.notify_all();
}

internet_receiver::snapshot internet_receiver::state() const {
  std::lock_guard lock(m_);
  return snap_;
}

std::optional<std::vector<std::uint8_t>> internet_receiver::spend() {
  std::optional<std::vector<std::uint8_t>> out;
  {
    std::lock_guard lock(m_);
    if (snap_.p != phase::ready || !secret_ || clock::now() >= expires_) return std::nullopt;
    out = secret_;
    secret_.reset();
    snap_.p = phase::used;
    snap_.text.clear();
    snap_.link.clear();
    snap_.note = "Someone is using this code. It works once; make a new one for anyone else.";
    ++holding_;  // the attempt runs on the listener; release() when it ends
    cv_.notify_all();
  }
  notify();
  return out;
}

void internet_receiver::wrong_attempt() {
  {
    std::lock_guard lock(m_);
    snap_.p = phase::used;
    snap_.note = "Someone tried this code and it was wrong, so it no longer works. Make a new one.";
  }
  notify();
}

void internet_receiver::release() {
  std::lock_guard lock(m_);
  if (holding_ > 0) --holding_;
  cv_.notify_all();
}

void internet_receiver::run() {
  std::unique_lock lock(m_);
  while (!stop_) {
    const bool waiting = snap_.p == phase::ready;
    if (waiting) {
      cv_.wait_until(lock, expires_, [&] { return stop_ || want_code_ || want_drop_ || snap_.p != phase::ready; });
    } else {
      cv_.wait(lock, [&] {
        return stop_ || want_code_ || want_drop_ || (holding_ == 0 && (listener_ || !mapped_.empty()));
      });
    }
    if (stop_) break;
    if (want_code_) {
      want_code_ = false;
      want_drop_ = false;
      prepare(lock);
      continue;
    }
    if (want_drop_) {
      want_drop_ = false;
      secret_.reset();
      snap_ = snapshot{};
      lock.unlock();
      notify();
      lock.lock();
    }
    bool expired = false;
    if (snap_.p == phase::ready && clock::now() >= expires_) {
      secret_.reset();
      snap_.p = phase::expired;
      snap_.text.clear();
      snap_.link.clear();
      snap_.note = "The code expired unused after 10 minutes. Make a new one when you need it.";
      expired = true;
    }
    // No code waits and no transfer holds them: the listener and the
    // mappings go, before the window hears the code is over.
    if (snap_.p != phase::ready && snap_.p != phase::preparing && holding_ == 0) release_locked(lock);
    if (expired) {
      lock.unlock();
      notify();
      lock.lock();
    }
  }
  holding_ = 0;
  release_locked(lock);
}

void internet_receiver::release_locked(std::unique_lock<std::mutex>& lock) {
  std::unique_ptr<net::listener> l = std::move(listener_);
  lock.unlock();
  if (l) l->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  l.reset();
  if (mapper_) {
    for (const auto& m : mapped_) mapper_->remove(m);
  }
  mapped_.clear();
  mapper_.reset();
  lock.lock();
}

void internet_receiver::prepare(std::unique_lock<std::mutex>& lock) {
  if (holding_ > 0) {
    // A transfer over the last code still runs on its listener (one at a
    // time, as every transfer): a new code waits for it to end.
    snap_.note = "A transfer over the internet is running. Make a new code when it ends.";
    lock.unlock();
    notify();
    lock.lock();
    return;
  }
  secret_.reset();
  snap_ = snapshot{};
  snap_.p = phase::preparing;
  snap_.note = "Making a code\xE2\x80\xA6";
  // A code before this one: its listener and mappings go, unless a transfer
  // over it still runs (then this one waits for it to end: one listener).
  release_locked(lock);  // a code before this one: its listener and mappings go
  const std::string forwarded_text = opts_.forwarded;
  lock.unlock();
  notify();

  // A listener: on the forwarded port if one is set (the router sends it
  // there), else wherever the OS puts it.
  const auto forwarded = forwarded_text.empty() ? std::nullopt : parse_hostport(forwarded_text);
  auto l = net::listen_internet(forwarded ? forwarded->port : 0, opts_.loopback_only);
  if (!l && forwarded) l = net::listen_internet(0, opts_.loopback_only);
  snapshot s;
  std::vector<net::endpoint> places;
  std::unique_ptr<mapping::mapper> mapper;
  std::vector<mapping::mapped> mapped;
  if (!l) {
    s.p = phase::failed;
    s.note = "Could not listen for a sender on this computer.";
  } else {
    const std::uint16_t port = (*l)->port();
    if (opts_.loopback_only) {
      places = {{"::1", port}, {"127.0.0.1", port}};
      s.how = {"this computer"};
    } else {
      if (forwarded) {
        places.push_back(*forwarded);
        s.how.emplace_back("forwarded port");
      }
      std::vector<net::address> mine = net::local_addresses();
      const auto v6 = std::find_if(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
      if (opts_.mapping) {
        mapper = std::make_unique<mapping::mapper>(*opts_.mapping);
        if (auto m = mapper->map_tcp(port, port, kMapLifetimeS)) {
          places.push_back(m->external);
          s.how.emplace_back(mapping::method_name(m->how));
          mapped.push_back(*m);
          // For a sender whose network lets only 443 out (an office).
          if (m->external.port != 443) {
            if (auto m443 = mapper->map_tcp(port, 443, kMapLifetimeS); m443 && m443->external.port == 443) {
              places.push_back(m443->external);
              mapped.push_back(*m443);
            }
          }
        }
        if (v6 != mine.end()) {
          if (auto hole = mapper->pinhole_v6(*v6, port, kMapLifetimeS)) mapped.push_back(*hole);
        }
      }
      if (v6 != mine.end()) {
        places.push_back({net::address_text(*v6), port});
        s.how.emplace_back("IPv6");
      }
      const bool reachable = !places.empty();
      // This network's address: a sender on the same network, or behind the
      // same router, comes in this way.
      const auto gw = opts_.mapping && opts_.mapping->gateway ? opts_.mapping->gateway : net::default_gateway();
      if (const auto lan = gw ? net::local_address_toward(*gw) : std::nullopt; lan && !lan->v6) {
        places.push_back({net::address_text(*lan), port});
      }
      if (places.size() > invite::kMaxPlaces) places.resize(invite::kMaxPlaces);
      if (!reachable) {
        s.p = phase::failed;
        const bool nat = mapper && mapper->behind_another_nat();
        s.note = nat ? "This network has no public address of its own: its router sits behind another NAT (often "
                       "the provider's), so a port it opens is not reachable from outside, and there is no public "
                       "IPv6. Try another network (a phone hotspot often has IPv6), or forward a port on the outer "
                       "router (Forwarded port, below)."
                     : "No way in from the internet was found: the router did not open a port (turn on UPnP or "
                       "NAT-PMP in its settings), and this computer has no public IPv6 address. Forward a port by "
                       "hand (Forwarded port, below), or try another network (a phone hotspot often has IPv6).";
      }
    }
  }
  std::unique_ptr<net::listener> keep;
  lock.lock();
  if (stop_) {
    lock.unlock();
    if (mapper) {
      for (const auto& m : mapped) mapper->remove(m);
    }
    lock.lock();
    return;
  }
  if (l && s.p != phase::failed) {
    const invite::code c = invite::make(places);
    s.p = phase::ready;
    s.text = invite::encode(c);
    s.link = invite::link(c);
    s.note = "Send this code to the other person. It works once, for 10 minutes.";
    secret_ = std::vector<std::uint8_t>(c.secret.begin(), c.secret.end());
    expires_ = clock::now() + std::chrono::milliseconds(opts_.lifetime_ms);
    listener_ = std::move(*l);
    net::listener* raw = listener_.get();
    accept_thread_ = std::thread([this, raw] { accept_loop(raw); });
  } else if (l) {
    (*l)->shutdown();
  }
  mapper_ = std::move(mapper);
  mapped_ = std::move(mapped);
  snap_ = std::move(s);
  lock.unlock();
  notify();
  lock.lock();
}

void internet_receiver::accept_loop(net::listener* l) {
  for (;;) {
    auto c = l->accept(500);
    if (c) {
      if (on_link_) on_link_(std::move(*c));
      continue;
    }
    if (c.error() == status::cancelled) return;
    if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

int backoff_ms(int attempt) noexcept {
  static constexpr int kSteps[] = {1000, 2000, 5000, 10000};
  if (attempt < 1) return 0;
  if (attempt <= 4) return kSteps[attempt - 1];
  return 30000;
}

void start_round(link_race& race, const std::vector<net::endpoint>& places, const connect_fn& connect) {
  for (const auto& p : places) {
    const auto ip = net::parse_address(p.address);
    const int delay = ip && ip->v6 ? 0 : kHappyEyeballsMs;
    race.add([connect, p] { return connect(p, kReachConnectMs); }, delay);
  }
}

std::string why_unreachable(const std::vector<net::endpoint>& places, const std::vector<link_race::outcome>& outcomes,
                            bool here_has_ipv6) {
  int timeouts = 0, refused = 0, v6 = 0;
  for (std::size_t i = 0; i < places.size() && i < outcomes.size(); ++i) {
    const auto ip = net::parse_address(places[i].address);
    if (ip && ip->v6) ++v6;
    if (outcomes[i].how == status::timeout) ++timeouts;
    if (outcomes[i].how == status::io) ++refused;
  }
  if (places.empty()) return "The code holds no address to try.";
  if (v6 == static_cast<int>(places.size()) && !here_has_ipv6) {
    return "The code has only an IPv6 address, and this network has no IPv6. Try another network (a phone hotspot "
           "often has IPv6), or ask for a code made where the router can open a port.";
  }
  if (refused > 0 && timeouts == 0) {
    return "The other computer answered but MediaViewer is not listening there any more: its code may have expired, "
           "or MediaViewer closed. Ask for a new code.";
  }
  return "Nothing answered at the addresses in the code. The other network may block incoming connections, or this "
         "one may block going out. Ask them to turn on UPnP or NAT-PMP on their router, or forward a port; or try "
         "from another network.";
}

}  // namespace mv::sharepoint
