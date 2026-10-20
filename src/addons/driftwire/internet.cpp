// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/internet.h"

#include <algorithm>

#include "addons/driftwire/meet.h"
#include "addons/driftwire/splice.h"

namespace mv::driftwire {

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

std::optional<relay_server_field> relay_server_of(const std::string& text, std::string& why) {
  const auto hash = text.rfind('#');
  if (hash == std::string::npos || text.size() - hash - 1 < relay::kMinKeyChars) {
    why = "Your relay server needs its key after the address: <address>:<port>#<key>, as the server prints it.";
    return std::nullopt;
  }
  relay_server_field f{text.substr(0, hash), text.substr(hash + 1)};
  const auto colon = f.at.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= f.at.size()) {
    why = "Your relay server's address is not one (address or name, a colon, the port, then #key).";
    return std::nullopt;
  }
  return f;
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
        return stop_ || want_code_ || want_drop_ || (holding_ == 0 && (listener_ || mapped_));
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
  if (relay_setup_thread_.joinable()) relay_setup_thread_.join();  // a slow resolve, abandoned
  relay_stop_.store(false);
  dht_.reset();
  dht_keys_.reset();
  if (hub_) hub_->stop();
  hub_.reset();
  mapped_.reset();  // every mapping removed
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
  const std::string relay_text = opts_.relay;
  const bool use_dht = opts_.dht;
  lock.unlock();
  notify();

  // A listener wherever the OS puts it; the router is asked to map it below.
  auto l = net::listen_internet(0, opts_.loopback_only);
  snapshot s;
  std::vector<net::endpoint> places;
  std::unique_ptr<mapping::mapper> mapper;
  std::vector<mapping::mapped> mapped;
  // #472 relays: the person's relay server, if set: this code waits there
  // too, under a token of its own (registered now, so the window says at
  // once whether the relay answered).
  // Its name is resolved and it is asked on a thread of its own, beside the
  // router's mapping below; the code waits for it kRelaySetupMs at most.
  std::optional<invite::relay_place> relay;
  std::unique_ptr<link> relay_link;
  std::string relay_note, relay_key;
  struct relay_setup {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, abandoned = false;
    std::optional<invite::relay_place> place;
    std::unique_ptr<link> registered;
    std::string note;
  };
  auto setup = std::make_shared<relay_setup>();
  bool relay_started = false, relay_awaited = false;
  if (l && !relay_text.empty() && !(opts_.force_words && use_dht)) {
    std::string why;
    if (const auto server = relay_server_of(relay_text, why)) {
      relay_key = server->key;
      relay_started = true;
      relay_setup_thread_ = std::thread([setup, server = *server] {
        std::optional<invite::relay_place> place;
        std::unique_ptr<link> registered;
        std::string note;
        if (const auto at = relay_endpoint(server.at)) {
          invite::relay_place r;
          r.at = *at;
          r.token = relay::random_token();
          auto c = net::connect(*at, kReachConnectMs);
          if (c && relay::register_waiter(**c, r.token, server.key)) {
            registered = std::move(*c);
            place = r;
          } else {
            note = " Your relay server did not answer, so this code does not use it.";
          }
        } else {
          note = " Your relay server's name did not resolve, so this code does not use it.";
        }
        std::lock_guard g(setup->m);
        if (setup->abandoned) {
          if (registered) registered->shutdown();
        } else {
          setup->place = std::move(place);
          setup->registered = std::move(registered);
          setup->note = std::move(note);
        }
        setup->done = true;
        setup->cv.notify_all();
      });
    } else {
      relay_note = " " + why;
    }
  }
  const auto relay_started_at = clock::now();
  const auto await_relay = [&] {
    if (!relay_started || relay_awaited) return;
    relay_awaited = true;
    std::unique_lock g(setup->m);
    setup->cv.wait_until(g, relay_started_at + std::chrono::milliseconds(kRelaySetupMs), [&] { return setup->done; });
    if (!setup->done) {
      setup->abandoned = true;  // its thread closes what it gets; joined with this code's end
      relay_note = " Your relay server is slow to answer, so this code does not use it.";
      return;
    }
    relay = std::move(setup->place);
    relay_link = std::move(setup->registered);
    relay_note = setup->note;
  };
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
      await_relay();
      if (relay) s.how.emplace_back("your relay");
    } else {
      std::vector<net::address> mine = net::local_addresses();
      const auto v6 = std::find_if(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
      if (opts_.mapping) {
        mapper = std::make_unique<mapping::mapper>(*opts_.mapping);
        auto m = mapper->map_tcp(port, port, kMapLifetimeS);
        if (m) {
          places.push_back(m->external);
          s.how.emplace_back(mapping::method_name(m->how));
          mapped.push_back(*m);
          // For a sender whose network lets only 443 out (an office).
          if (m->external.port != 443) {
            if (auto m443 = mapper->map_tcp(port, 443, kMapLifetimeS)) {
              if (m443->external.port == 443) {
                places.push_back(m443->external);
                mapped.push_back(*m443);
              } else {
                mapper->remove(*m443);  // another port the router chose (#499: never left behind)
              }
            }
          }
        }
        // PCP answered: UPnP is not searched for a pinhole besides (#499).
        if (v6 != mine.end() && !(m && m->how == mapping::method::pcp)) {
          if (auto hole = mapper->pinhole_v6(*v6, port, kMapLifetimeS)) mapped.push_back(*hole);
        }
      }
      if (v6 != mine.end()) {
        places.push_back({net::address_text(*v6), port});
        s.how.emplace_back("IPv6");
      }
      await_relay();
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
                       "IPv6. Turn on \xE2\x80\x9C" "Find each other over the internet\xE2\x80\x9D, or try another network (a "
                       "phone hotspot often has IPv6)."
                     : "No way in from the internet was found: the router did not open a port (turn on UPnP or "
                       "NAT-PMP in its settings), and this computer has no public IPv6 address. Turn on \xE2\x80\x9C"
                       "Find each other over the internet\xE2\x80\x9D, or try another network (a phone hotspot often "
                       "has IPv6).";
      }
    }
  }
  await_relay();  // a branch above that did not ask
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
        s.note = "The public DHT did not answer: this network may block UDP, or there is no connection. Try "
                 "another network (a phone hotspot often has IPv6).";
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
    mapping::keeper(std::move(mapper), std::move(mapped), kMapLifetimeS);  // removes them
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
    mo.tcp = true;  // a word code's meeting, the window open: TCP simultaneous open too
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
      relay_thread_ = std::thread([this, at = c.relay->at, token = c.relay->token, key = relay_key,
                                   first = std::move(relay_link)]() mutable {
        relay_loop(std::move(at), token, std::move(key), std::move(first));
      });
    }
  } else if (l) {
    (*l)->shutdown();
  }
  if (relay_link) relay_link->shutdown();  // not used: a word code, or no code
  if (s.p == phase::failed && !relay_note.empty()) s.note += relay_note;
  if (mapper) mapped_ = std::make_unique<mapping::keeper>(std::move(mapper), std::move(mapped), kMapLifetimeS);
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
  mapped_.reset();  // every mapping removed
}

