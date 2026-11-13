// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire's relays (issue #472, options 9 and 10; docs/design/28
// "Relays"): the rendezvous and the splice (splice.h), a code that carries a
// relay place, the own-device relay's requests and answers (relay.h), and two
// engines that cannot reach each other meeting through a relay server run in
// process, its bytes ciphertext only. #500: nothing on the wire to a relay
// server opens again (replay), the token and the key are never on it, a
// server that does not prove the key gets no token, a refused registration
// says so, and R never relays into its own networks or carries anything but
// an internet hello. The three-engine own-device case, which
// needs the stand-in DHT, is in test_driftwire_dht.cpp.
#include "driftwire_fixture.h"

#include <cctype>

#include "addons/driftwire/invite.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/relay.h"
#include "addons/driftwire/splice.h"
#include "addons/driftwire/tls.h"
#include "addons/driftwire/tunnel.h"

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

// A waiter registered with the key (the server said so).
std::unique_ptr<relay::waiting> waiter(std::uint16_t port, const relay::token& t, const std::string& key = kKey) {
  auto w = relay::register_waiter(dial(port), t, key, 5000);
  REQUIRE(w);
  return std::move(*w);
}

// Both sides of a rendezvous: a waiter, then a sender with its token.
std::pair<std::unique_ptr<sp::link>, std::unique_ptr<sp::link>> paired(std::uint16_t port, const relay::token& t) {
  auto b = waiter(port, t);
  mv::expected waited = mv::err(mv::status::internal);
  std::thread waits([&] { waited = relay::wait_paired(*b, 5000, {}); });
  auto a = relay::join(dial(port), t, 5000, {});
  waits.join();
  REQUIRE(a);
  REQUIRE(waited);
  return {std::move(*a), b->release()};
}

// A link that keeps every byte it writes and reads: what a machine on the
// path sees.
struct wiretap final : sp::link {
  std::unique_ptr<sp::link> inner;
  std::shared_ptr<std::vector<std::uint8_t>> sent = std::make_shared<std::vector<std::uint8_t>>();
  std::shared_ptr<std::vector<std::uint8_t>> heard = std::make_shared<std::vector<std::uint8_t>>();
  explicit wiretap(std::unique_ptr<sp::link> l) : inner(std::move(l)) {}
  mv::expected write_all(std::span<const std::uint8_t> b) override {
    sent->insert(sent->end(), b.begin(), b.end());
    return inner->write_all(b);
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override {
    auto n = inner->read_some(into, ms);
    if (n) heard->insert(heard->end(), into.begin(), into.begin() + static_cast<std::ptrdiff_t>(*n));
    return n;
  }
  void shutdown() noexcept override { inner->shutdown(); }
  sp::transport via() const noexcept override { return inner->via(); }
};

template <class Bytes, class Needle>
bool contains(const Bytes& hay, const Needle& needle) {
  return std::search(hay.begin(), hay.end(), std::begin(needle), std::end(needle)) != hay.end();
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
bool closed_soon(relay::waiting& w) { return closed_soon(w.l()); }

// Says `bytes` on a fresh connection and reads until the server closes it:
// true when it did, within the server's handshake time (and a little).
bool says_and_is_closed(std::uint16_t port, const std::vector<std::uint8_t>& bytes) {
  auto l = dial(port);
  (void)l->write_all(bytes);  // may fail part-way: the server hangs up
  std::vector<std::uint8_t> buf(4096);
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(relay::kHelloMs + 3000);
  while (std::chrono::steady_clock::now() < until) {
    auto n = l->read_some(buf, 250);
    if (!n && n.error() == mv::status::timeout) continue;
    if (!n || *n == 0) return true;  // its answers (fresh each time) may come first
  }
  return false;
}

}  // namespace

TEST_CASE("driftwire relay: a rendezvous head carries its role and version, and a token's id is not the token",
          "[driftwire]") {
  const auto t = relay::random_token();
  CHECK(relay::random_token() != t);
  const std::array<std::uint8_t, relay::kHeadBytes> h = {'M', 'V', 'R', 'L', relay::kVersion, relay::kWaiter, 0, 0};
  CHECK(relay::parse_head(h) == relay::kWaiter);
  for (std::size_t i = 0; i < h.size(); ++i) {
    auto bad = h;
    bad[i] ^= 2;
    CHECK_FALSE(relay::parse_head(bad));  // magic, version, role, reserved
  }
  auto v2 = h;
  v2[4] = 2;  // #500: the version that said the token in the clear
  CHECK_FALSE(relay::parse_head(v2));
  CHECK_FALSE(relay::parse_head(std::span(h).first(h.size() - 1)));
  const auto id = relay::token_id(t);
  CHECK(relay::token_id(t) == id);
  CHECK(relay::token_id(relay::random_token()) != id);
  CHECK_FALSE(std::equal(id.begin(), id.end(), t.begin()));
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
  // The relay's address is not in the code as text. (A bare "203" can occur
  // by chance in the encoded random token, so look for the address itself.)
  CHECK(text.find("203.0.113.9") == std::string::npos);
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
  // A waiter with the wrong key: the server proves nothing it can open, so
  // the token never goes, and nobody waits.
  auto wrong = relay::register_waiter(dial(port), relay::random_token(), "not the server's key at all", 5000);
  REQUIRE_FALSE(wrong);
  CHECK(wrong.error() == mv::status::permission_denied);
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
  const std::uint8_t noise[relay::kHeadBytes] = {'G', 'E', 'T', ' ', '/'};
  REQUIRE(junk->write_all(noise));
  CHECK(closed_soon(*junk));
  // A waiter that speaks before it is paired: closed.
  auto eager = waiter(port, relay::random_token());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE(eager->l().write_all(std::span<const std::uint8_t>(noise, 4)));
  CHECK(closed_soon(*eager));
  CHECK((*s)->snapshot().refused >= 4);
  // The pair still works.
  const auto bytes = pattern(1000, 1);
  REQUIRE((*a)->write_all(bytes));
  CHECK(read_n(second->l(), bytes.size()) == bytes);
}

TEST_CASE("driftwire relay: one address cannot fill the server's handshakes or its waits", "[driftwire]") {
  // #500: counted by network, not address: an IPv6 /64, an IPv4 /24.
  const auto src = [](const char* a) { return relay::server::source_of(*sp::net::parse_address(a)); };
  CHECK(src("2001:db8:1:2::1") == src("2001:db8:1:2:ffff::9"));
  CHECK(src("2001:db8:1:2::1") != src("2001:db8:1:3::1"));
  CHECK(src("198.51.100.7") == src("198.51.100.200"));
  CHECK(src("198.51.100.7") != src("198.51.101.7"));
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
  std::vector<std::unique_ptr<relay::waiting>> waits;
  for (int i = 0; i < relay::server::kPerSource; ++i) waits.push_back(waiter(port, relay::random_token()));
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == static_cast<std::uint64_t>(relay::server::kPerSource); }));
  // One more is refused, and says so (no registered word).
  CHECK_FALSE(relay::register_waiter(dial(port), relay::random_token(), kKey, 3000));
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
  // #500: tunnel relays and identifiers are not peers.
  CHECK_FALSE(relay::may_reach({"2001:0:4136:e378::1", 80}, false));  // Teredo
  CHECK_FALSE(relay::may_reach({"2001:2::1", 80}, false));            // benchmarking
  CHECK_FALSE(relay::may_reach({"2001:10::1", 80}, false));           // ORCHID
  CHECK_FALSE(relay::may_reach({"2001:20::1", 80}, false));           // ORCHIDv2
  CHECK_FALSE(relay::may_reach({"192.88.99.1", 80}, false));          // 6to4 relay anycast
  CHECK_FALSE(relay::may_reach({"2002:c000:204::1", 80}, false));     // 6to4
}

