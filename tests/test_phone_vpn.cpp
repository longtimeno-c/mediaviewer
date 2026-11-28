// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's VPN through the person's own server (docs/plans/phone-vpn.md S1/S2): the C ABI the
// iPhone's app and packet tunnel extension call (src/mobile/mv_mobile_vpn.h), run here as the
// extension would run it. The test plays NEPacketTunnelFlow: packets go in through
// mv_vpn_packet_in and come out of the packet callback. Every relay is in this process, on
// loopback (§6: never a public one). The extension's Swift and iOS's own settings need a device.
#include "driftwire_fixture.h"

#include <cctype>
#include <deque>

#include "addons/vpn/netstack.h"
#include "addons/vpn/vpn.h"
#include "core/json.h"
#include "mobile/mv_mobile_vpn.h"
#include "wire/serve_wire.h"
#include "wire/splice.h"
#include "wire/tls.h"

namespace vpn = mv::driftwire::vpn;
namespace relay = mv::driftwire::relay;
namespace serve = mv::driftwire::serve;
namespace tls = mv::driftwire::tls;

namespace {

constexpr const char* kKey = "PHONE-VPN-TEST-KEY-0123456789";

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

vpn::flow v4_flow(const char* src, std::uint16_t sport, const char* dst, std::uint16_t dport, std::uint8_t proto) {
  vpn::flow f;
  f.proto = proto;
  f.src = *sp::net::parse_address(src);
  f.dst = *sp::net::parse_address(dst);
  f.sport = sport;
  f.dport = dport;
  return f;
}

vpn::flow reversed(vpn::flow f) {
  std::swap(f.src, f.dst);
  std::swap(f.sport, f.dport);
  return f;
}

// NEPacketTunnelFlow, as the test sees it: what the VPN wrote, and what it told.
struct phone_flow {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::vector<std::uint8_t>> out;  // under m
  std::vector<mv_vpn_status> events;          // under m
  static void packet(void* ctx, const std::uint8_t* p, std::size_t n) {
    auto* self = static_cast<phone_flow*>(ctx);
    {
      std::lock_guard lock(self->m);
      self->out.emplace_back(p, p + n);
    }
    self->cv.notify_all();
  }
  static void event(void* ctx, mv_vpn_status s) {
    auto* self = static_cast<phone_flow*>(ctx);
    {
      std::lock_guard lock(self->m);
      self->events.push_back(s);
    }
    self->cv.notify_all();
  }
  // The next packet of `proto` the VPN wrote (others are skipped).
  std::optional<vpn::parsed> next(std::uint8_t proto, std::vector<std::uint8_t>& keep, int ms = 5000) {
    std::unique_lock lock(m);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      if (!cv.wait_until(lock, until, [&] { return !out.empty(); })) return std::nullopt;
      keep = std::move(out.front());
      out.pop_front();
      auto p = vpn::parse(keep);
      if (p && p->f.proto == proto) return p;
    }
  }
  std::vector<mv_vpn_status> told() {
    std::lock_guard lock(m);
    return events;
  }
};

