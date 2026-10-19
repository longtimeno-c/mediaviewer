// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's relays (issue #472, options 9 and 10; docs/design/28
// "Relays"): the rendezvous and the splice (splice.h), a code that carries a
// relay place, the own-device relay's requests and answers (relay.h), and two
// engines that cannot reach each other meeting through a relay server run in
// process, its bytes ciphertext only. The three-engine own-device case, which
// needs the stand-in DHT, is in test_driftwire_dht.cpp.
#include "driftwire_fixture.h"

#include "addons/driftwire/invite.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/relay.h"
#include "addons/driftwire/splice.h"

namespace relay = mv::driftwire::relay;
namespace invite = mv::driftwire::invite;

namespace {

// Every byte a relay passed, both ways, for checking that none of it is
// plaintext.
struct recorder {
  std::mutex m;
  std::vector<std::uint8_t> seen;
  relay::limits limits(relay::limits l = {}) {
    l.tap = [this](bool, std::span<const std::uint8_t> b) {
      std::lock_guard lock(m);
      seen.insert(seen.end(), b.begin(), b.end());
    };
    return l;
  }
  std::size_t size() {
    std::lock_guard lock(m);
    return seen.size();
  }
  // Any 32 bytes of `plain` (every 4 KiB, and its start and end) in what passed.
  bool saw_any_of(const std::vector<std::uint8_t>& plain) {
    std::lock_guard lock(m);
    for (std::size_t at = 0; at + 32 <= plain.size(); at += 4096) {
      if (std::search(seen.begin(), seen.end(), plain.begin() + at, plain.begin() + at + 32) != seen.end()) {
        return true;
      }
    }
    const auto tail = plain.end() - 32;
    return std::search(seen.begin(), seen.end(), tail, plain.end()) != seen.end();
  }
};

std::unique_ptr<sp::link> dial(std::uint16_t port) {
  auto c = sp::net::connect({"127.0.0.1", port}, 3000);
  REQUIRE(c);
  return std::move(*c);
}

std::vector<std::uint8_t> read_n(sp::link& l, std::size_t n) {
  std::vector<std::uint8_t> out(n);
  REQUIRE(sp::read_exact(l, out, 10000));
  return out;
}

constexpr const char* kKey = "TEST-RELAY-KEY-0123456789";

// A waiter registered with the key; nullptr when the server closed it.
std::unique_ptr<sp::link> waiter(std::uint16_t port, const relay::token& t, const std::string& key = kKey) {
  auto l = dial(port);
  REQUIRE(relay::register_waiter(*l, t, key));
  return l;
}

// Both sides of a rendezvous: a waiter, then a sender with its token.
std::pair<std::unique_ptr<sp::link>, std::unique_ptr<sp::link>> paired(std::uint16_t port, const relay::token& t) {
  auto b = waiter(port, t);
  mv::expected waited = mv::err(mv::status::internal);
  std::thread waits([&] { waited = relay::wait_paired(*b, 5000, {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // registered before the sender comes
  auto a = relay::join(dial(port), t, 5000, {});
  waits.join();
  REQUIRE(a);
  REQUIRE(waited);
  return {std::move(*a), std::move(b)};
}

relay::server::options server_options(relay::limits lim = {}) {
  relay::server::options o;
  o.key = kKey;
  o.loopback_only = true;
  o.lim = std::move(lim);
  return o;
}

bool closed_soon(sp::link& l) {
  std::uint8_t one = 0;
  auto n = l.read_some({&one, 1}, 3000);
  return !n || *n == 0;
}

}  // namespace

TEST_CASE("driftwire relay: a rendezvous hello carries its role and token, and a waiter's proves the key",
          "[driftwire]") {
  const auto t = relay::random_token();
  CHECK(relay::random_token() != t);
  const auto h = relay::sender_hello(t);
  CHECK(h.size() == relay::kHelloBytes);
  auto back = relay::parse_hello(h);
  REQUIRE(back);
  CHECK(back->role == relay::kSender);
  CHECK(back->t == t);
  for (std::size_t i = 0; i < 8; ++i) {
    auto bad = h;
    bad[i] ^= 2;
    CHECK_FALSE(relay::parse_hello(bad));  // magic, version, role, reserved
  }
  CHECK_FALSE(relay::parse_hello(std::span(h).first(h.size() - 1)));
  const auto w = relay::waiter_hello(t, kKey);
  CHECK(w.size() == relay::kWaiterHelloBytes);
  auto wb = relay::parse_hello(std::span<const std::uint8_t>(w).first(relay::kHelloBytes));
  REQUIRE(wb);
  CHECK(wb->role == relay::kWaiter);
  const auto p = relay::prove(kKey, t);
  CHECK(std::equal(p.begin(), p.end(), w.begin() + relay::kHelloBytes));
  CHECK(relay::prove("another key, long enough", t) != p);
  CHECK(relay::prove(kKey, relay::random_token()) != p);
  const auto k = relay::random_key();
  CHECK(k.size() == 26);
  CHECK(k.size() >= relay::kMinKeyChars);
  CHECK(relay::random_key() != k);
}

TEST_CASE("driftwire relay: a code carries the relay place and its token, after the places", "[driftwire]") {
  invite::relay_place r;
  r.at = {"203.0.113.9", 47614};
  r.token = relay::random_token();
  const std::vector<sp::net::endpoint> places{
      {"2001:db8::1", 4000}, {"198.51.100.2", 4001}, {"198.51.100.3", 4002}, {"198.51.100.4", 4003}};
  const auto c = invite::make(places, r);
  REQUIRE(c.relay);
  CHECK(c.places.size() == invite::kMaxPlaces - 1);  // the relay takes one of the four
  const std::string text = invite::encode(c);
  CHECK(text.find("203") == std::string::npos);
  auto back = invite::decode(text);
  REQUIRE(back);
  REQUIRE(back->relay);
  CHECK(back->relay->at.address == "203.0.113.9");
  CHECK(back->relay->at.port == 47614);
  CHECK(back->relay->token == r.token);
  CHECK(back->places.size() == 3);
  CHECK(back->secret == c.secret);
  auto via_link = invite::decode(invite::link(c));
  REQUIRE(via_link);
  CHECK(via_link->relay->token == r.token);
  // Only the relay, IPv6: still a code.
  invite::relay_place r6 = r;
  r6.at = {"2001:db8::77", 9};
  const auto only = invite::make({}, r6);
  auto only_back = invite::decode(invite::encode(only));
  REQUIRE(only_back);
  CHECK(only_back->places.empty());
  REQUIRE(only_back->relay);
  CHECK(only_back->relay->at.address == "2001:db8::77");
  // A code without one is as before.
  auto plain = invite::decode(invite::encode(invite::make(places)));
  REQUIRE(plain);
  CHECK_FALSE(plain->relay);
  CHECK(plain->places.size() == 4);
}

TEST_CASE("driftwire relay: the server pairs a waiter and a sender with one token, byte for byte both ways",
          "[driftwire]") {
  recorder rec;
  auto s = relay::server::start(server_options(rec.limits()));
  REQUIRE(s);
  auto [a, b] = paired((*s)->port(), relay::random_token());
  const auto there = pattern(3 * 1024 * 1024 + 11, 5);
  const auto back = pattern(700 * 1024 + 3, 6);
  std::thread writer([&, &a = a] { REQUIRE(a->write_all(there)); });
  REQUIRE(b->write_all(back));
  CHECK(read_n(*b, there.size()) == there);
  CHECK(read_n(*a, back.size()) == back);
  writer.join();
  CHECK((*s)->snapshot().paired == 1);
  CHECK(rec.size() == there.size() + back.size());
  // One side going ends the splice for the other.
  a->shutdown();
  a.reset();
  CHECK(closed_soon(*b));
  REQUIRE(eventually([&] { return (*s)->snapshot().active == 0; }));
  CHECK((*s)->snapshot().bytes == there.size() + back.size());
  // A server without a key does not start.
  relay::server::options keyless = server_options();
  keyless.key = "short";
  CHECK_FALSE(relay::server::start(keyless));
}

TEST_CASE("driftwire relay: only a key holder waits; senders never wait; a waiter again replaces itself",
          "[driftwire]") {
  auto s = relay::server::start(server_options());
  REQUIRE(s);
  const auto port = (*s)->port();
  // A waiter with the wrong key: closed, and nobody waits.
  auto wrong = waiter(port, relay::random_token(), "not the server's key at all");
  CHECK(closed_soon(*wrong));
  CHECK((*s)->snapshot().waiting == 0);
  // A sender with no waiter for its token: closed at once (it never waits).
  const auto t0 = std::chrono::steady_clock::now();
  auto lone = relay::join(dial(port), relay::random_token(), 5000, {});
  CHECK_FALSE(lone);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  // A waiter registering again under its token replaces its old wait: the
  // old connection is closed, and a sender pairs with the new one.
  const auto t = relay::random_token();
  auto first = waiter(port, t);
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == 1; }));
  auto second = waiter(port, t);
  CHECK(closed_soon(*first));
  CHECK((*s)->snapshot().waiting == 1);
  mv::expected waited = mv::err(mv::status::internal);
  std::thread w([&] { waited = relay::wait_paired(*second, 5000, {}); });
  auto a = relay::join(dial(port), t, 5000, {});
  w.join();
  REQUIRE(a);
  REQUIRE(waited);
  // Then a second sender with that token: nobody waits under it now.
  CHECK_FALSE(relay::join(dial(port), t, 3000, {}));
  // Not a hello: closed.
  auto junk = dial(port);
  const std::uint8_t noise[relay::kHelloBytes] = {'G', 'E', 'T', ' ', '/'};
  REQUIRE(junk->write_all(noise));
  CHECK(closed_soon(*junk));
  // A waiter that speaks before it is paired: closed.
  auto eager = waiter(port, relay::random_token());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE(eager->write_all(std::span<const std::uint8_t>(noise, 4)));
  CHECK(closed_soon(*eager));
  CHECK((*s)->snapshot().refused >= 4);
  // The pair still works.
  const auto bytes = pattern(1000, 1);
  REQUIRE((*a)->write_all(bytes));
  CHECK(read_n(*second, bytes.size()) == bytes);
}

TEST_CASE("driftwire relay: one address cannot fill the server's handshakes or its waits", "[driftwire]") {
  relay::server::options o = server_options();
  o.max_waiting = 64;
  auto s = relay::server::start(o);
  REQUIRE(s);
  const auto port = (*s)->port();
  // Silent connections from one address: past kPerSource they are closed at once.
  std::vector<std::unique_ptr<sp::link>> silent;
  for (int i = 0; i < relay::server::kPerSource; ++i) silent.push_back(dial(port));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto extra = dial(port);
  CHECK(closed_soon(*extra));
  silent.clear();
  // Key holders' waits from one address: kPerSource at most.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::vector<std::unique_ptr<sp::link>> waits;
  for (int i = 0; i < relay::server::kPerSource; ++i) waits.push_back(waiter(port, relay::random_token()));
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == static_cast<std::uint64_t>(relay::server::kPerSource); }));
  auto over = waiter(port, relay::random_token());
  CHECK(closed_soon(*over));
  CHECK((*s)->snapshot().waiting == static_cast<std::uint64_t>(relay::server::kPerSource));
}