TEST_CASE("driftwire relay: R never relays into a network it is on, its IPv6 site, or its router's outside address",
          "[driftwire]") {
  relay::fence f;
  // R on a LAN of public IPv4 addresses (a university, an office, a static /29).
  f.own = {*sp::net::parse_address("203.0.114.10"), *sp::net::parse_address("2001:4860:4860:1a00::5")};
  f.nets = {{*sp::net::parse_address("203.0.114.0"), 24}, {*sp::net::parse_address("2001:4860:4860:1a00::"), 64}};
  f.outside = {*sp::net::parse_address("8.8.4.4")};
  CHECK_FALSE(relay::may_relay_to({"203.0.114.77", 443}, f));  // a neighbour behind the firewall
  CHECK_FALSE(relay::may_relay_to({"203.0.114.10", 443}, f));  // R itself
  CHECK(relay::may_relay_to({"203.0.115.77", 443}, f));         // the next network over: the internet
  // IPv6: R's /64, and every sibling /64 in its /56 (guest, IoT).
  CHECK_FALSE(relay::may_relay_to({"2001:4860:4860:1a00::9", 443}, f));
  CHECK_FALSE(relay::may_relay_to({"2001:4860:4860:1a07::9", 443}, f));
  CHECK_FALSE(relay::may_relay_to({"2001:4860:4860:1aff::9", 443}, f));
  CHECK(relay::may_relay_to({"2001:4860:4860:1b00::9", 443}, f));
  f.site6_bits = 64;
  CHECK(relay::may_relay_to({"2001:4860:4860:1a07::9", 443}, f));
  f.site6_bits = relay::kSite6Bits;
  // The router's outside address (hairpin to its admin page, UPnP, DNS).
  CHECK_FALSE(relay::may_relay_to({"8.8.4.4", 80}, f));
  CHECK(relay::may_relay_to({"8.8.8.8", 80}, f));
  // Never private, special or loopback; loopback only for the tests.
  CHECK_FALSE(relay::may_relay_to({"192.168.1.10", 80}, f));
  CHECK_FALSE(relay::may_relay_to({"2001:0:4136:e378::1", 80}, f));
  CHECK_FALSE(relay::may_relay_to({"127.0.0.1", 80}, f));
  CHECK_FALSE(relay::may_relay_to({"8.8.8.8", 0}, f));
  f.allow_loopback = true;
  CHECK(relay::may_relay_to({"127.0.0.1", 80}, f));
  CHECK(relay::may_relay_to({"::1", 80}, f));
  // This machine's own fence holds its addresses and networks.
  const auto mine = relay::local_fence(false);
  CHECK(mine.nets.size() >= mine.own.size() / 2);
  for (const auto& a : mine.own) {
    CHECK_FALSE(relay::may_relay_to({sp::net::address_text(a), 443}, mine));
  }
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
        o.where = relay::fence{};
        o.where->allow_loopback = loopback;
        o.hello_ms = 2000;
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
  // A's bytes start as an internet hello does (#500: R forwards nothing else).
  auto to_b = pattern(200 * 1024, 8);
  const std::uint8_t hello_start[] = {'M', 'V', 'S', 'P', 2, 'I'};
  std::copy(std::begin(hello_start), std::end(hello_start), to_b.begin());
  CHECK(sp::is_internet_hello(to_b));
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
  CHECK(q.out.forwarded);
  CHECK(q.out.spliced.a_to_b + sp::kInternetHelloBytes == to_b.size());
}

TEST_CASE("driftwire relay: R forwards nothing until A's bytes start as an internet hello", "[driftwire]") {
  const auto attempt = [](std::vector<std::uint8_t> first) {
    auto b = sp::net::listen(true);
    REQUIRE(b);
    std::vector<std::uint8_t> at_b;
    std::thread bt([&] {
      auto c = (*b)->accept(10000);
      if (!c) return;
      std::vector<std::uint8_t> buf(4096);
      for (;;) {
        auto n = (*c)->read_some(buf, 5000);
        if (!n || *n == 0) break;
        at_b.insert(at_b.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
      }
    });
    auto [ca, cr] = channel_pair();
    std::shared_ptr<sp::channel> a(std::move(ca)), r(std::move(cr));
    relay::served out;
    std::thread rt([&] {
      sp::message m;
      if (!r->receive(m, 5000)) return;
      relay::serve_options o;
      o.where = relay::fence{};
      o.where->allow_loopback = true;
      o.hello_ms = 2000;
      out = relay::serve(r, m, true, o);
      r->shutdown();
    });
    relay::answer said = relay::answer::refused;
    auto l = relay::open_through(a, {{"127.0.0.1", (*b)->port()}}, nullptr, said);
    REQUIRE(l);
    (void)(*l)->write_all(first);
    rt.join();
    a->shutdown();
    bt.join();
    return std::make_pair(out, at_b);
  };
  // Raw bytes, an HTTP request, or a protocol 1 hello: B is reached (the
  // connect), but not one byte goes onward, and the relay ends.
  std::vector<std::uint8_t> http(sp::kInternetHelloBytes + 10, ' ');
  std::memcpy(http.data(), "POST /upnp/control HTTP/1.1", 27);
  auto [junk, at_b] = attempt(http);
  CHECK_FALSE(junk.forwarded);
  CHECK(at_b.empty());
  std::vector<std::uint8_t> v1(sp::kInternetHelloBytes, 0);
  std::memcpy(v1.data(), "MVSP\x01", 5);
  auto [old, at_b1] = attempt(v1);
  CHECK_FALSE(old.forwarded);
  CHECK(at_b1.empty());
  // An internet hello's start goes on, whole.
  std::vector<std::uint8_t> hi(sp::kInternetHelloBytes + 100, 7);
  std::memcpy(hi.data(), "MVSP\x02I", 6);
  auto [ok, at_b2] = attempt(hi);
  CHECK(ok.forwarded);
  CHECK(at_b2 == hi);
}

TEST_CASE("driftwire relay: neither the token nor the key is ever on the wire to the server", "[driftwire]") {
  auto s = relay::server::start(server_options());
  REQUIRE(s);
  const auto port = (*s)->port();
  const auto t = relay::random_token();
  // The receiver's link and the sender's, as a machine on either path sees them.
  auto tap_b = std::make_unique<wiretap>(dial(port));
  auto b_sent = tap_b->sent, b_heard = tap_b->heard;
  auto b = relay::register_waiter(std::move(tap_b), t, kKey, 5000);
  REQUIRE(b);
  mv::expected waited = mv::err(mv::status::internal);
  std::thread waits([&] { waited = relay::wait_paired(**b, 5000, {}); });
  auto tap_a = std::make_unique<wiretap>(dial(port));
  auto a_sent = tap_a->sent, a_heard = tap_a->heard;
  auto a = relay::join(std::move(tap_a), t, 5000, {});
  waits.join();
  REQUIRE(a);
  REQUIRE(waited);
  const std::string key(kKey);
  for (const auto* wire : {b_sent.get(), b_heard.get(), a_sent.get(), a_heard.get()}) {
    CHECK_FALSE(wire->empty());
    CHECK_FALSE(contains(*wire, t));                   // #500: the token was plaintext
    CHECK_FALSE(contains(*wire, key));                 // the key never goes at all
  }
  CHECK_FALSE(contains(*b_sent, relay::token_id(t)));  // a waiter says the token only sealed
  CHECK(contains(*a_sent, relay::token_id(t)));        // a sender names it by its id only
  // An observer with every byte of both cannot take the slot: it holds no
  // token to join with, and the id alone opens nothing (below: a replay).
}

TEST_CASE("driftwire relay: a recorded registration or join said again opens nothing", "[driftwire]") {
  auto s = relay::server::start(server_options());
  REQUIRE(s);
  const auto port = (*s)->port();
  const auto t = relay::random_token();
  // Record a waiter's registration, every byte it sent.
  auto tap = std::make_unique<wiretap>(dial(port));
  auto recorded_waiter = tap->sent;
  auto first = relay::register_waiter(std::move(tap), t, kKey, 5000);
  REQUIRE(first);
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == 1; }));
  const auto refused_before = (*s)->snapshot().refused;
  // #500 item 1: said again from elsewhere, as often as anyone likes, it never
  // registers: the server's answer is fresh each time, so the old reply does
  // not open. The real wait is untouched (not replaced).
  for (int i = 0; i < 3; ++i) CHECK(says_and_is_closed(port, *recorded_waiter));
  CHECK((*s)->snapshot().waiting == 1);
  CHECK((*s)->snapshot().refused >= refused_before + 3);
  // A sender's join, recorded, then said again against a fresh wait under the
  // same token: no pair (the wait is still there for the real sender).
  mv::expected waited = mv::err(mv::status::internal);
  std::thread w1([&] { waited = relay::wait_paired(**first, 5000, {}); });
  auto tap_a = std::make_unique<wiretap>(dial(port));
  auto recorded_sender = tap_a->sent;
  auto a = relay::join(std::move(tap_a), t, 5000, {});
  w1.join();
  REQUIRE(a);
  REQUIRE(waited);
  auto again = waiter(port, t);
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == 1; }));
  const auto paired_before = (*s)->snapshot().paired;
  CHECK(says_and_is_closed(port, *recorded_sender));
  CHECK((*s)->snapshot().waiting == 1);
  CHECK((*s)->snapshot().paired == paired_before);
  // And the id alone (what a path to the sender shows) is no sender either.
  std::vector<std::uint8_t> id_only = {'M', 'V', 'R', 'L', relay::kVersion, relay::kSender, 0, 0};
  const auto id = relay::token_id(t);
  id_only.insert(id_only.end(), id.begin(), id.end());
  std::vector<std::uint8_t> garbage(2 + 48, 0);
  garbage[0] = 48;
  id_only.insert(id_only.end(), garbage.begin(), garbage.end());
  CHECK(says_and_is_closed(port, id_only));
  CHECK((*s)->snapshot().waiting == 1);
  // The real sender still pairs.
  mv::expected waited2 = mv::err(mv::status::internal);
  std::thread w2([&] { waited2 = relay::wait_paired(*again, 5000, {}); });
  auto a2 = relay::join(dial(port), t, 5000, {});
  w2.join();
  CHECK(a2);
  CHECK(waited2);
}