// A TCP server on loopback that answers each connection's bytes upper-cased.
struct tcp_echo {
  std::unique_ptr<sp::net::listener> l;
  std::thread t;
  std::atomic<bool> stop{false};
  std::vector<std::thread> conns;
  tcp_echo() {
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
          std::vector<std::uint8_t> b(65536);
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
  ~tcp_echo() {
    stop.store(true);
    l->shutdown();
    t.join();
    for (auto& c : conns) c.join();
  }
  std::uint16_t port() const { return l->port(); }
};

// A UDP server on loopback that answers each datagram upper-cased past its first two bytes (kept,
// as a resolver keeps a query's ID): the relay's "resolver".
struct udp_echo {
  std::unique_ptr<sp::net::udp_socket> s;
  std::thread t;
  std::atomic<bool> stop{false};
  udp_echo() {
    auto x = sp::net::open_udp(false, 1, true);
    REQUIRE(x);
    s = std::move(*x);
    t = std::thread([this] {
      std::vector<std::uint8_t> b(2048);
      while (!stop.load()) {
        sp::net::endpoint from;
        auto n = s->receive_from(b, 200, from);
        if (!n) continue;
        for (std::size_t i = 2; i < *n; ++i) b[i] = static_cast<std::uint8_t>(std::toupper(b[i]));
        (void)s->send_to(std::span<const std::uint8_t>(b.data(), *n), from);
      }
    });
  }
  ~udp_echo() {
    stop.store(true);
    s->shutdown();
    t.join();
  }
  std::uint16_t port() const { return s->port(); }
};

relay::server::options relay_options(std::uint16_t tcp_port, std::uint16_t udp_port) {
  relay::server::options o;
  o.key = kKey;
  o.loopback_only = true;
  o.forward_private = true;
  o.forward_ports = {tcp_port};
  o.forward_udp_ports = {udp_port};
  o.dns_upstream = sp::net::endpoint{"127.0.0.1", udp_port};
  return o;
}

mv_vpn_config config_for(const std::string& relay_at, const char* key, phone_flow& flow) {
  mv_vpn_config c{};
  c.relay = relay_at.c_str();
  c.key = key;
  c.packet_out = &phone_flow::packet;
  c.event = &phone_flow::event;
  c.ctx = &flow;
  return c;
}

std::string at(std::uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

// A port nothing listens on (taken, then let go).
std::uint16_t closed_port() {
  auto l = sp::net::listen(true);
  REQUIRE(l);
  const auto p = (*l)->port();
  (*l)->shutdown();
  return p;
}

struct parsed_code {
  mv_vpn_status status;
  std::string relay, key;
  std::int32_t tls = -1;
  std::uint16_t tls_port = 1;
};
parsed_code parse(const std::string& text) {
  char* r = nullptr;
  char* k = nullptr;
  parsed_code out{};
  out.status = mv_vpn_parse_code(text.c_str(), &r, &k, &out.tls, &out.tls_port);
  if (r) out.relay = r;
  if (k) out.key = k;
  mv_vpn_free(r);
  mv_vpn_free(k);
  return out;
}

}  // namespace

TEST_CASE("phone vpn: the server's VPN code is read by the laptop's own parser", "[phone-vpn][nas-vpn]") {
  const std::string line = std::string("home.duckdns.org:47614#") + kKey;
  CHECK(serve::relay_connect_code(line) == "mediaviewer://relay#" + line);
  // Pasted as the server shows it, as a link (any case), with a newline, or bare.
  for (const auto& text : {serve::relay_connect_code(line), "MediaViewer://Relay#" + line,
                           "  " + serve::relay_connect_code(line) + "\n", line}) {
    INFO(text);
    const auto p = parse(text);
    CHECK(p.status == MV_VPN_OK);
    CHECK(p.relay == "home.duckdns.org:47614");
    CHECK(p.key == kKey);
    CHECK(p.tls == 0);  // the relay's plain port
    CHECK(p.tls_port == 0);
  }
  // The code a host hands out by default (vpn-stealth): the HTTPS look, on the code's own port.
  for (const auto& text : {"mediaviewer://relay-tls#home.duckdns.org:443#" + std::string(kKey),
                           "  MediaViewer://Relay-TLS#home.duckdns.org:443#" + std::string(kKey) + "\n"}) {
    INFO(text);
    const auto p = parse(text);
    CHECK(p.status == MV_VPN_OK);
    CHECK(p.relay == "home.duckdns.org:443");
    CHECK(p.key == kKey);
    CHECK(p.tls == 1);
    CHECK(p.tls_port == 443);
  }
  const auto other = parse("mediaviewer://relay-tls#[2001:db8::7]:8443#" + std::string(kKey));
  CHECK(other.status == MV_VPN_OK);
  CHECK(other.tls == 1);
  CHECK(other.tls_port == 8443);
  const auto v6 = parse(std::string("mediaviewer://relay#[2001:db8::7]:443#") + kKey);
  CHECK(v6.status == MV_VPN_OK);
  CHECK(v6.relay == "[2001:db8::7]:443");
  // Not codes: a pairing code, no key, a key shorter than the relay takes, a second '#', no port,
  // a space inside the key.
  for (const std::string& text : {std::string("mediaviewer://pair#abc"), std::string("home.duckdns.org:47614"),
                                 std::string("home.duckdns.org:47614#short"),
                                 "home.duckdns.org:47614#" + std::string(kKey) + "#more",
                                 "home.duckdns.org#" + std::string(kKey), std::string("home.duckdns.org:47614#PHONE VPN KEY 0123456789"),
                                 std::string()}) {
    INFO(text);
    const auto p = parse(text);
    CHECK(p.status == MV_VPN_INVALID);
    CHECK(p.relay.empty());
    CHECK(p.key.empty());
    CHECK(p.tls == 0);
    CHECK(p.tls_port == 0);
  }
  CHECK(parse("mediaviewer://relay-tls#home.duckdns.org#" + std::string(kKey)).status == MV_VPN_INVALID);  // no port
  char* r = nullptr;
  char* k = nullptr;
  std::int32_t t = 0;
  std::uint16_t tp = 0;
  CHECK(mv_vpn_parse_code(nullptr, &r, &k, &t, &tp) == MV_VPN_INVALID);
  CHECK(mv_vpn_parse_code("home.duckdns.org:47614#0123456789abcdef", &r, &k, nullptr, &tp) == MV_VPN_INVALID);
  mv_vpn_free(nullptr);  // allowed
}

TEST_CASE("phone vpn: the knock key is the desktop's, derived by the app and handed in", "[phone-vpn]") {
  std::uint8_t k[32] = {};
  REQUIRE(mv_vpn_derive_knock_key(kKey, k) == MV_VPN_OK);
  const auto desktop = tls::derive_knock_key(kKey);
  REQUIRE(desktop);
  CHECK(std::equal(desktop->begin(), desktop->end(), k));
  CHECK(mv_vpn_derive_knock_key("", k) == MV_VPN_INVALID);
  // The extension never derives it: the HTTPS look without the app's key is refused at start,
  // before anything is dialled.
  phone_flow flow;
  const auto where = at(closed_port());
  auto c = config_for(where, kKey, flow);
  c.tls = 1;
  mv_vpn* v = nullptr;
  CHECK(mv_vpn_start(&c, &v) == MV_VPN_INVALID);
  CHECK(v == nullptr);
}

TEST_CASE("phone vpn: starting checks the server first, and says why it could not", "[phone-vpn]") {
  udp_echo ue;
  tcp_echo te;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  phone_flow flow;
  mv_vpn* v = nullptr;
  SECTION("the wrong key: an old code") {
    const auto where = at((*server)->port());
    auto c = config_for(where, "AN-OLD-KEY-THE-SERVER-FORGOT", flow);
    CHECK(mv_vpn_start(&c, &v) == MV_VPN_KEY_REJECTED);
  }
  SECTION("nothing at that address and port") {
    const auto where = at(closed_port());
    auto c = config_for(where, kKey, flow);
    CHECK(mv_vpn_start(&c, &v) == MV_VPN_UNREACHABLE);
  }
  SECTION("not an address") {
    const std::string where = "no port here";
    auto c = config_for(where, kKey, flow);
    CHECK(mv_vpn_start(&c, &v) == MV_VPN_INVALID);
  }
  CHECK(v == nullptr);
  CHECK(flow.told().empty());
  CHECK((*server)->snapshot().forwarded == 0);  // the check forwards nothing anywhere
}

TEST_CASE("phone vpn: an app's TCP and DNS go through the extension's VPN and the relay", "[phone-vpn]") {
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  phone_flow flow;
  const auto where = at((*server)->port());
  auto c = config_for(where, kKey, flow);
  mv_vpn* v = nullptr;
  REQUIRE(mv_vpn_start(&c, &v) == MV_VPN_OK);
  REQUIRE(v);
  const auto in = [&](const std::vector<std::uint8_t>& p) { return mv_vpn_packet_in(v, p.data(), p.size()); };
  std::vector<std::uint8_t> keep;
  // DNS to the tunnel's resolver goes to the relay's (the UDP echo here).
  const auto q = v4_flow("198.18.0.1", 5353, "198.18.0.2", 53, vpn::kUdp);
  REQUIRE(in(vpn::make_udp(reversed(q), bytes("who is example.com"))) == MV_VPN_OK);
  auto answer = flow.next(vpn::kUdp, keep);
  REQUIRE(answer);
  CHECK(answer->f == reversed(q));
  CHECK(std::string(answer->payload.begin(), answer->payload.end()) == "whO IS EXAMPLE.COM");
  // An app's TCP connection, through the relay to the echo server.
  const auto f = v4_flow("198.18.0.1", 40100, "127.0.0.1", te.port(), vpn::kTcp);
  std::uint32_t seq = 1000;
  const auto tcp = [&](std::uint8_t flags, std::uint32_t ack, const std::string& payload = {}) {
    vpn::tcp_fields t{seq, ack, flags, 65535, static_cast<std::uint16_t>((flags & vpn::kSyn) ? 1460 : 0)};
    return vpn::make_tcp(reversed(f), t, bytes(payload));
  };
  REQUIRE(in(tcp(vpn::kSyn, 0)) == MV_VPN_OK);
  auto synack = flow.next(vpn::kTcp, keep);
  REQUIRE(synack);
  CHECK(synack->tcp.flags == (vpn::kSyn | vpn::kAck));
  seq = 1001;
  const auto ack = synack->tcp.seq + 1;
  REQUIRE(in(tcp(vpn::kAck, ack)) == MV_VPN_OK);
  REQUIRE(in(tcp(vpn::kAck | vpn::kPsh, ack, "through the phone")) == MV_VPN_OK);
  std::string got;
  for (int i = 0; i < 20 && got.size() < 17; ++i) {
    auto p = flow.next(vpn::kTcp, keep);
    REQUIRE(p);
    got.append(p->payload.begin(), p->payload.end());
  }
  CHECK(got == "THROUGH THE PHONE");
  mv_vpn_state s{};
  CHECK(eventually([&] {
    mv_vpn_get_state(v, &s);
    return s.tcp_made == 1;
  }));
  CHECK(s.udp_flows >= 1);
  CHECK(s.problem == MV_VPN_OK);
  CHECK(s.dropped == 0);
  CHECK((*server)->snapshot().forwarded >= 2);  // the DNS flow and the TCP stream
  // The interface's packets are whole packets: none empty, none over 64 KiB.
  CHECK(mv_vpn_packet_in(v, keep.data(), 0) == MV_VPN_INVALID);
  std::vector<std::uint8_t> huge(70000);
  CHECK(mv_vpn_packet_in(v, huge.data(), huge.size()) == MV_VPN_INVALID);
  CHECK(mv_vpn_check(v) == MV_VPN_OK);
  mv_vpn_stop(v);  // every stream reset, every thread joined
  CHECK(flow.told().empty());  // nothing went wrong, so nothing was told
}

TEST_CASE("phone vpn: the server going away is told once, and the check says so", "[phone-vpn]") {
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  phone_flow flow;
  const auto where = at((*server)->port());
  auto c = config_for(where, kKey, flow);
  mv_vpn* v = nullptr;
  REQUIRE(mv_vpn_start(&c, &v) == MV_VPN_OK);
  server->reset();  // the server stops (a network change, the PC asleep)
  // An app tries a connection: its forward fails, and the extension hears why.
  const auto f = v4_flow("198.18.0.1", 40200, "127.0.0.1", te.port(), vpn::kTcp);
  vpn::tcp_fields t{5000, 0, vpn::kSyn, 65535, 1460};
  const auto syn = vpn::make_tcp(reversed(f), t, {});
  REQUIRE(mv_vpn_packet_in(v, syn.data(), syn.size()) == MV_VPN_OK);
  CHECK(eventually([&] { return !flow.told().empty(); }));
  CHECK(flow.told() == std::vector<mv_vpn_status>{MV_VPN_UNREACHABLE});
  mv_vpn_state s{};
  mv_vpn_get_state(v, &s);
  CHECK(s.problem == MV_VPN_UNREACHABLE);
  CHECK(s.failed >= 1);
  CHECK(mv_vpn_check(v) == MV_VPN_UNREACHABLE);
  CHECK(flow.told().size() == 1);  // the same state is not told twice
  mv_vpn_stop(v);
}

namespace {

// One loopback address whose server can change under it, as a home server's does when Use as VPN
// is turned off and on (a new key): each connection is passed on to `to`'s port.
struct switchable_front {
  std::unique_ptr<sp::net::listener> l;
  std::atomic<std::uint16_t> to{0};
  std::atomic<bool> stop{false};
  std::thread t;
  std::mutex m;
  std::vector<std::shared_ptr<sp::link>> open;  // under m
  std::vector<std::thread> pumps;                // under m
  switchable_front() {
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
        auto back = sp::net::connect({"127.0.0.1", to.load()}, 2000);
        if (!back) continue;
        std::shared_ptr<sp::link> a(std::move(*c)), b(std::move(*back));
        const auto pump = [this](std::shared_ptr<sp::link> from, std::shared_ptr<sp::link> into) {
          std::vector<std::uint8_t> buf(65536);
          while (!stop.load()) {
            auto n = from->read_some(buf, 200);
            if (!n && n.error() == mv::status::timeout) continue;
            if (!n || *n == 0 || !into->write_all(std::span<const std::uint8_t>(buf.data(), *n))) break;
          }
          into->shutdown();
        };
        std::lock_guard lock(m);
        open.push_back(a);
        open.push_back(b);
        pumps.emplace_back(pump, a, b);
        pumps.emplace_back(pump, b, a);
      }
    });
  }
  ~switchable_front() {
    stop.store(true);
    l->shutdown();
    t.join();
    std::lock_guard lock(m);
    for (auto& x : open) x->shutdown();
    for (auto& p : pumps) p.join();
  }
  std::uint16_t port() const { return l->port(); }
};

}  // namespace