TEST_CASE("driftwire relay: a splice ends at its byte cap, and a rate cap paces it", "[driftwire]") {
  {
    auto [a1, a2] = loopback_pair();
    auto [b1, b2] = loopback_pair();
    relay::limits lim;
    lim.max_bytes = 256 * 1024;
    relay::splice_result r;
    std::thread t([&, &a2 = a2, &b1 = b1] { r = relay::splice(*a2, *b1, lim, {}); });
    const auto big = pattern(1024 * 1024, 2);
    (void)a1->write_all(big);  // may fail part-way: the relay hangs up
    std::vector<std::uint8_t> got(64 * 1024);
    std::size_t total = 0;
    for (;;) {
      auto n = b2->read_some(got, 5000);
      if (!n || *n == 0) break;
      total += *n;
    }
    t.join();
    CHECK(r.why == relay::ended::capped);
    CHECK(total <= lim.max_bytes);
    CHECK(r.a_to_b <= lim.max_bytes);
  }
  {
    auto [a1, a2] = loopback_pair();
    auto [b1, b2] = loopback_pair();
    relay::limits lim;
    lim.rate = 2 * 1000 * 1000;  // 2 MB/s
    relay::splice_result r;
    std::atomic<bool> stop{false};
    std::thread t([&, &a2 = a2, &b1 = b1] { r = relay::splice(*a2, *b1, lim, [&] { return stop.load(); }); });
    const auto data = pattern(1024 * 1024, 3);
    // Idle first: a cap paced from the splice's start would now let it all
    // through at once; the bucket holds a quarter second's worth.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    std::thread w([&, &a1 = a1] { (void)a1->write_all(data); });
    CHECK(read_n(*b2, data.size()) == data);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s >= 0.2);  // (1 MiB - 0.5 MB of burst) at 2 MB/s is ~0.27 s
    w.join();
    stop.store(true);
    t.join();
    CHECK(r.why == relay::ended::stopped);
  }
}