TEST_CASE("driftwire relay: a server that does not hold the key gets one guess and no token", "[driftwire]") {
  // A name that resolves to someone else's server (a spoofed DNS answer): it
  // runs the protocol, but on another key.
  relay::server::options o = server_options();
  o.key = "AN-IMPOSTOR-KEY-NOT-YOURS";
  auto impostor = relay::server::start(o);
  REQUIRE(impostor);
  const auto t = relay::random_token();
  auto tap = std::make_unique<wiretap>(dial((*impostor)->port()));
  auto sent = tap->sent;
  auto w = relay::register_waiter(std::move(tap), t, kKey, 5000);
  REQUIRE_FALSE(w);
  CHECK(w.error() == mv::status::permission_denied);
  CHECK((*impostor)->snapshot().waiting == 0);
  // What went: the head, the session id, one CPace message. No token, no
  // second message, nothing a guess at the key can be checked against.
  CHECK(sent->size() == relay::kHeadBytes + relay::kSessionIdBytes + 32);
  CHECK_FALSE(contains(*sent, t));
}

TEST_CASE("driftwire relay: a registration the server refuses says so, and the old wait still pairs",
          "[driftwire]") {
  auto s = relay::server::start(server_options());
  REQUIRE(s);
  const auto port = (*s)->port();
  const auto t = relay::random_token();
  auto old_wait = waiter(port, t);
  REQUIRE(eventually([&] { return (*s)->snapshot().waiting == 1; }));
  // #500 item 5: the server is full of handshakes from this network, so the
  // new registration is closed. It is not taken for a success (the old wait
  // would be dropped for nothing): register_waiter fails.
  std::vector<std::unique_ptr<sp::link>> silent;
  for (int i = 0; i < relay::server::kPerSource; ++i) silent.push_back(dial(port));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_FALSE(relay::register_waiter(dial(port), t, kKey, 3000));
  silent.clear();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // their handshakes see the close
  // The old one is still the wait: a sender pairs with it.
  mv::expected waited = mv::err(mv::status::internal);
  std::thread w([&] { waited = relay::wait_paired(*old_wait, 8000, {}); });
  auto a = relay::join(dial(port), t, 8000, {});
  w.join();
  CHECK(a);
  CHECK(waited);
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

TEST_CASE("driftwire relay: the relay server's key is kept in the secret store, never the settings file",
          "[driftwire]") {
  scratch_dir dir("driftwire_relay_key_kept");
  bus b;
  const std::string field = "relay.example:47614#" + std::string(kKey);
  {
    client c(b, dir / "in", "Bea", dir / "secrets", dir / "data");
    c.eng.on_event("relay_server", 2, "\"" + field + "\"");
    CHECK(c.eng.snapshot().relay_server == field);
  }
  const auto file = read_bytes(dir / "data" / sp::kPrefsFile);
  const std::string text(file.begin(), file.end());
  CHECK(text.find("relay.example:47614") != std::string::npos);
  CHECK(text.find(kKey) == std::string::npos);
  {
    client c(b, dir / "in", "Bea", dir / "secrets", dir / "data");  // the next launch
    CHECK(c.eng.snapshot().relay_server == field);
    c.eng.on_event("relay_server", 2, "\"\"");  // cleared: the key goes too
  }
  auto store = sp::secrets::file_store(utf8(dir / "secrets"));
  CHECK_FALSE(store->get(mv::driftwire::known::kRelayKeyName));
}

// ---- the Tunnel: forwarding on the relay server (docs/plans/driftwire-menu-and-tunnel.md) ----

namespace {

// A TCP server on loopback that echoes what each connection says, upper-cased
// (so an echo is not mistaken for the bytes going one way only).
struct echo_server {
  std::unique_ptr<sp::net::listener> l;
  std::thread t;
  std::atomic<bool> stop{false};
  std::vector<std::thread> conns;
  echo_server() {
    auto x = sp::net::listen(true);
    REQUIRE(x);
    l = std::move(*x);
    t = std::thread([this] {
      while (!stop.load()) {
        auto c = l->accept(200);
        if (!c) {
          if (c.error() == mv::status::cancelled) return;
          continue;
        }
        std::shared_ptr<sp::link> one(std::move(*c));
        conns.emplace_back([this, one] {
          std::vector<std::uint8_t> b(4096);
          while (!stop.load()) {
            auto n = one->read_some(b, 200);
            if (!n && n.error() == mv::status::timeout) continue;
            if (!n || *n == 0) break;
            for (std::size_t i = 0; i < *n; ++i) b[i] = static_cast<std::uint8_t>(std::toupper(b[i]));
            if (!one->write_all(std::span<const std::uint8_t>(b.data(), *n))) break;
          }
          one->shutdown();
        });
      }
    });
  }
  ~echo_server() {
    stop.store(true);
    l->shutdown();
    t.join();
    for (auto& c : conns) c.join();
  }
  std::uint16_t port() const { return l->port(); }
};

relay::server::options forward_options(std::uint16_t allowed_port) {
  auto o = server_options();
  o.forward_ports = {allowed_port};
  o.forward_private = true;  // the echo server is on loopback
  return o;
}

}  // namespace

TEST_CASE("driftwire relay: a key holder's forward reaches the host, sealed both ways", "[driftwire]") {
  echo_server echo;
  auto s = relay::server::start(forward_options(echo.port()));
  REQUIRE(s);
  auto tap = std::make_unique<wiretap>(dial((*s)->port()));
  auto sent = tap->sent, heard = tap->heard;
  relay::forward_answer said{};
  auto f = relay::forward(std::move(tap), kKey, "127.0.0.1", echo.port(), relay::kForwardWaitMs, said);
  REQUIRE(f);
  CHECK(said == relay::forward_answer::ok);
  CHECK(std::string((*f)->through()) == "relay");
  // More than one sealed frame each way.
  std::string ask = "GET / HTTP/1.1\r\nHost: blocked.example\r\n\r\n";
  while (ask.size() < 3 * relay::kSealedChunk) ask += "padding-padding-padding-";
  REQUIRE((*f)->write_all({reinterpret_cast<const std::uint8_t*>(ask.data()), ask.size()}));
  const auto back = read_n(**f, ask.size());
  std::string upper = ask;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  CHECK(std::string(back.begin(), back.end()) == upper);
  // A network on the path sees neither the host asked for, nor what went.
  const std::string host = "127.0.0.1", site = "blocked.example", key = kKey;
  CHECK_FALSE(contains(*sent, host));
  CHECK_FALSE(contains(*sent, site));
  CHECK_FALSE(contains(*heard, std::string("BLOCKED.EXAMPLE")));
  CHECK_FALSE(contains(*sent, key));
  (*f)->shutdown();
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((*s)->snapshot().forwarding > 0 && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto c = (*s)->snapshot();
  CHECK(c.forwarded == 1);
  CHECK(c.forwarding == 0);
  CHECK(c.bytes >= 2 * ask.size());
}

TEST_CASE("driftwire relay: no forward without the key, to another port, past the cap, or into a private network",
          "[driftwire]") {
  echo_server echo;
  relay::forward_answer said{};
  SECTION("the wrong key: the server proves nothing and hears no host") {
    auto s = relay::server::start(forward_options(echo.port()));
    REQUIRE(s);
    CHECK_FALSE(relay::forward(dial((*s)->port()), "not the server's key at all", "127.0.0.1", echo.port(), 5000,
                               said));
    CHECK(said == relay::forward_answer::key);
    CHECK((*s)->snapshot().forwarded == 0);
  }
  SECTION("a port it does not forward to") {
    auto s = relay::server::start(forward_options(echo.port()));
    REQUIRE(s);
    const std::uint16_t other = echo.port() == 443 ? 444 : 443;
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "127.0.0.1", other, 5000, said));
    CHECK(said == relay::forward_answer::port);
  }
  SECTION("forwarding off") {
    auto o = forward_options(echo.port());
    o.forward_ports.clear();
    auto s = relay::server::start(o);
    REQUIRE(s);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "127.0.0.1", echo.port(), 5000, said));
    CHECK(said == relay::forward_answer::off);
  }
  SECTION("by default, only public addresses: loopback and a private name are refused") {
    auto o = forward_options(echo.port());
    o.forward_private = false;
    auto s = relay::server::start(o);
    REQUIRE(s);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "127.0.0.1", echo.port(), 5000, said));
    CHECK(said == relay::forward_answer::not_public);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "192.168.1.1", echo.port(), 5000, said));
    CHECK(said == relay::forward_answer::not_public);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "localhost", echo.port(), 5000, said));
    CHECK(said == relay::forward_answer::not_public);
  }
  SECTION("the cap: a second forward while the first is open is busy") {
    auto o = forward_options(echo.port());
    o.max_forwards = 1;
    auto s = relay::server::start(o);
    REQUIRE(s);
    auto first = relay::forward(dial((*s)->port()), kKey, "127.0.0.1", echo.port(), 5000, said);
    REQUIRE(first);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "127.0.0.1", echo.port(), 5000, said));
    CHECK(said == relay::forward_answer::busy);
  }
  SECTION("nothing listening there") {
    auto o = forward_options(1);  // nothing listens on loopback port 1
    auto s = relay::server::start(o);
    REQUIRE(s);
    CHECK_FALSE(relay::forward(dial((*s)->port()), kKey, "127.0.0.1", 1, relay::kForwardWaitMs, said));
    CHECK(said == relay::forward_answer::unreachable);
  }
}