std::unique_ptr<way_in> way_in::open(bool loopback_only, const std::optional<mapping::options>& mapping,
                                     link_fn on_link) {
  auto l = net::listen_internet(0, loopback_only);
  if (!l) return nullptr;
  std::unique_ptr<way_in> w(new way_in());
  w->listener_ = std::move(*l);
  const std::uint16_t port = w->listener_->port();
  if (loopback_only) {
    w->places_ = {{"::1", port}, {"127.0.0.1", port}};  // the tests
  } else {
    // As a code's places (internet_receiver::prepare), less the LAN address.
    const auto mine = net::local_addresses();
    const auto v6 = std::find_if(mine.begin(), mine.end(), [](const net::address& a) { return a.v6 && net::is_public(a); });
    if (mapping) {
      auto mapper = std::make_unique<mapping::mapper>(*mapping);
      std::vector<mapping::mapped> mapped;
      auto m = mapper->map_tcp(port, port, kMapLifetimeS);
      if (m) {
        w->places_.push_back(m->external);
        mapped.push_back(*m);
      }
      if (v6 != mine.end() && !(m && m->how == mapping::method::pcp)) {
        if (auto hole = mapper->pinhole_v6(*v6, port, kMapLifetimeS)) mapped.push_back(*hole);
      }
      w->mapped_ = std::make_unique<mapping::keeper>(std::move(mapper), std::move(mapped), kMapLifetimeS);
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

void internet_receiver::relay_loop(net::endpoint at, std::array<std::uint8_t, 16> token, std::string key,
                                   std::unique_ptr<link> first) {
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
  const auto fresh = [&]() -> std::unique_ptr<link> {
    auto c = net::connect(at, kReachConnectMs);
    if (!c || !relay::register_waiter(**c, token, key)) return nullptr;
    return std::move(*c);
  };
  // A pair on `l`: on once the sender has spoken (a sender that lost its
  // race to a direct place closes without a byte).
  const auto hand_on = [&](std::unique_ptr<link> l) {
    auto spoke = relay::first_bytes(std::move(l), kConnectMs, stopped, "relay");
    if (spoke && on_link_) on_link_(std::move(*spoke));  // the internet hello comes over it
  };
  std::unique_ptr<link> l = std::move(first);
  auto registered = clock::now();
  while (!stopped()) {
    if (!l) {
      // The relay let the wait go, or paired it: wait there again, same token.
      l = fresh();
      if (!l) {
        rest(2000);
        continue;
      }
      registered = clock::now();
    }
    const int until_again = static_cast<int>(std::max<std::int64_t>(
        1, relay::kReregisterMs - std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - registered).count()));
    auto r = relay::wait_paired(*l, until_again, stopped);
    if (r) {
      hand_on(std::move(l));
      continue;
    }
    if (r.error() != status::timeout) {
      l->shutdown();
      l.reset();
      rest(500);
      continue;
    }
    // Ahead of a NAT that drops an idle connection: register again (the
    // server replaces the old wait), new before old.
    registered = clock::now();
    auto next = fresh();
    if (!next) continue;  // the old one may still do; try again in a minute
    if (relay::wait_paired(*l, 1, stopped)) {
      hand_on(std::move(l));  // paired just before it was replaced
    } else {
      l->shutdown();
    }
    l = std::move(next);
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
         "one may block going out. Ask them to turn on UPnP or NAT-PMP on their router, or both of you turn on "
         "\xE2\x80\x9C" "Find each other over the internet\xE2\x80\x9D; or try from another network.";
}

}  // namespace mv::driftwire