TEST_CASE("driftwire relay: a link over a channel carries bytes, and a cancel closes it", "[driftwire]") {
  auto [a, b] = channel_pair();
  std::shared_ptr<sp::channel> sa(std::move(a)), sb(std::move(b));
  relay::channel_link la(sa, nullptr, "relay"), lb(sb, nullptr, "relay");
  CHECK(std::string(la.through()) == "relay");
  const auto bytes = pattern(300 * 1024 + 9, 4);  // more than one relay_data
  std::thread w([&] { REQUIRE(la.write_all(bytes)); });
  CHECK(read_n(lb, bytes.size()) == bytes);
  w.join();
  REQUIRE(sa->send(sp::msg::cancel, std::string_view("{}")));
  std::uint8_t one = 0;
  auto n = lb.read_some({&one, 1}, 3000);
  REQUIRE(n);
  CHECK(*n == 0);
}

TEST_CASE("driftwire relay: requests and answers read with care, and only public places are reached",
          "[driftwire]") {
  const std::vector<sp::net::endpoint> to{{"2001:db8::1", 5}, {"198.51.100.7", 6}};
  CHECK(relay::parse_request(relay::request_json(to)).size() == 2);
  CHECK(relay::parse_request("{}").empty());
  CHECK(relay::parse_request("{\"places\":[{\"a\":\"nope\",\"p\":5},{\"a\":\"1.2.3.4\",\"p\":0}]}").empty());
  std::string many = "{\"places\":[";
  for (int i = 0; i < 20; ++i) many += std::string(i ? "," : "") + "{\"a\":\"198.51.100.1\",\"p\":" + std::to_string(i + 1) + "}";
  many += "]}";
  CHECK(relay::parse_request(many).size() == invite::kMaxPlaces);
  for (auto a : {relay::answer::ok, relay::answer::off, relay::answer::unreachable,
                 relay::answer::refused}) {
    CHECK(relay::parse_answer(relay::answer_json(a)) == a);
  }
  CHECK(relay::parse_answer("{\"ok\":false,\"why\":\"something new\"}") == relay::answer::refused);
  CHECK_FALSE(relay::parse_answer("{\"why\":\"off\"}"));
  CHECK_FALSE(relay::parse_answer("[]"));
  // Never into the relay's own network.
  CHECK(relay::may_reach({"8.8.8.8", 80}, false));  // only parsed: nothing connects
  CHECK(relay::may_reach({"2001:4860:4860::8888", 80}, false));
  CHECK_FALSE(relay::may_reach({"198.51.100.7", 80}, false));  // documentation
  CHECK_FALSE(relay::may_reach({"192.168.1.10", 80}, false));
  CHECK_FALSE(relay::may_reach({"10.0.0.1", 80}, false));
  CHECK_FALSE(relay::may_reach({"127.0.0.1", 80}, false));
  CHECK_FALSE(relay::may_reach({"fd00::1", 80}, false));
  CHECK_FALSE(relay::may_reach({"169.254.1.1", 80}, false));
  CHECK_FALSE(relay::may_reach({"8.8.8.8", 0}, false));
  CHECK(relay::may_reach({"127.0.0.1", 80}, true));
  CHECK(relay::may_reach({"::1", 80}, true));
  CHECK_FALSE(relay::may_reach({"192.168.1.10", 80}, true));
  // Never one of R's own addresses, nor its own IPv6 /64.
  const std::vector<sp::net::address> own{*sp::net::parse_address("2001:4860:4860::1"),
                                          *sp::net::parse_address("8.8.4.4")};
  CHECK_FALSE(relay::may_reach({"2001:4860:4860::8888", 80}, false, own));
  CHECK_FALSE(relay::may_reach({"8.8.4.4", 80}, false, own));
  CHECK(relay::may_reach({"2001:4860:4861::8888", 80}, false, own));
  CHECK(relay::may_reach({"8.8.8.8", 80}, false, own));
}