TEST_CASE("driftwire relay: a forward's host is checked, and a sealed frame that does not open closes it",
          "[driftwire]") {
  CHECK(relay::forward_host_ok("example.com"));
  CHECK(relay::forward_host_ok("2001:db8::1"));
  CHECK_FALSE(relay::forward_host_ok(""));
  CHECK_FALSE(relay::forward_host_ok("bad host"));
  CHECK_FALSE(relay::forward_host_ok("a/b"));
  CHECK_FALSE(relay::forward_host_ok(std::string(relay::kMaxHostBytes + 1, 'a')));
  echo_server echo;
  auto s = relay::server::start(forward_options(echo.port()));
  REQUIRE(s);
  // A recorded forward said again opens nothing: the server's fresh
  // ephemeral makes the old answer useless (the connection is closed).
  auto tap = std::make_unique<wiretap>(dial((*s)->port()));
  auto sent = tap->sent;
  relay::forward_answer said{};
  auto f = relay::forward(std::move(tap), kKey, "127.0.0.1", echo.port(), 5000, said);
  REQUIRE(f);
  (*f)->shutdown();
  CHECK(says_and_is_closed((*s)->port(), *sent));
  CHECK((*s)->snapshot().forwarded == 1);
}

// ---- the Tunnel: the local proxy (tunnel.h) and the engine's switch -------------------------

namespace tunnel = mv::driftwire::tunnel;

namespace {

// What a client reads up to the end of a proxy's HTTP answer (its head).
std::string read_head(sp::link& l) {
  std::string got;
  std::uint8_t b = 0;
  while (got.find("\r\n\r\n") == std::string::npos && got.size() < 4096) {
    if (!sp::read_exact(l, {&b, 1}, 10000)) break;
    got.push_back(static_cast<char>(b));
  }
  return got;
}

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

void say(sp::link& l, const std::string& s) {
  REQUIRE(l.write_all({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}));
}

std::string hear(sp::link& l, std::size_t n) {
  const auto b = read_n(l, n);
  return {b.begin(), b.end()};
}

// curl -p (HTTP CONNECT) through the proxy at `proxy_port` to `host:port`,
// then a line both ways: true when the echo came back.
bool curl_connect(std::uint16_t proxy_port, const std::string& authority, std::string* status = nullptr) {
  auto l = dial(proxy_port);
  say(*l, "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\nUser-Agent: curl/8.7.1\r\n"
          "Proxy-Connection: Keep-Alive\r\n\r\n");
  const auto head = read_head(*l);
  if (status) *status = head.substr(0, head.find("\r\n"));
  if (head.rfind("HTTP/1.1 200", 0) != 0) return false;
  const std::string hello = "hello through the tunnel";
  say(*l, hello);
  return hear(*l, hello.size()) == upper(hello);
}

// As curl_connect, but false (never a failed REQUIRE) when any step fails:
// for a proxy that may be restarting.
bool try_curl_connect(std::uint16_t proxy_port, const std::string& authority) {
  auto c = sp::net::connect({"127.0.0.1", proxy_port}, 1000);
  if (!c) return false;
  auto& l = **c;
  const std::string ask = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n";
  if (!l.write_all({reinterpret_cast<const std::uint8_t*>(ask.data()), ask.size()})) return false;
  std::string head;
  std::uint8_t b = 0;
  while (head.find("\r\n\r\n") == std::string::npos && head.size() < 4096) {
    if (!sp::read_exact(l, {&b, 1}, 10000)) return false;
    head.push_back(static_cast<char>(b));
  }
  if (head.rfind("HTTP/1.1 200", 0) != 0) return false;
  const std::string hello = "hello again";
  if (!l.write_all({reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size()})) return false;
  std::vector<std::uint8_t> back(hello.size());
  if (!sp::read_exact(l, back, 10000)) return false;
  return std::string(back.begin(), back.end()) == upper(hello);
}

}  // namespace

