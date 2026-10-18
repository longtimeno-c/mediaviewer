// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/internet.h"

#include <algorithm>

#include "addons/sharepoint/meet.h"
#include "addons/sharepoint/splice.h"

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

std::optional<net::endpoint> relay_endpoint(const std::string& text) {
  if (auto e = parse_hostport(text)) return e;
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || text.front() == '[') return std::nullopt;
  const std::string host = text.substr(0, colon);
  const auto port = parse_hostport("0.0.0.0" + text.substr(colon));
  if (!port || host.size() > 253) return std::nullopt;
  const auto all = net::resolve(host);
  if (all.empty()) return std::nullopt;
  // IPv4 first: a sender's network has it more often than IPv6.
  const auto v4 = std::find_if(all.begin(), all.end(), [](const net::address& a) { return !a.v6; });
  return net::endpoint{net::address_text(v4 != all.end() ? *v4 : all.front()), port->port};
}

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

void internet_receiver::set_dht(bool on) {
  std::lock_guard lock(m_);
  opts_.dht = on;
}

void internet_receiver::set_relay(std::string relay) {
  std::lock_guard lock(m_);
  opts_.relay = std::move(relay);
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
  // A word code: the wait on the DHT stops (it withdraws its items), then
  // the client (which keeps its table) and the socket go.
  meet_stop_.store(true);
  if (meet_thread_.joinable()) meet_thread_.join();
  meet_stop_.store(false);
  relay_stop_.store(true);
  if (relay_thread_.joinable()) relay_thread_.join();
  relay_stop_.store(false);
  dht_.reset();
  dht_keys_.reset();
  if (hub_) hub_->stop();
  hub_.reset();
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
  const std::string relay_text = opts_.relay;
  const bool use_dht = opts_.dht;
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
  // #472 relays: the person's relay server, if set: this code waits there
  // too, under a token of its own (registered now, so the window says at
  // once whether the relay answered).
  std::optional<invite::relay_place> relay;
  std::unique_ptr<link> relay_link;
  std::string relay_note;
  if (l && !relay_text.empty() && !(opts_.force_words && use_dht)) {
    const auto at = relay_endpoint(relay_text);
    if (!at) {
      relay_note = " Your relay server's address is not one (address or name, a colon, the port).";
    } else {
      invite::relay_place r;
      r.at = *at;
      r.token = relay::random_token();
      auto c = net::connect(*at, kReachConnectMs);
      if (c && (*c)->write_all(relay::hello_bytes(r.token))) {
        relay_link = std::move(*c);
        relay = r;
      } else {
        relay_note = " Your relay server did not answer, so this code does not use it.";
      }
    }
  }
  if (!l) {
    s.p = phase::failed;
    s.note = "Could not listen for a sender on this computer.";
  } else {
    const std::uint16_t port = (*l)->port();
    if (opts_.loopback_only && opts_.force_words && use_dht) {
      s.p = phase::preparing;  // the tests' word code (below)
    } else if (opts_.loopback_only) {
      places = {{"::1", port}, {"127.0.0.1", port}};
      s.how = {"this computer"};
      if (relay) s.how.emplace_back("your relay");
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
      if (relay) s.how.emplace_back("your relay");
      const bool reachable = (!places.empty() || relay) && !opts_.force_words;
      // This network's address: a sender on the same network, or behind the
      // same router, comes in this way.
      const auto gw = opts_.mapping && opts_.mapping->gateway ? opts_.mapping->gateway : net::default_gateway();
      if (const auto lan = gw ? net::local_address_toward(*gw) : std::nullopt; lan && !lan->v6) {
        places.push_back({net::address_text(*lan), port});
      }
      if (places.size() > invite::kMaxPlaces) places.resize(invite::kMaxPlaces);
      if (!reachable && use_dht) {
        s.p = phase::preparing;  // a word code instead (below)
      } else if (!reachable) {
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
  // #472 PR 3: no public place, the DHT on: four words, and a wait on the
  // DHT for the sender (on its own thread) that punches to it.
  std::shared_ptr<udp_hub> hub;
  std::unique_ptr<dht::client> client;
  std::unique_ptr<dht::keys> keys;
  if (l && s.p == phase::preparing) {
    (*l)->shutdown();  // no TCP way in: the words are the way
    const dht::word_code words = dht::random_code();
    auto k = dht::derive(words, opts_.dht_fast_kdf);
    auto h = udp_hub::open(0, opts_.loopback_only);
    if (!k || !h) {
      s.p = phase::failed;
      s.note = "Could not make a code on this computer.";
    } else {
      hub = std::move(*h);
      client = std::make_unique<dht::client>(hub, opts_.dht_client);
      if (!client->start([this] { return meet_stop_.load() || stop_; })) {
        s.p = phase::failed;
        s.note = "The public DHT did not answer: this network may block UDP, or there is no connection. Forward a "
                 "port by hand (Forwarded port, below), or try another network.";
        client.reset();
        hub->stop();
        hub.reset();
      } else {
        keys = std::make_unique<dht::keys>(std::move(*k));
        s.p = phase::ready;
        s.text = dht::words_text(words);
        s.note = "Read these four words to the other person, or send them. They work once, for 10 minutes. Both "
                 "computers find each other through the public DHT.";
        s.how = {"DHT"};
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
  if (keys) {
    secret_ = std::vector<std::uint8_t>(keys->password.begin(), keys->password.end());
    expires_ = clock::now() + std::chrono::milliseconds(opts_.lifetime_ms);
    hub_ = std::move(hub);
    dht_ = std::move(client);
    dht_keys_ = std::move(keys);
    meet_stop_.store(false);
    dht::meet_options mo;
    mo.loopback = opts_.loopback_only;
    mo.wait_ms = opts_.lifetime_ms;
    meet_thread_ = std::thread([this, mo] {
      const auto stopped = [this] { return meet_stop_.load(); };
      auto l2 = dht::meet(hub_, *dht_, *dht_keys_, dht::role::receiver, mo, stopped, {});
      if (l2 && on_link_) on_link_(std::move(*l2));  // the internet hello comes over it
      dht::withdraw(*dht_, *dht_keys_, dht::role::receiver, stopped);
    });
  } else if (l && s.p != phase::failed) {
    const invite::code c = invite::make(places, relay);
    s.p = phase::ready;
    s.text = invite::encode(c);
    s.link = invite::link(c);
    s.note = "Send this code to the other person. It works once, for 10 minutes." + relay_note;
    secret_ = std::vector<std::uint8_t>(c.secret.begin(), c.secret.end());
    expires_ = clock::now() + std::chrono::milliseconds(opts_.lifetime_ms);
    listener_ = std::move(*l);
    net::listener* raw = listener_.get();
    accept_thread_ = std::thread([this, raw] { accept_loop(raw); });
    if (c.relay && relay_link) {
      relay_stop_.store(false);
      relay_thread_ = std::thread([this, at = c.relay->at, token = c.relay->token,
                                   first = std::move(relay_link)]() mutable {
        relay_loop(std::move(at), token, std::move(first));
      });
    }
  } else if (l) {
    (*l)->shutdown();
  }
  if (relay_link) relay_link->shutdown();  // not used: a word code, or no code
  if (s.p == phase::failed && !relay_note.empty()) s.note += relay_note;
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

way_in::~way_in() {
  if (listener_) listener_->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  listener_.reset();
  if (mapper_) {
    for (const auto& m : mapped_) mapper_->remove(m);
  }
}

std::unique_ptr<way_in> way_in::open(bool loopback_only, const std::optional<mapping::options>& mapping,
                                     const std::string& forwarded_text, link_fn on_link) {
  const auto forwarded = forwarded_text.empty() ? std::nullopt : parse_hostport(forwarded_text);
  auto l = net::listen_internet(forwarded ? forwarded->port : 0, loopback_only);
  if (!l && forwarded) l = net::listen_internet(0, loopback_only);
  if (!l) return nullptr;
  std::unique_ptr<way_in> w(new way_in());
  w->listener_ = std::move(*l);
  const std::uint16_t port = w->listener_->port();
  if (loopback_only) {
    w->places_ = {{"::1", port}, {"127.0.0.1", port}};  // the tests
  } else {
    // As a code's places (internet_receiver::prepare), less the LAN address.
    if (forwarded) w->places_.push_back(*forwarded);
    const auto mine = net::local_addresses();
    const auto v6 = std::find_if(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
    if (mapping) {
      w->mapper_ = std::make_unique<mapping::mapper>(*mapping);
      if (auto m = w->mapper_->map_tcp(port, port, kMapLifetimeS)) {
        w->places_.push_back(m->external);
        w->mapped_.push_back(*m);
      }
      if (v6 != mine.end()) {
        if (auto hole = w->mapper_->pinhole_v6(*v6, port, kMapLifetimeS)) w->mapped_.push_back(*hole);
      }
    }
    if (v6 != mine.end()) w->places_.push_back({net::address_text(*v6), port});
  }
  if (w->places_.empty()) return nullptr;  // nowhere a sender elsewhere could reach: the destructor tidies
  net::listener* raw = w->listener_.get();
  w->accept_thread_ = std::thread([raw, on_link = std::move(on_link)] {
    for (;;) {
      auto c = raw->accept(500);
      if (c) {
        on_link(std::move(*c));
        continue;
      }
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });
  return w;
}

void internet_receiver::relay_loop(net::endpoint at, std::array<std::uint8_t, 16> token, std::unique_ptr<link> first) {
  const auto stopped = [this] {
    if (relay_stop_.load()) return true;
    std::lock_guard lock(m_);
    return stop_ || snap_.p != phase::ready;
  };
  const auto rest = [&](int ms) {
    for (int waited = 0; waited < ms && !stopped(); waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };
  std::unique_ptr<link> l = std::move(first);
  while (!stopped()) {
    if (!l) {
      // The relay let the last wait go (or paired it with a sender that
      // left): wait there again, under the same token.
      auto c = net::connect(at, kReachConnectMs);
      if (!c || !(*c)->write_all(relay::hello_bytes(token))) {
        rest(2000);
        continue;
      }
      l = std::move(*c);
    }
    int left_ms = 0;
    {
      std::lock_guard lock(m_);
      left_ms = static_cast<int>(
          std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(expires_ - clock::now()).count()));
    }
    auto paired = relay::await_pair(std::move(l), left_ms, stopped);
    l.reset();
    if (!paired) {
      rest(500);
      continue;
    }
    auto spoke = relay::first_bytes(std::move(*paired), kConnectMs, stopped, "relay");
    if (spoke && on_link_) on_link_(std::move(*spoke));  // the internet hello comes over it
  }
  if (l) l->shutdown();
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