TEST_CASE("driftwire relay: R says no when it does not relay, or may not reach, and splices when it may",
          "[driftwire]") {
  // A asks R over a fresh channel; R serves on its own thread.
  struct asking {
    std::shared_ptr<sp::channel> a, r;
    relay::served out;
    std::thread rt;
    mv::result<std::unique_ptr<sp::link>> l = mv::err(mv::status::internal);
    relay::answer said = relay::answer::ok;
    asking(bool allowed, const std::vector<sp::net::endpoint>& to, bool loopback) {
      auto [ca, cr] = channel_pair();
      a = std::move(ca);
      r = std::move(cr);
      rt = std::thread([this, allowed, loopback] {
        sp::message m;
        if (!r->receive(m, 5000)) return;
        relay::serve_options o;
        o.allow_loopback = loopback;
        o.own = std::vector<sp::net::address>{};
        out = relay::serve(r, m, allowed, o);
        r->shutdown();
      });
      l = relay::open_through(a, to, nullptr, said);
    }
    ~asking() {
      a->shutdown();
      if (rt.joinable()) rt.join();
    }
  };
  {
    asking q(false, {{"198.51.100.7", 6}}, false);
    CHECK_FALSE(q.l);
    CHECK(q.said == relay::answer::off);
  }
  {
    asking q(true, {{"192.168.1.10", 6}, {"127.0.0.1", 7}}, false);
    CHECK_FALSE(q.l);
    CHECK(q.said == relay::answer::refused);
  }
  {
    // Nothing listens there (port 1: no test can bind it).
    asking q(true, {{"127.0.0.1", 1}}, true);
    CHECK_FALSE(q.l);
    CHECK(q.said == relay::answer::unreachable);
  }
  // B listens; R reaches it for A.
  auto b = sp::net::listen(true);
  REQUIRE(b);
  std::unique_ptr<sp::link> at_b;
  std::thread bt([&] {
    auto c = (*b)->accept(10000);
    if (c) at_b = std::move(*c);
  });
  asking q(true, {{"127.0.0.1", (*b)->port()}}, true);
  bt.join();
  REQUIRE(q.l);
  CHECK(q.said == relay::answer::ok);
  REQUIRE(at_b);
  auto& l = *q.l;
  CHECK(std::string(l->through()) == "relay");
  const auto to_b = pattern(200 * 1024, 8);
  const auto to_a = pattern(5000, 9);
  REQUIRE(l->write_all(to_b));
  CHECK(read_n(*at_b, to_b.size()) == to_b);
  REQUIRE(at_b->write_all(to_a));
  CHECK(read_n(*l, to_a.size()) == to_a);
  at_b->shutdown();  // B goes: R's splice ends, and A's link closes
  std::uint8_t one = 0;
  auto n = l->read_some({&one, 1}, 5000);
  CHECK((!n || *n == 0));
  q.rt.join();
  CHECK(q.out.said == relay::answer::ok);
  CHECK(q.out.spliced.a_to_b == to_b.size());
}