TEST_CASE("driftwire tunnel: proxy requests are read with care", "[driftwire]") {
  auto t = tunnel::parse_authority("example.com:443", 0);
  REQUIRE(t);
  CHECK(t->host == "example.com");
  CHECK(t->port == 443);
  t = tunnel::parse_authority("[2001:db8::1]:8443", 0);
  REQUIRE(t);
  CHECK(t->host == "2001:db8::1");
  CHECK(t->port == 8443);
  CHECK(tunnel::parse_authority("example.com", 80)->port == 80);
  CHECK_FALSE(tunnel::parse_authority("example.com", 0));
  CHECK_FALSE(tunnel::parse_authority("example.com:0", 0));
  CHECK_FALSE(tunnel::parse_authority("example.com:99999", 0));
  CHECK_FALSE(tunnel::parse_authority("bad host:443", 0));

  auto c = tunnel::parse_http_head("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");
  REQUIRE(c);
  CHECK(c->connect);
  CHECK(c->to.host == "example.com");
  CHECK(c->to.port == 443);
  auto g = tunnel::parse_http_head(
      "GET http://user:pw@example.com/a/b?q=1 HTTP/1.1\r\nHost: example.com\r\nProxy-Connection: keep-alive\r\n"
      "Proxy-Authorization: Basic eA==\r\nAccept: */*\r\n\r\n");
  REQUIRE(g);
  CHECK_FALSE(g->connect);
  CHECK(g->to.host == "example.com");
  CHECK(g->to.port == 80);
  CHECK(g->head == "GET /a/b?q=1 HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nConnection: close\r\n\r\n");
  CHECK(tunnel::parse_http_head("GET http://example.com:8080 HTTP/1.1\r\n\r\n")->head.rfind("GET / HTTP/1.1", 0) == 0);
  CHECK_FALSE(tunnel::parse_http_head("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"));  // not a proxy request
  CHECK_FALSE(tunnel::parse_http_head("GET https://example.com/ HTTP/1.1\r\n\r\n"));     // https goes by CONNECT
  CHECK_FALSE(tunnel::parse_http_head("CONNECT example.com HTTP/1.1\r\n\r\n"));          // no port
  CHECK_FALSE(tunnel::parse_http_head("hello\r\n\r\n"));

  std::size_t needed = 0;
  const std::vector<std::uint8_t> name = {5, 1, 0, 3, 11, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', 1, 187};
  auto s = tunnel::parse_socks_request(name, needed);
  REQUIRE(s);
  CHECK(s->reply == 0);
  CHECK(s->to.host == "example.com");
  CHECK(s->to.port == 443);
  CHECK_FALSE(tunnel::parse_socks_request(std::span(name).first(7), needed));
  CHECK(needed == name.size());
  const std::vector<std::uint8_t> v4 = {5, 1, 0, 1, 93, 184, 216, 34, 0, 80};
  s = tunnel::parse_socks_request(v4, needed);
  REQUIRE(s);
  CHECK(s->to.host == "93.184.216.34");
  CHECK(s->to.port == 80);
  const std::vector<std::uint8_t> bind = {5, 2, 0, 1, 1, 2, 3, 4, 0, 80};
  CHECK(tunnel::parse_socks_request(bind, needed)->reply == 7);
  CHECK(tunnel::parse_socks_request(std::vector<std::uint8_t>{5, 1, 0, 9}, needed)->reply == 8);
  CHECK(tunnel::socks_reply_for(relay::forward_answer::ok) == 0);
  CHECK(tunnel::socks_reply_for(relay::forward_answer::port) == 2);
  CHECK(tunnel::http_reply_for(relay::forward_answer::ok).rfind("HTTP/1.1 200", 0) == 0);
  CHECK(tunnel::http_reply_for(relay::forward_answer::port).rfind("HTTP/1.1 403", 0) == 0);
  CHECK(tunnel::relay_shown(std::string("relay.example:47614#") + kKey) == "relay.example:47614");
  CHECK(tunnel::relay_shown("relay.example:47614") == "");
}

TEST_CASE("driftwire tunnel: SOCKS5, CONNECT and a plain request go through the relay server", "[driftwire]") {
  echo_server echo;
  auto s = relay::server::start(forward_options(echo.port()));
  REQUIRE(s);
  tunnel::proxy::options o;
  o.relay = "127.0.0.1:" + std::to_string((*s)->port()) + "#" + kKey;
  auto p = tunnel::proxy::start(o);
  REQUIRE(p);
  const auto pp = (*p)->port();
  REQUIRE(pp != 0);
  const std::string to = "127.0.0.1:" + std::to_string(echo.port());

  // HTTP CONNECT, as curl -p or a browser's HTTPS.
  std::string status;
  CHECK(curl_connect(pp, to, &status));
  CHECK(status == "HTTP/1.1 200 Connection established");

  // SOCKS5 with a name (resolved at the relay: socks5h).
  {
    auto l = dial(pp);
    say(*l, std::string("\x05\x01\x00", 3));
    CHECK(hear(*l, 2) == std::string("\x05\x00", 2));
    std::string req("\x05\x01\x00\x03", 4);
    req.push_back(static_cast<char>(9));
    req += "localhost";
    req.push_back(static_cast<char>(echo.port() >> 8));
    req.push_back(static_cast<char>(echo.port() & 0xFF));
    say(*l, req);
    const auto reply = hear(*l, 10);
    CHECK(reply[0] == 5);
    CHECK(reply[1] == 0);
    say(*l, "socks bytes");
    CHECK(hear(*l, 11) == "SOCKS BYTES");
  }
  // A SOCKS5 client that offers no method this proxy takes.
  {
    auto l = dial(pp);
    say(*l, std::string("\x05\x01\x02", 3));  // username/password only
    CHECK(hear(*l, 2) == std::string("\x05\xFF", 2));
    CHECK(closed_soon(*l));
  }
  // A plain http:// request (HTTP_PROXY): sent on in origin form, closed after.
  {
    auto l = dial(pp);
    say(*l, "GET http://" + to + "/page HTTP/1.1\r\nHost: " + to + "\r\nProxy-Connection: keep-alive\r\n\r\n");
    const std::string sent = "GET /page HTTP/1.1\r\nHost: " + to + "\r\nConnection: close\r\n\r\n";
    CHECK(hear(*l, sent.size()) == upper(sent));
  }
  // A port the relay does not forward to: 403, and counted as the site's,
  // not the tunnel's problem.
  CHECK_FALSE(curl_connect(pp, "127.0.0.1:1", &status));
  CHECK(status.rfind("HTTP/1.1 403", 0) == 0);
  const auto st = (*p)->state();
  CHECK(st.streams == 3);
  CHECK(st.failed == 1);
  CHECK(st.problem.empty());
  CHECK((*s)->snapshot().forwarded == 3);
}

TEST_CASE("driftwire tunnel: a relay that does not take the key, or does not forward, is said", "[driftwire]") {
  echo_server echo;
  auto o = forward_options(echo.port());
  o.forward_ports.clear();
  auto s = relay::server::start(o);
  REQUIRE(s);
  const std::string to = "127.0.0.1:" + std::to_string(echo.port());
  {
    tunnel::proxy::options po;
    po.relay = "127.0.0.1:" + std::to_string((*s)->port()) + "#not-the-servers-key-at-all";
    auto p = tunnel::proxy::start(po);
    REQUIRE(p);
    std::string status;
    CHECK_FALSE(curl_connect((*p)->port(), to, &status));
    CHECK(status.rfind("HTTP/1.1 502", 0) == 0);
    CHECK((*p)->state().problem.find("key") != std::string::npos);
  }
  {
    tunnel::proxy::options po;
    po.relay = "127.0.0.1:" + std::to_string((*s)->port()) + "#" + kKey;
    auto p = tunnel::proxy::start(po);
    REQUIRE(p);
    CHECK_FALSE(curl_connect((*p)->port(), to));
    CHECK((*p)->state().problem.find("does not forward") != std::string::npos);
  }
  CHECK_FALSE(tunnel::proxy::start({}));  // no relay: nothing listens
}

TEST_CASE("driftwire tunnel: two engines and a relay; the switch runs the proxy beside a relayed send",
          "[driftwire]") {
  scratch_dir dir("driftwire_tunnel");
  echo_server echo;
  auto o = forward_options(echo.port());
  auto server = relay::server::start(o);
  REQUIRE(server);
  const std::uint16_t relay_port = (*server)->port();
  const std::string field = "\"127.0.0.1:" + std::to_string(relay_port) + "#" + kKey + "\"";
  far_end bea(dir / "bea_received", "Bea", {});
  far_end ann(dir / "ann_received", "Ann", [relay_port](const sp::net::endpoint& to) { return to.port == relay_port; });
  bea.eng.on_event("relay_server", 2, field);
  ann.eng.on_event("relay_server", 2, field);

  // Off by default: nothing listens, and the window offers the switch.
  CHECK_FALSE(ann.eng.snapshot().tunnel);
  CHECK(ann.eng.snapshot().tunnel_address.empty());
  const auto off_tree = sp::build_tree(ann.eng.snapshot());
  CHECK(off_tree.find("\"id\":\"tunnel\"") != std::string::npos);
  CHECK(off_tree.find("Tunnel through 127.0.0.1:" + std::to_string(relay_port)) != std::string::npos);

  // The window's switch (the menu-bar row sends the same event).
  ann.eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !ann.eng.snapshot().tunnel_address.empty(); }));
  const auto v = ann.eng.snapshot();
  CHECK(v.tunnel);
  CHECK(v.tunnel_relay == "127.0.0.1:" + std::to_string(relay_port));
  CHECK(v.tunnel_address.rfind("127.0.0.1:", 0) == 0);
  const auto proxy_port = static_cast<std::uint16_t>(std::stoi(v.tunnel_address.substr(10)));
  const auto on_tree = sp::build_tree(v);
  CHECK(on_tree.find("\"via\":\"tunnel\"") != std::string::npos);
  CHECK(on_tree.find("\"copy\":\"" + v.tunnel_address + "\"") != std::string::npos);
  CHECK(sp::build_settings_tree(v).find("\"id\":\"tunnel\"") != std::string::npos);

  // curl -p through it, while a send goes through the same relay server.
  const fs::path f = dir / "out" / "clip.mov";
  const auto bytes = pattern(512 * 1024 + 7, 3);
  write_bytes(f, bytes);
  const std::string code = bea.make_code();
  ann.eng.set_files({utf8(f)});
  REQUIRE(eventually([&] { return ann.eng.snapshot().file_count == 1; }));
  ann.eng.on_event("send_code", 2, "\"" + code + "\"");
  ann.eng.on_event("send_to_code", 1, "");
  CHECK(curl_connect(proxy_port, "127.0.0.1:" + std::to_string(echo.port())));
  {
    // A stream held open shows in the window.
    auto l = dial(proxy_port);
    say(*l, "CONNECT 127.0.0.1:" + std::to_string(echo.port()) + " HTTP/1.1\r\n\r\n");
    CHECK(read_head(*l).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(eventually([&] { return ann.eng.snapshot().tunnel_open == 1; }));
    CHECK(sp::build_tree(ann.eng.snapshot()).find("\"state\":\"moving\"") != std::string::npos);
  }
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }, 30));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  CHECK(read_bytes(dir / "bea_received" / "clip.mov") == bytes);
  CHECK(ann.eng.snapshot().tunnel_streams >= 2);
  CHECK((*server)->snapshot().forwarded >= 2);

  // Off: the port closes and the address goes.
  ann.eng.on_event("tunnel", 2, "false");
  REQUIRE(eventually([&] { return ann.eng.snapshot().tunnel_address.empty(); }));
  CHECK_FALSE(sp::net::connect({"127.0.0.1", proxy_port}, 1000));
}