TEST_CASE("phone vpn: a server whose key changed while running is named by the check", "[phone-vpn]") {
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  auto other = relay_options(te.port(), ue.port());
  other.key = "A-NEW-KEY-AFTER-USE-AS-VPN-WAS-TURNED-OFF-AND-ON";
  auto renewed = relay::server::start(other);
  REQUIRE(renewed);
  switchable_front front;
  front.to.store((*server)->port());
  phone_flow flow;
  const auto where = at(front.port());
  auto c = config_for(where, kKey, flow);
  mv_vpn* v = nullptr;
  REQUIRE(mv_vpn_start(&c, &v) == MV_VPN_OK);
  front.to.store((*renewed)->port());  // the same address, a new key
  const auto f = v4_flow("198.18.0.1", 40300, "127.0.0.1", te.port(), vpn::kTcp);
  vpn::tcp_fields t{7000, 0, vpn::kSyn, 65535, 1460};
  const auto syn = vpn::make_tcp(reversed(f), t, {});
  REQUIRE(mv_vpn_packet_in(v, syn.data(), syn.size()) == MV_VPN_OK);
  // While running the VPN knows only that the server had a problem; the check names it.
  CHECK(eventually([&] { return flow.told() == std::vector<mv_vpn_status>{MV_VPN_UNREACHABLE}; }));
  CHECK(mv_vpn_check(v) == MV_VPN_KEY_REJECTED);
  CHECK(flow.told() == std::vector<mv_vpn_status>{MV_VPN_UNREACHABLE, MV_VPN_KEY_REJECTED});
  mv_vpn_state s{};
  mv_vpn_get_state(v, &s);
  CHECK(s.problem == MV_VPN_KEY_REJECTED);
  // Stopping ends the stream that failed (the runner's problem still says so): nothing more is
  // told once the stop begins, so Swift never hears from a VPN it is tearing down.
  mv_vpn_stop(v);
  CHECK(flow.told().size() == 2);
}