// ---- two engines through a relay server -------------------------------------------------

namespace {

// A MediaViewer "on the internet" on loopback. `reach`: what its connects
// may reach (the rest time out, as a network that blocks them would).
struct far_end {
  fake_host host;
  sp::engine eng;
  far_end(const fs::path& received, const std::string& name, std::function<bool(const sp::net::endpoint&)> reach)
      : host(received), eng(host, options(name, std::move(reach))) {
    eng.screen_opened("");
    eng.on_event("internet", 2, "true");
  }
  static sp::engine::options options(const std::string& name, std::function<bool(const sp::net::endpoint&)> reach) {
    sp::engine::options o;
    o.my_name = name;
    o.use_bluetooth = false;
    o.loopback_only = true;
    o.make_discovery = [] { return std::make_unique<inject_discovery>(); };
    o.internet_loopback = true;
    o.internet_mapping = std::nullopt;
    if (reach) {
      o.connect_wifi = [reach](const sp::net::endpoint& to, int ms) -> mv::result<std::unique_ptr<sp::link>> {
        if (!reach(to)) return mv::err(mv::status::timeout);
        return sp::net::connect(to, ms);
      };
    }
    return o;
  }
  std::string make_code() {
    // The receiver of codes is made on the control thread after the toggle:
    // ask until it has heard.
    std::string code;
    auto asked = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    REQUIRE(eventually([&] {
      const auto v = eng.snapshot();
      if (v.code_phase == 0 && std::chrono::steady_clock::now() - asked > std::chrono::milliseconds(300)) {
        eng.on_event("make_code", 1, "");
        asked = std::chrono::steady_clock::now();
      }
      code = v.code_text;
      return v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::ready) && !code.empty();
    }));
    return code;
  }
};

}  // namespace