TEST_CASE("driftwire tunnel: on with no relay server says why; the switch and its port are kept", "[driftwire]") {
  scratch_dir dir("driftwire_tunnel_prefs");
  const auto prefs_path = utf8(dir / "driftwire.json");
  std::uint16_t first_port = 0;
  echo_server echo;
  auto server = relay::server::start(forward_options(echo.port()));
  REQUIRE(server);
  {
    fake_host host(dir / "received");
    auto o = far_end::options("Ann", {});
    o.prefs_path = prefs_path;
    sp::engine eng(host, o);
    eng.on_event("tunnel", 2, "true");
    REQUIRE(eventually([&] { return !eng.snapshot().tunnel_note.empty(); }));
    CHECK(eng.snapshot().tunnel_address.empty());
    CHECK(sp::build_tree(eng.snapshot()).find("relay server") != std::string::npos);
    eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"");
    REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));
    first_port = static_cast<std::uint16_t>(std::stoi(eng.snapshot().tunnel_address.substr(10)));
  }
  const auto kept = sp::load_prefs(prefs_path);
  REQUIRE(kept);
  CHECK(kept->tunnel);
  CHECK(kept->tunnel_port == first_port);
  {
    // The next run listens on the same port, so a browser set up once keeps working.
    fake_host host(dir / "received");
    auto o = far_end::options("Ann", {});
    o.prefs_path = prefs_path;
    sp::engine eng(host, o);
    REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));
    CHECK(eng.snapshot().tunnel_address == "127.0.0.1:" + std::to_string(first_port));
  }
}

// ---- the Tunnel's system proxy (sysproxy.h), through a stand-in for the OS ----------------

namespace sysproxy = mv::driftwire::sysproxy;

namespace {

// The OS's setting, as a stand-in: what it is now, and every call.
struct fake_os {
  std::mutex m;
  std::string url = "http://wpad.example/proxy.pac";  // the person's own, before
  int sets = 0, restores = 0;
  bool deny = false;
  std::string now() {
    std::lock_guard lock(m);
    return url;
  }
};

struct fake_port final : sysproxy::port {
  std::shared_ptr<fake_os> os;
  explicit fake_port(std::shared_ptr<fake_os> o) : os(std::move(o)) {}
  std::string unsupported() const override { return {}; }
  mv::result<std::string> set(const std::string& pac) override {
    std::lock_guard lock(os->m);
    ++os->sets;
    if (os->deny) return mv::err(mv::status::permission_denied);
    std::string was = os->url;
    os->url = pac;
    return was;
  }
  bool restore(const std::string& saved, const std::string& pac) override {
    std::lock_guard lock(os->m);
    ++os->restores;
    if (os->url == pac) os->url = saved;  // only if still ours
    return true;
  }
};

std::string http_get(std::uint16_t port, const std::string& path) {
  auto l = dial(port);
  say(*l, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  std::string got;
  std::vector<std::uint8_t> buf(4096);
  for (;;) {
    auto n = l->read_some(buf, 5000);
    if (!n || *n == 0) break;
    got.append(reinterpret_cast<const char*>(buf.data()), *n);
  }
  return got;
}

}  // namespace

TEST_CASE("driftwire tunnel: the PAC script and the record read with care", "[driftwire]") {
  const auto script = tunnel::pac_script(54321);
  CHECK(script.find("FindProxyForURL") != std::string::npos);
  CHECK(script.find("\"SOCKS5 127.0.0.1:54321; DIRECT\"") != std::string::npos);  // DIRECT when it is gone
  CHECK(script.find("dnsResolve") == std::string::npos);  // never asks the network's DNS
  CHECK(tunnel::pac_url(54321) == "http://127.0.0.1:54321/driftwire-tunnel.pac");
  CHECK(tunnel::asks_for_pac("GET /driftwire-tunnel.pac HTTP/1.1\r\n\r\n"));
  CHECK(tunnel::asks_for_pac("GET /driftwire-tunnel.pac?x=1 HTTP/1.1\r\n\r\n"));
  CHECK_FALSE(tunnel::asks_for_pac("GET /driftwire-tunnel.pacx HTTP/1.1\r\n\r\n"));
  CHECK_FALSE(tunnel::asks_for_pac("GET http://example.com/driftwire-tunnel.pac HTTP/1.1\r\n\r\n"));
  CHECK_FALSE(tunnel::asks_for_pac("POST /driftwire-tunnel.pac HTTP/1.1\r\n\r\n"));
  const sysproxy::record r{"http://127.0.0.1:1/driftwire-tunnel.pac", "1\tsvc\t0\t\n"};
  const auto back = sysproxy::parse_record(sysproxy::record_json(r));
  REQUIRE(back);
  CHECK(back->pac_url == r.pac_url);
  CHECK(back->saved == r.saved);
  CHECK_FALSE(sysproxy::parse_record("{}"));
  CHECK_FALSE(sysproxy::parse_record("not json"));
}

TEST_CASE("driftwire tunnel: Use for all apps points the system proxy at it, and puts it back", "[driftwire]") {
  scratch_dir dir("driftwire_tunnel_sysproxy");
  echo_server echo;
  auto server = relay::server::start(forward_options(echo.port()));
  REQUIRE(server);
  auto os = std::make_shared<fake_os>();
  const std::string before = os->now();
  fake_host host(dir / "received");
  auto o = far_end::options("Ann", {});
  o.prefs_path = utf8(dir / "driftwire.json");
  o.make_sysproxy = [os] { return std::make_unique<fake_port>(os); };
  sp::engine eng(host, o);
  eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"");
  CHECK(eng.snapshot().tunnel_all_apps);  // on by default
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
  const auto v = eng.snapshot();
  const auto port = static_cast<std::uint16_t>(std::stoi(v.tunnel_address.substr(10)));
  CHECK(os->now() == tunnel::pac_url(port));
  // Kept on disk while it is Driftwire's, so a crash can put it back.
  const auto rec = sysproxy::load_record(utf8(dir / sysproxy::kRecordFile));
  REQUIRE(rec);
  CHECK(rec->pac_url == tunnel::pac_url(port));
  CHECK(rec->saved == before);
  CHECK(sp::build_tree(v).find("\"id\":\"tunnel_all_apps\"") != std::string::npos);
  // The script, from the proxy's own port.
  const auto pac = http_get(port, std::string(tunnel::kPacPath));
  CHECK(pac.rfind("HTTP/1.1 200", 0) == 0);
  CHECK(pac.find("application/x-ns-proxy-autoconfig") != std::string::npos);
  CHECK(pac.find("SOCKS5 127.0.0.1:" + std::to_string(port) + "; DIRECT") != std::string::npos);
  // More reconciles set nothing again.
  eng.screen_opened("");
  eng.screen_closed();
  CHECK(os->sets == 1);