TEST_CASE("phone vpn: the extension's caps hold: at most MV_VPN_MAX_TCP app connections", "[phone-vpn]") {
  // The desktop takes 1024 (netstack.h kMaxConnections); a packet tunnel extension's memory
  // ceiling cannot hold that (plan §2.1), so the phone's profile resets the one past its cap.
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  phone_flow flow;
  const auto where = at((*server)->port());
  auto c = config_for(where, kKey, flow);
  mv_vpn* v = nullptr;
  REQUIRE(mv_vpn_start(&c, &v) == MV_VPN_OK);
  constexpr int kAsked = MV_VPN_MAX_TCP + 1;
  static_assert(MV_VPN_MAX_TCP < vpn::kMaxConnections);
  for (int i = 0; i < kAsked; ++i) {
    const auto f = v4_flow("198.18.0.1", static_cast<std::uint16_t>(41000 + i), "127.0.0.1", te.port(), vpn::kTcp);
    vpn::tcp_fields t{1000, 0, vpn::kSyn, 65535, 1460};
    const auto syn = vpn::make_tcp(reversed(f), t, {});
    REQUIRE(mv_vpn_packet_in(v, syn.data(), syn.size()) == MV_VPN_OK);
  }
  // The first answer to each SYN: a SYN-ACK for the first MV_VPN_MAX_TCP, a reset for the last.
  std::map<std::uint16_t, std::uint8_t> first;
  std::vector<std::uint8_t> keep;
  while (first.size() < static_cast<std::size_t>(kAsked)) {
    auto p = flow.next(vpn::kTcp, keep);
    REQUIRE(p);
    first.emplace(p->f.dport, p->tcp.flags);
  }
  int accepted = 0;
  for (int i = 0; i < MV_VPN_MAX_TCP; ++i) accepted += first[static_cast<std::uint16_t>(41000 + i)] == (vpn::kSyn | vpn::kAck);
  CHECK(accepted == MV_VPN_MAX_TCP);
  CHECK((first[static_cast<std::uint16_t>(41000 + MV_VPN_MAX_TCP)] & vpn::kRst) != 0);
  mv_vpn_stop(v);
}