TEST_CASE("driftwire relay: two computers that cannot reach each other send through a relay server, sealed",
          "[driftwire]") {
  scratch_dir dir("driftwire_relay_server");
  recorder rec;
  auto server = relay::server::start(server_options(rec.limits()));
  REQUIRE(server);
  const std::uint16_t relay_port = (*server)->port();
  far_end bea(dir / "bea_received", "Bea", {});
  // Ann's network lets out only the relay's port: Bea's own places time out.
  far_end ann(dir / "ann_received", "Ann", [relay_port](const sp::net::endpoint& to) { return to.port == relay_port; });
  bea.eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string(relay_port) + "#" + kKey + "\"");
  const std::string code = bea.make_code();
  CHECK(bea.eng.snapshot().code_how.find("your relay") != std::string::npos);
  auto decoded = invite::decode(code);
  REQUIRE(decoded);
  REQUIRE(decoded->relay);
  CHECK(decoded->relay->at.port == relay_port);

  const fs::path f = dir / "out" / "clip.mov";
  const auto bytes = pattern(2 * 1024 * 1024 + 333, 17);
  write_bytes(f, bytes);
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  ann.eng.on_event("send_code", 2, "\"" + code + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }, 30));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  CHECK(ann.eng.snapshot().out_relay == "your relay server");
  CHECK(bea.eng.snapshot().in_relay == "your relay server");
  CHECK(ann.eng.snapshot().out_check == bea.eng.snapshot().in_code);  // end to end: one session, one check
  CHECK(sp::build_tree(ann.eng.snapshot()).find("Through your relay server") != std::string::npos);
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(dir / "bea_received" / "clip.mov") == bytes);
  CHECK(rec.size() > bytes.size());   // it all went through the relay...
  CHECK_FALSE(rec.saw_any_of(bytes));  // ...and none of it in the clear
  CHECK((*server)->snapshot().paired >= 1);
}

TEST_CASE("driftwire relay: a relay server that does not answer leaves it out of the code, and says so",
          "[driftwire]") {
  scratch_dir dir("driftwire_relay_gone");
  const std::uint16_t port = 1;  // nothing listens there, and no test can bind it
  far_end bea(dir / "bea_received", "Bea", {});
  bea.eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string(port) + "#" + kKey + "\"");
  const std::string code = bea.make_code();
  auto decoded = invite::decode(code);
  REQUIRE(decoded);
  CHECK_FALSE(decoded->relay);
  CHECK(bea.eng.snapshot().code_note.find("relay server did not answer") != std::string::npos);
  // And the setting is kept with the others.
  CHECK(bea.eng.snapshot().relay_server == "127.0.0.1:" + std::to_string(port) + "#" + kKey);
  std::string why;
  auto f = sp::relay_server_of("relay.example:47614#" + std::string(kKey), why);
  REQUIRE(f);
  CHECK(f->at == "relay.example:47614");
  CHECK(f->key == kKey);
  CHECK_FALSE(sp::relay_server_of("relay.example:47614", why));
  CHECK(why.find("key") != std::string::npos);
  CHECK_FALSE(sp::relay_server_of("relay.example:47614#short", why));
  CHECK_FALSE(sp::relay_server_of("nocolon#" + std::string(kKey), why));
  CHECK(sp::relay_endpoint("127.0.0.1:47614")->port == 47614);
  CHECK(sp::relay_endpoint("[::1]:9")->address == "::1");
  CHECK_FALSE(sp::relay_endpoint("no port"));
  CHECK_FALSE(sp::relay_endpoint("host:99999"));
}

TEST_CASE("driftwire relay: the 'my computer' mark is kept in the record, and a list without one is as before",
          "[driftwire]") {
  namespace known = mv::driftwire::known;
  known::device a, b;
  a.name = "Rob";
  a.key.fill(1);
  b.name = "Bea";
  b.key.fill(2);
  // No mark: the record a build before relays writes and reads.
  const auto plain = known::encode({a, b});
  CHECK(plain[0] == 1);
  auto back = known::decode(plain);
  REQUIRE(back);
  CHECK_FALSE((*back)[0].own);
  a.own = true;
  const auto marked = known::encode({a, b});
  CHECK(marked[0] == 2);
  CHECK(marked.size() == plain.size() + 2);
  back = known::decode(marked);
  REQUIRE(back);
  CHECK((*back)[0].own);
  CHECK_FALSE((*back)[1].own);
  CHECK((*back)[1].name == "Bea");
  auto odd = marked;
  odd.back() = 0x80;  // a flag this build does not know
  CHECK_FALSE(known::decode(odd));
  for (std::size_t cut = 0; cut < marked.size(); ++cut) CHECK_FALSE(known::decode(std::span(marked).first(cut)));
}