  // Use for all apps off: back as it was; on: pointed again.
  eng.on_event("tunnel_all_apps", 2, "false");
  REQUIRE(eventually([&] { return os->now() == before; }));
  CHECK_FALSE(eng.snapshot().tunnel_system);
  CHECK_FALSE(sysproxy::load_record(utf8(dir / sysproxy::kRecordFile)));
  CHECK_FALSE(eng.snapshot().tunnel_address.empty());  // the Tunnel itself still runs
  eng.on_event("tunnel_all_apps", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
  // The Tunnel off: back as it was.
  eng.on_event("tunnel", 2, "false");
  REQUIRE(eventually([&] { return os->now() == before && eng.snapshot().tunnel_address.empty(); }));
  CHECK_FALSE(sysproxy::load_record(utf8(dir / sysproxy::kRecordFile)));

  // On, then quit: the engine's way out puts it back.
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
  // A setting the person changed since is theirs: not touched on the way out.
}

TEST_CASE("driftwire tunnel: the system proxy goes back at quit, after a crash, and never over the person's own",
          "[driftwire]") {
  scratch_dir dir("driftwire_tunnel_sysproxy_quit");
  auto os = std::make_shared<fake_os>();
  const std::string before = os->now();
  echo_server echo;
  auto server = relay::server::start(forward_options(echo.port()));
  REQUIRE(server);
  const std::string field = "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"";
  const auto make = [&] {
    auto o = far_end::options("Ann", {});
    o.prefs_path = utf8(dir / "driftwire.json");
    o.make_sysproxy = [os] { return std::make_unique<fake_port>(os); };
    return o;
  };
  SECTION("quit") {
    fake_host host(dir / "received");
    {
      sp::engine eng(host, make());
      eng.on_event("relay_server", 2, field);
      eng.on_event("tunnel", 2, "true");
      REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
    }
    CHECK(os->now() == before);
    CHECK_FALSE(sysproxy::load_record(utf8(dir / sysproxy::kRecordFile)));
  }
  SECTION("a crash left it set: the next start puts it back, Tunnel on or off") {
    const std::string left = "http://127.0.0.1:1234/driftwire-tunnel.pac";
    os->url = left;
    REQUIRE(sysproxy::save_record(utf8(dir / sysproxy::kRecordFile), {left, before}));
    fake_host host(dir / "received");
    sp::engine eng(host, make());
    REQUIRE(eventually([&] { return os->now() == before; }));
    CHECK(eventually([&] { return !sysproxy::load_record(utf8(dir / sysproxy::kRecordFile)); }));
  }
  SECTION("the person changed it while the Tunnel was on: left alone") {
    fake_host host(dir / "received");
    {
      sp::engine eng(host, make());
      eng.on_event("relay_server", 2, field);
      eng.on_event("tunnel", 2, "true");
      REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
      std::lock_guard lock(os->m);
      os->url = "http://corp.example/their.pac";
    }
    CHECK(os->now() == "http://corp.example/their.pac");
  }
}

TEST_CASE("driftwire tunnel: a declined system proxy says so once, and asks again only when the switch moves",
          "[driftwire]") {
  scratch_dir dir("driftwire_tunnel_sysproxy_denied");
  auto os = std::make_shared<fake_os>();
  os->deny = true;
  echo_server echo;
  auto server = relay::server::start(forward_options(echo.port()));
  REQUIRE(server);
  fake_host host(dir / "received");
  auto o = far_end::options("Ann", {});
  o.make_sysproxy = [os] { return std::make_unique<fake_port>(os); };
  sp::engine eng(host, o);
  eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"");
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_system_note.find("administrator") != std::string::npos; }));
  CHECK_FALSE(eng.snapshot().tunnel_system);
  CHECK_FALSE(eng.snapshot().tunnel_address.empty());  // the Tunnel runs; apps can be pointed at it by hand
  eng.screen_opened("");  // reconciles: no second prompt
  eng.screen_closed();
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_system_note.empty(); }));
  CHECK(os->sets == 1);
  {
    std::lock_guard lock(os->m);
    os->deny = false;
  }
  eng.on_event("tunnel_all_apps", 2, "false");
  eng.on_event("tunnel_all_apps", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_system; }));
  CHECK(os->sets == 2);
}

// ---- slice 4: looking like HTTPS (tls.h) ---------------------------------------------------

namespace tls = mv::driftwire::tls;

namespace {

relay::server::options tls_options(std::uint16_t echo_port) {
  auto o = forward_options(echo_port);
  o.tls_port = 1;  // loopback: the OS picks (tls_port())
  o.tls_name = "relay.example";
  return o;
}

// What a TLS client says first: the ClientHello's record, from the bytes it sent.
std::vector<std::uint8_t> first_record(const std::vector<std::uint8_t>& sent) {
  REQUIRE(sent.size() >= 5);
  const std::size_t n = 5 + (std::size_t{sent[3]} << 8 | sent[4]);
  REQUIRE(sent.size() >= n);
  return {sent.begin(), sent.begin() + static_cast<std::ptrdiff_t>(n)};
}

std::string https_get(std::uint16_t port, const std::string& request) {
  auto t = tls::client(dial(port), "relay.example", nullptr);  // a browser: no knock
  REQUIRE(t);
  say(**t, request);
  std::string got;
  std::vector<std::uint8_t> buf(4096);
  for (;;) {
    auto n = (*t)->read_some(buf, 15000);
    if (!n || *n == 0) break;
    got.append(reinterpret_cast<const char*>(buf.data()), *n);
  }
  return got;
}

}  // namespace