TEST_CASE("phone vpn: with the HTTPS look, the handed-in knock key reaches the server", "[phone-vpn]") {
  if (!tls::available()) SKIP("this build has no BoringSSL");
  tcp_echo te;
  udp_echo ue;
  auto o = relay_options(te.port(), ue.port());
  o.tls_port = 1;  // loopback: the OS picks (tls_port())
  o.tls_name = "relay.example";
  auto server = relay::server::start(o);
  REQUIRE(server);
  phone_flow flow;
  const auto where = at((*server)->port());
  auto c = config_for(where, kKey, flow);
  c.tls = 1;
  c.tls_port = (*server)->tls_port();
  std::uint8_t k[32] = {};
  REQUIRE(mv_vpn_derive_knock_key(kKey, k) == MV_VPN_OK);
  c.knock_key = k;
  mv_vpn* v = nullptr;
  REQUIRE(mv_vpn_start(&c, &v) == MV_VPN_OK);
  CHECK((*server)->snapshot().website == 0);  // knocked: the hop, not the web site
  mv_vpn_stop(v);
  // (A wrong handed-in key is the one used, never a derived one: "driftwire tls: a dialer given
  // the knock key uses it and derives none", which waits out the web site's 10 s.)
}

TEST_CASE("phone vpn: the interface's settings route everything, both families, around the server", "[phone-vpn]") {
  const auto read = [](const char* relay_at) -> std::optional<std::string> {
    char* j = mv_vpn_settings_json(relay_at);
    if (!j) return std::nullopt;
    std::string s(j);
    mv_vpn_free(j);
    return s;
  };
  const auto has = [](const std::string& json, const char* list, const char* base, int prefix) {
    auto doc = mv::json::parse(json);
    REQUIRE(doc);
    const auto* arr = doc->find(list);
    REQUIRE(arr);
    for (const auto& n : arr->a) {
      if (n.str("base") && *n.str("base") == base && n.integer("prefix").value_or(-1) == prefix) return true;
    }
    return false;
  };
  const auto v4 = read("127.0.0.1:47614");
  REQUIRE(v4);
  INFO(*v4);
  auto doc = mv::json::parse(*v4);
  REQUIRE(doc);
  const auto text = [&](const char* key) { return doc->str(key) ? *doc->str(key) : std::string(); };
  CHECK(text("remote") == "127.0.0.1");  // the server's address, for tunnelRemoteAddress
  CHECK(text("address4") == "198.18.0.1");
  CHECK(doc->integer("prefix4").value_or(0) == 30);
  CHECK(text("dns4") == "198.18.0.2");
  CHECK(text("address6") == "fd5d:7269:6674::1");
  CHECK(doc->integer("prefix6").value_or(0) == 64);
  CHECK(doc->integer("mtu").value_or(0) == static_cast<std::int64_t>(vpn::kMtu));
  CHECK(has(*v4, "include4", "0.0.0.0", 0));  // every IPv4 address through the tunnel
  CHECK(has(*v4, "include6", "::", 0));       // and every IPv6 one: no leak around it (plan §1.3)
  CHECK(has(*v4, "exclude4", "127.0.0.1", 32));  // the server itself left out
  const auto v6 = read("[::1]:47614");
  REQUIRE(v6);
  CHECK(has(*v6, "exclude6", "::1", 128));
  CHECK_FALSE(read("no port"));
  CHECK_FALSE(read(nullptr));
}