TEST_CASE("driftwire tls: the web site a prober sees, read with care", "[driftwire]") {
  const tls::decoy builtin;
  auto r = tls::decoy_response("GET / HTTP/1.1\r\nHost: a\r\n\r\n", builtin);
  CHECK(r.rfind("HTTP/1.1 200 OK", 0) == 0);
  CHECK(r.find("Server: nginx") != std::string::npos);
  CHECK(r.find("Welcome") != std::string::npos);
  CHECK(tls::decoy_response("GET /admin HTTP/1.1\r\n\r\n", builtin).rfind("HTTP/1.1 404", 0) == 0);
  CHECK(tls::decoy_response("POST / HTTP/1.1\r\n\r\n", builtin).rfind("HTTP/1.1 405", 0) == 0);
  CHECK(tls::decoy_response("\x16\x03\x01garbage", builtin).rfind("HTTP/1.1 400", 0) == 0);
  const auto head = tls::decoy_response("HEAD / HTTP/1.1\r\n\r\n", builtin);
  CHECK(head.find("Welcome") == std::string::npos);  // a HEAD has no body
  scratch_dir dir("driftwire_tls_decoy");
  write_bytes(dir / "site" / "index.html", std::vector<std::uint8_t>{'h', 'i'});
  write_bytes(dir / "secret.txt", std::vector<std::uint8_t>{'n', 'o'});
  const tls::decoy site{utf8(dir / "site")};
  CHECK(tls::decoy_response("GET / HTTP/1.1\r\n\r\n", site).find("\r\n\r\nhi") != std::string::npos);
  CHECK(tls::decoy_response("GET /../secret.txt HTTP/1.1\r\n\r\n", site).rfind("HTTP/1.1 404", 0) == 0);
  CHECK(tls::decoy_response("GET /%2e%2e/secret.txt HTTP/1.1\r\n\r\n", site).rfind("HTTP/1.1 404", 0) == 0);
  // The knock: keyed by the server key, one per exporter.
  const auto k1 = tls::derive_knock_key(kKey);
  const auto k2 = tls::derive_knock_key("another key entirely, 2");
  REQUIRE(k1);
  REQUIRE(k2);
  CHECK(*k1 != *k2);
  const std::array<std::uint8_t, 32> e1{1}, e2{2};
  CHECK(tls::knock_of(*k1, e1) != tls::knock_of(*k1, e2));
  CHECK(tls::knock_of(*k1, e1) != tls::knock_of(*k2, e1));
  CHECK_FALSE(tls::ja4_of(std::vector<std::uint8_t>{0x17, 3, 3, 0, 0}));
}

TEST_CASE("driftwire tls: the ClientHello is Chrome's by JA4, and the Tunnel forwards inside TLS", "[driftwire]") {
  if (!tls::available()) SKIP("this build has no BoringSSL");
  echo_server echo;
  auto s = relay::server::start(tls_options(echo.port()));
  REQUIRE(s);
  const auto tp = (*s)->tls_port();
  REQUIRE(tp != 0);
  const auto k = tls::derive_knock_key(kKey);
  REQUIRE(k);
  auto tap = std::make_unique<wiretap>(dial(tp));
  auto sent = tap->sent;
  auto t = tls::client(std::move(tap), "relay.example", &*k);
  REQUIRE(t);
  // The fingerprint a filter matches: TLS 1.3, SNI, 15 ciphers, 16
  // extensions, h2; Chrome's cipher hash.
  const auto ja4 = tls::ja4_of(first_record(*sent));
  REQUIRE(ja4);
  INFO(*ja4);
  CHECK(ja4->substr(0, 10) == "t13d1516h2");
  CHECK(ja4->substr(11, 12) == "8daaf6152771");
  // The permuted extensions: a second hello's order differs, its JA4 does not.
  {
    auto tap2 = std::make_unique<wiretap>(dial(tp));
    auto sent2 = tap2->sent;
    auto t2 = tls::client(std::move(tap2), "relay.example", &*k);
    REQUIRE(t2);
    CHECK(tls::ja4_of(first_record(*sent2)) == ja4);
  }
  // The sealed hop inside: a forward to the echo server.
  relay::forward_answer said{};
  auto f = relay::forward(std::move(*t), kKey, "127.0.0.1", echo.port(), relay::kForwardWaitMs, said);
  REQUIRE(f);
  CHECK(said == relay::forward_answer::ok);
  say(**f, "inside tls");
  CHECK(hear(**f, 10) == "INSIDE TLS");
  // Neither the key nor the host is in what went on the wire (TLS's own
  // SNI names only the relay).
  const std::string key = kKey;
  CHECK_FALSE(contains(*sent, key));
  CHECK_FALSE(contains(*sent, std::string("inside tls")));
  CHECK((*s)->snapshot().forwarded == 1);
}

TEST_CASE("driftwire tls: without the knock, the relay is an ordinary web site", "[driftwire]") {
  if (!tls::available()) SKIP("this build has no BoringSSL");
  echo_server echo;
  auto s = relay::server::start(tls_options(echo.port()));
  REQUIRE(s);
  const auto tp = (*s)->tls_port();
  // A browser.
  const auto page = https_get(tp, "GET / HTTP/1.1\r\nHost: relay.example\r\nUser-Agent: Mozilla/5.0\r\n\r\n");
  CHECK(page.rfind("HTTP/1.1 200 OK", 0) == 0);
  CHECK(page.find("Server: nginx") != std::string::npos);
  CHECK(https_get(tp, "GET /wp-admin HTTP/1.1\r\nHost: relay.example\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
  // A prober that speaks Driftwire's own hop inside TLS, without the knock:
  // a web server's 400, never a CPace answer.
  {
    auto t = tls::client(dial(tp), "relay.example", nullptr);
    REQUIRE(t);
    say(**t, std::string("MVRL\x03\x02\x00\x00", 8) + std::string(48, 'x'));
    std::string got;
    std::vector<std::uint8_t> buf(4096);
    for (;;) {
      auto n = (*t)->read_some(buf, 15000);
      if (!n || *n == 0) break;
      got.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    CHECK((got.empty() || got.rfind("HTTP/1.1 400", 0) == 0));
  }
  // A knock made with the wrong key: the same.
  {
    const auto wrong = tls::derive_knock_key("not the server's key at all");
    REQUIRE(wrong);
    auto t = tls::client(dial(tp), "relay.example", &*wrong);
    REQUIRE(t);
    relay::forward_answer said{};
    CHECK_FALSE(relay::forward(std::move(*t), kKey, "127.0.0.1", echo.port(), 15000, said));
  }
  // Plain bytes on the TLS port (not TLS at all): closed.
  CHECK(says_and_is_closed(tp, std::vector<std::uint8_t>{'G', 'E', 'T', ' ', '/', '\r', '\n', '\r', '\n'}));
  const auto c = (*s)->snapshot();
  CHECK(c.website >= 4);
  CHECK(c.forwarded == 0);
}

TEST_CASE("driftwire tls: the Tunnel's Look like a web site switch goes through the TLS port", "[driftwire]") {
  if (!tls::available()) SKIP("this build has no BoringSSL");
  scratch_dir dir("driftwire_tunnel_tls");
  echo_server echo;
  auto s = relay::server::start(tls_options(echo.port()));
  REQUIRE(s);
  fake_host host(dir / "received");
  auto o = far_end::options("Ann", {});
  o.tunnel_tls_port = (*s)->tls_port();
  sp::engine eng(host, o);
  eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string((*s)->port()) + "#" + kKey + "\"");
  CHECK(eng.snapshot().tunnel_tls_available);
  CHECK_FALSE(eng.snapshot().tunnel_tls);  // off by default
  eng.on_event("tunnel_tls", 2, "true");
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));
  CHECK(sp::build_tree(eng.snapshot()).find("\"id\":\"tunnel_tls\"") != std::string::npos);
  const auto pp = static_cast<std::uint16_t>(std::stoi(eng.snapshot().tunnel_address.substr(10)));
  CHECK(curl_connect(pp, "127.0.0.1:" + std::to_string(echo.port())));
  CHECK((*s)->snapshot().forwarded == 1);
  CHECK((*s)->snapshot().website == 0);
  // Off again: the plain port.
  eng.on_event("tunnel_tls", 2, "false");
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));
  // The proxy restarts: on the same port when it can be had again, else on
  // one the OS picks, so follow the address the window shows. Until it
  // listens a connect is refused: probe without a REQUIRE (one inside
  // eventually ends the test).
  REQUIRE(eventually([&] {
    const auto at = eng.snapshot().tunnel_address;
    if (at.size() <= 10) return false;
    const auto now = static_cast<std::uint16_t>(std::stoi(at.substr(10)));
    return try_curl_connect(now, "127.0.0.1:" + std::to_string(echo.port()));
  }));
}
