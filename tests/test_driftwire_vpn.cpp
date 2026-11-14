// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire VPN (slice 5; docs/plans/driftwire-menu-and-tunnel.md): the
// userspace TCP/IP stack (netstack.h) spoken to by a scripted app, packet by
// packet; the relay's datagram flows (splice.h kForwardUdp); the VPN runner
// end to end, a stand-in interface's packets through a relay server on
// loopback to a TCP and a UDP echo; and the engine's switch with a stand-in
// for the OS. The OS halves (vpn_mac.mm, vpn_win.cpp) need the OS's
// approval and are not run here.
#include "driftwire_fixture.h"

#include <algorithm>
#include <cctype>
#include <deque>

#include "addons/driftwire/netstack.h"
#include "wire/splice.h"
#include "addons/driftwire/vpn.h"

namespace vpn = mv::driftwire::vpn;
namespace relay = mv::driftwire::relay;

namespace {

constexpr const char* kKey = "TEST-RELAY-KEY-0123456789";

vpn::flow v6_flow(const char* src, std::uint16_t sport, const char* dst, std::uint16_t dport,
                  std::uint8_t proto = vpn::kTcp) {
  vpn::flow f;
  f.v6 = true;
  f.proto = proto;
  f.src = *sp::net::parse_address(src);
  f.dst = *sp::net::parse_address(dst);
  f.sport = sport;
  f.dport = dport;
  return f;
}

vpn::flow v4_flow(const char* src, std::uint16_t sport, const char* dst, std::uint16_t dport,
                  std::uint8_t proto = vpn::kTcp) {
  vpn::flow f;
  f.proto = proto;
  f.src = *sp::net::parse_address(src);
  f.dst = *sp::net::parse_address(dst);
  f.sport = sport;
  f.dport = dport;
  return f;
}

// The flow seen from the other end (make_tcp builds dst -> src).
vpn::flow reversed(const vpn::flow& f) {
  vpn::flow r = f;
  std::swap(r.src, r.dst);
  std::swap(r.sport, r.dport);
  return r;
}

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Packets the stack sent, in order.
struct wire {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::vector<std::uint8_t>> q;
  void push(std::vector<std::uint8_t> p) {
    std::lock_guard lock(m);
    q.push_back(std::move(p));
    cv.notify_all();
  }
  std::optional<std::vector<std::uint8_t>> next(int ms = 3000) {
    std::unique_lock lock(m);
    if (!cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !q.empty(); })) return std::nullopt;
    auto p = std::move(q.front());
    q.pop_front();
    return p;
  }
  std::size_t size() {
    std::lock_guard lock(m);
    return q.size();
  }
};

// An app's TCP connection, as its OS would speak it on the interface.
struct app_tcp {
  vpn::flow f;  // src: the app, dst: where it goes
  std::uint32_t seq = 1000, ack = 0;
  std::vector<std::uint8_t> packet(std::uint8_t flags, const std::string& payload = {}, std::uint16_t window = 65535) {
    vpn::tcp_fields t{seq, ack, flags, window, static_cast<std::uint16_t>((flags & vpn::kSyn) ? 1460 : 0)};
    return vpn::make_tcp(reversed(f), t, bytes(payload));
  }
};

// The next packet that is TCP to the app, parsed (its payload copied).
struct seen {
  vpn::tcp_fields t;
  std::string payload;
};
std::optional<seen> next_tcp(wire& w, int ms = 3000) {
  auto p = w.next(ms);
  if (!p) return std::nullopt;
  auto r = vpn::parse(*p);
  REQUIRE(r);
  REQUIRE(r->f.proto == vpn::kTcp);
  return seen{r->tcp, std::string(r->payload.begin(), r->payload.end())};
}

// A TCP server on loopback that answers each connection's bytes upper-cased
// (`both`: IPv6's loopback too, on a dual-stack listener).
struct tcp_echo {
  std::unique_ptr<sp::net::listener> l;
  std::thread t;
  std::atomic<bool> stop{false};
  std::vector<std::thread> conns;
  explicit tcp_echo(bool both = false) {
    auto x = both ? sp::net::listen_internet(0, false) : sp::net::listen(true);
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

// A UDP server on loopback that answers each datagram upper-cased.
struct udp_echo {
  std::unique_ptr<sp::net::udp_socket> s;
  std::thread t;
  std::atomic<bool> stop{false};
  explicit udp_echo(bool v6 = false) {
    auto x = sp::net::open_udp(v6, 1, true);
    REQUIRE(x);
    s = std::move(*x);
    t = std::thread([this] {
      std::vector<std::uint8_t> b(2048);
      while (!stop.load()) {
        sp::net::endpoint from;
        auto n = s->receive_from(b, 200, from);
        if (!n) continue;
        for (std::size_t i = 0; i < *n; ++i) b[i] = static_cast<std::uint8_t>(std::toupper(b[i]));
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
  o.dns_upstream = sp::net::endpoint{"127.0.0.1", udp_port};  // the "resolver" upper-cases what it hears
  return o;
}

}  // namespace

TEST_CASE("driftwire vpn: packets made and read with care, checksums both ways", "[driftwire]") {
  // RFC 1071's example words.
  const std::vector<std::uint8_t> words{0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
  CHECK(vpn::checksum(words) == 0x220d);
  const auto f = v4_flow("198.18.0.1", 40000, "93.184.216.34", 443);
  const auto p = vpn::make_tcp(reversed(f), {7, 9, vpn::kAck | vpn::kPsh, 1000, 0}, bytes("hi"));
  const auto r = vpn::parse(p);
  REQUIRE(r);
  CHECK(r->f == f);
  CHECK(r->tcp.seq == 7);
  CHECK(r->tcp.ack == 9);
  CHECK(r->tcp.window == 1000);
  CHECK(std::string(r->payload.begin(), r->payload.end()) == "hi");
  // A flipped bit fails its checksum; a fragment, and what is not TCP or UDP, are not taken.
  auto bad = p;
  bad.back() ^= 1;
  CHECK_FALSE(vpn::parse(bad));
  auto frag = p;
  frag[6] = 0x20;  // more fragments
  frag[10] = frag[11] = 0;
  const auto hc = vpn::checksum(std::span<const std::uint8_t>(frag.data(), 20));
  frag[10] = static_cast<std::uint8_t>(hc >> 8);
  frag[11] = static_cast<std::uint8_t>(hc);
  CHECK_FALSE(vpn::parse(frag));
  CHECK_FALSE(vpn::parse(std::vector<std::uint8_t>(40, 0)));
  // IPv6 and UDP.
  vpn::flow u;
  u.v6 = true;
  u.proto = vpn::kUdp;
  u.src = *sp::net::parse_address("fd00:6477::1");
  u.dst = *sp::net::parse_address("2001:db8::53");
  u.sport = 5353;
  u.dport = 53;
  const auto up = vpn::make_udp(reversed(u), bytes("query"));
  const auto ur = vpn::parse(up);
  REQUIRE(ur);
  CHECK(ur->f == u);
  CHECK(std::string(ur->payload.begin(), ur->payload.end()) == "query");
}

TEST_CASE("driftwire vpn: the stack takes an app's TCP connection as a link, both ways, and closes it",
          "[driftwire]") {
  wire w;
  vpn::flow accepted_flow;
  std::unique_ptr<sp::link> accepted;  // destroyed before the stack, below it
  vpn::stack s({[&](std::vector<std::uint8_t> p) { w.push(std::move(p)); },
                [&](std::unique_ptr<sp::link> l, const vpn::flow& f) {
                  accepted = std::move(l);
                  accepted_flow = f;
                },
                {}});
  struct drop_first {
    std::unique_ptr<sp::link>& l;
    ~drop_first() { l.reset(); }
  } dropped{accepted};
  app_tcp a{v4_flow("198.18.0.1", 40000, "93.184.216.34", 443)};
  // The handshake: SYN, SYN-ACK (an MSS), ACK.
  s.input(a.packet(vpn::kSyn));
  auto synack = next_tcp(w);
  REQUIRE(synack);
  CHECK(synack->t.flags == (vpn::kSyn | vpn::kAck));
  CHECK(synack->t.ack == 1001);
  CHECK(synack->t.mss == vpn::kMtu - 40);
  REQUIRE(accepted);
  CHECK(accepted_flow == a.f);
  a.seq = 1001;
  a.ack = synack->t.seq + 1;
  s.input(a.packet(vpn::kAck));
  // App -> link, ACKed.
  s.input(a.packet(vpn::kAck | vpn::kPsh, "hello"));
  a.seq += 5;
  std::vector<std::uint8_t> buf(64);
  auto n = accepted->read_some(buf, 3000);
  REQUIRE(n);
  CHECK(std::string(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)) == "hello");
  auto ack = next_tcp(w);
  REQUIRE(ack);
  CHECK(ack->t.ack == a.seq);
  // Out of order: dropped, the ACK says what it still wants.
  {
    app_tcp later = a;
    later.seq += 100;
    s.input(later.packet(vpn::kAck | vpn::kPsh, "skip"));
    auto dup = next_tcp(w);
    REQUIRE(dup);
    CHECK(dup->t.ack == a.seq);
    CHECK_FALSE(accepted->read_some(buf, 100));  // nothing arrived
  }
  // Link -> app: 3000 bytes in segments of the MSS at most.
  const std::string big(3000, 'x');
  REQUIRE(accepted->write_all(bytes(big)));
  std::string got;
  while (got.size() < big.size()) {
    auto seg = next_tcp(w);
    REQUIRE(seg);
    CHECK(seg->payload.size() <= vpn::kMtu - 40);
    CHECK(seg->t.seq == a.ack + got.size());
    got += seg->payload;
  }
  CHECK(got == big);
  a.ack += static_cast<std::uint32_t>(big.size());
  s.input(a.packet(vpn::kAck));
  // A segment the app never ACKs is sent again on the timer.
  REQUIRE(accepted->write_all(bytes("lost")));
  auto first = next_tcp(w);
  REQUIRE(first);
  CHECK(first->payload == "lost");
  std::this_thread::sleep_for(std::chrono::milliseconds(vpn::kRtoMs + 100));
  s.tick();
  auto again = next_tcp(w);
  REQUIRE(again);
  CHECK(again->payload == "lost");
  CHECK(again->t.seq == first->t.seq);
  a.ack += 4;
  s.input(a.packet(vpn::kAck));
  // The app closes: what it sent, then the end.
  s.input(a.packet(vpn::kAck | vpn::kFin));
  a.seq += 1;
  n = accepted->read_some(buf, 3000);
  REQUIRE(n);
  CHECK(*n == 0);
  auto fin_ack = next_tcp(w);
  REQUIRE(fin_ack);
  CHECK(fin_ack->t.ack == a.seq);
  // The link closes: FIN to the app; its ACK ends the connection.
  accepted->shutdown();
  auto fin = next_tcp(w);
  REQUIRE(fin);
  CHECK((fin->t.flags & vpn::kFin) != 0);
  a.ack += 1;
  s.input(a.packet(vpn::kAck));
  s.tick();
  CHECK(s.connections() == 0);
}

TEST_CASE("driftwire vpn: a reset ends the link, a stray segment is reset, and UDP goes up and down",
          "[driftwire]") {
  wire w;
  std::unique_ptr<sp::link> accepted;
  std::vector<std::pair<vpn::flow, std::string>> datagrams;
  vpn::stack s({[&](std::vector<std::uint8_t> p) { w.push(std::move(p)); },
                [&](std::unique_ptr<sp::link> l, const vpn::flow&) { accepted = std::move(l); },
                [&](const vpn::flow& f, std::span<const std::uint8_t> d) {
                  datagrams.emplace_back(f, std::string(d.begin(), d.end()));
                }});
  struct drop_first {
    std::unique_ptr<sp::link>& l;
    ~drop_first() { l.reset(); }
  } dropped{accepted};
  app_tcp a{v4_flow("198.18.0.1", 40001, "10.20.30.40", 80)};
  s.input(a.packet(vpn::kSyn));
  REQUIRE(next_tcp(w));
  REQUIRE(accepted);
  s.input(a.packet(vpn::kRst));
  std::vector<std::uint8_t> buf(8);
  CHECK_FALSE(accepted->read_some(buf, 1000));
  CHECK(s.connections() == 0);
  // A segment for nothing: RST.
  app_tcp stray{v4_flow("198.18.0.1", 40002, "10.20.30.40", 80)};
  s.input(stray.packet(vpn::kAck | vpn::kPsh, "late"));
  auto rst = next_tcp(w);
  REQUIRE(rst);
  CHECK((rst->t.flags & vpn::kRst) != 0);
  // UDP.
  const auto u = v4_flow("198.18.0.1", 5353, "198.18.0.2", 53, vpn::kUdp);
  s.input(vpn::make_udp(reversed(u), bytes("q")));
  REQUIRE(datagrams.size() == 1);
  CHECK(datagrams[0].first == u);
  CHECK(datagrams[0].second == "q");
  s.udp_reply(u, bytes("answer"));
  auto back = w.next();
  REQUIRE(back);
  const auto r = vpn::parse(*back);
  REQUIRE(r);
  CHECK(r->f == reversed(u));
  CHECK(std::string(r->payload.begin(), r->payload.end()) == "answer");
}

TEST_CASE("driftwire vpn: the relay carries a key holder's datagram flows, DNS to its own resolver",
          "[driftwire]") {
  tcp_echo te;
  udp_echo ue;
  auto s = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(s);
  const auto dial = [&] {
    auto c = sp::net::connect({"127.0.0.1", (*s)->port()}, 3000);
    REQUIRE(c);
    return std::move(*c);
  };
  relay::forward_answer said{};
  auto f = relay::forward(dial(), kKey, "127.0.0.1", ue.port(), relay::kForwardWaitMs, said, relay::kForwardUdp);
  REQUIRE(f);
  CHECK(said == relay::forward_answer::ok);
  std::vector<std::uint8_t> part, d;
  for (const char* m : {"ping", "second"}) {
    REQUIRE(relay::write_datagram(**f, bytes(m)));
    REQUIRE(relay::read_datagram(**f, part, d, 5000));
    CHECK(std::string(d.begin(), d.end()) == upper(m));
  }
  // The server's resolver, whatever the ports list says.
  auto dns = relay::forward(dial(), kKey, std::string(relay::kDnsHost), 53, relay::kForwardWaitMs, said,
                            relay::kForwardUdp);
  REQUIRE(dns);
  REQUIRE(relay::write_datagram(**dns, bytes("example.com?")));
  REQUIRE(relay::read_datagram(**dns, part, d, 5000));
  CHECK(std::string(d.begin(), d.end()) == "EXAMPLE.COM?");
  // Another UDP port: no.
  CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", 443, 5000, said, relay::kForwardUdp));
  CHECK(said == relay::forward_answer::port);
  // A kind it does not know: said.
  CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", te.port(), 5000, said, 9));
  CHECK(said == relay::forward_answer::kind);
}

TEST_CASE("driftwire vpn: an app's TCP and DNS go through the VPN and the relay, end to end", "[driftwire]") {
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  auto [ours, os_side] = loopback_pair();
  auto os = vpn::framed_device(std::move(os_side));  // the OS's end of the interface
  vpn::runner::options o;
  o.dial.relay = "127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey;
  o.dev = vpn::framed_device(std::move(ours));
  auto r = vpn::runner::start(std::move(o));
  REQUIRE(r);
  std::vector<std::uint8_t> buf(65536);
  const auto next_from_vpn = [&](std::uint8_t proto) -> std::optional<vpn::parsed> {
    static thread_local std::vector<std::uint8_t> keep;
    for (int i = 0; i < 50; ++i) {
      auto n = os->read(buf, 5000);
      if (!n) return std::nullopt;
      keep.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
      auto p = vpn::parse(keep);
      if (p && p->f.proto == proto) return p;
    }
    return std::nullopt;
  };
  // DNS to the tunnel's resolver: the relay's (the UDP echo here).
  const auto q = v4_flow("198.18.0.1", 5353, "198.18.0.2", 53, vpn::kUdp);
  REQUIRE(os->write(vpn::make_udp(reversed(q), bytes("who is example.com"))));
  auto answer = next_from_vpn(vpn::kUdp);
  REQUIRE(answer);
  CHECK(answer->f == reversed(q));
  CHECK(std::string(answer->payload.begin(), answer->payload.end()) == "WHO IS EXAMPLE.COM");
  // TCP to the echo server through the relay.
  app_tcp a{v4_flow("198.18.0.1", 40100, "127.0.0.1", te.port())};
  REQUIRE(os->write(a.packet(vpn::kSyn)));
  auto synack = next_from_vpn(vpn::kTcp);
  REQUIRE(synack);
  CHECK(synack->tcp.flags == (vpn::kSyn | vpn::kAck));
  a.seq = 1001;
  a.ack = synack->tcp.seq + 1;
  REQUIRE(os->write(a.packet(vpn::kAck)));
  REQUIRE(os->write(a.packet(vpn::kAck | vpn::kPsh, "through the vpn")));
  a.seq += 15;
  std::string got;
  for (int i = 0; i < 20 && got.size() < 15; ++i) {
    auto p = next_from_vpn(vpn::kTcp);
    REQUIRE(p);
    got.append(p->payload.begin(), p->payload.end());
  }
  CHECK(got == "THROUGH THE VPN");
  CHECK(eventually([&] { return r->get()->state().tcp_made == 1; }));
  CHECK((*server)->snapshot().forwarded >= 2);  // the DNS flow and the TCP stream
  r->reset();  // every stream reset, every thread joined
}

namespace {

// The OS's interface, as a stand-in: a loopback pair per start.
struct fake_vpn final : vpn::os_port {
  std::shared_ptr<std::atomic<int>> starts = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<int>> stops = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<vpn::settings> last = std::make_shared<vpn::settings>();
  std::shared_ptr<std::unique_ptr<vpn::device>> os = std::make_shared<std::unique_ptr<vpn::device>>();
  std::string unavailable() override { return {}; }
  mv::result<std::unique_ptr<vpn::device>> start(const vpn::settings& s) override {
    ++*starts;
    *last = s;
    auto [a, b] = loopback_pair();
    *os = vpn::framed_device(std::move(b));
    return vpn::framed_device(std::move(a));
  }
  void stop() override { ++*stops; }
};

}  // namespace

TEST_CASE("driftwire vpn: All traffic (VPN) runs instead of the proxy, from the window and the menu",
          "[driftwire]") {
  scratch_dir dir("driftwire_vpn_engine");
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  auto fv = std::make_unique<fake_vpn>();
  auto starts = fv->starts, stops = fv->stops;
  auto last = fv->last;
  auto holder = std::make_shared<std::unique_ptr<fake_vpn>>(std::move(fv));
  fake_host host(dir / "received");
  sp::engine::options o;
  o.my_name = "Ann";
  o.use_bluetooth = false;
  o.loopback_only = true;
  o.make_discovery = [] { return std::make_unique<inject_discovery>(); };
  o.make_vpn = [holder]() -> std::unique_ptr<vpn::os_port> { return std::move(*holder); };
  sp::engine eng(host, o);
  CHECK(eng.snapshot().tunnel_vpn_available);
  CHECK_FALSE(eng.snapshot().tunnel_vpn);  // off by default
  CHECK_FALSE(eng.snapshot().tunnel_block);
  eng.on_event("relay_server", 2, "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"");
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));  // the proxy first
  auto v = eng.snapshot();
  CHECK(sp::build_tree(v).find("\"id\":\"tunnel_vpn\"") != std::string::npos);
  CHECK(sp::build_menu(v).find("\"id\":\"tunnel_vpn\"") != std::string::npos);
  // The menu row's event is the window's: the VPN takes over.
  eng.on_event("tunnel_vpn", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_mode == "vpn" && eng.snapshot().tunnel_address.empty(); }));
  CHECK(*starts == 1);  // and no proxy now
  // What the OS side was told: the relay left out, and the local networks.
  REQUIRE_FALSE(last->exclude.empty());
  CHECK(sp::net::address_text(last->exclude[0]) == "127.0.0.1");
  CHECK(sp::net::parse_address(last->address6).has_value());  // IPv6 claimed too
  CHECK_FALSE(last->kill_switch);
  v = eng.snapshot();
  const auto tree = sp::build_tree(v);
  CHECK(tree.find("All traffic through your relay server") != std::string::npos);
  CHECK(tree.find("\"id\":\"tunnel_block\"") != std::string::npos);
  // The kill switch restarts it with the switch on.
  eng.on_event("tunnel_block", 2, "true");
  REQUIRE(eventually([&] { return *starts == 2; }));
  CHECK(last->kill_switch);
  // Off: the interface goes, the proxy comes back.
  eng.on_event("tunnel_vpn", 2, "false");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_mode == "proxy" && !eng.snapshot().tunnel_address.empty(); }));
  CHECK(*stops >= 2);
  eng.on_event("tunnel", 2, "false");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_address.empty(); }));
}

// ---- IPv6 (the leak with the kill switch off) -----------------------------------------------

namespace {

std::string text(const sp::net::subnet& n) { return sp::net::address_text(n.base) + "/" + std::to_string(n.prefix); }
std::vector<std::string> texts(const std::vector<sp::net::subnet>& all) {
  std::vector<std::string> out;
  for (const auto& n : all) out.push_back(text(n));
  return out;
}

vpn::settings leaky_machine() {
  vpn::settings s;  // the kill switch off, as by default
  s.exclude = {*sp::net::parse_address("203.0.113.7"), *sp::net::parse_address("2001:db8:7::7"),
               *sp::net::parse_address("203.0.113.7")};  // a name's A, AAAA, and the A again
  const auto net = [](const char* a, int p) { return sp::net::subnet{*sp::net::parse_address(a), p}; };
  s.exclude_nets = {
      net("192.168.1.23", 24),        // the LAN, as an interface's address
      net("2001:db8:aa:1::23", 64),   // its IPv6 network
      net("fe80::1c2b:3a4d", 64),     // link-local
      net("0.0.0.0", 0),              // a default route's "network": never left out
      net("::", 0),
      net("198.18.0.1", 30),          // a stale interface of ours
      net("fd5d:7269:6674::1", 64),
      net("10.1.2.203", 30),          // off a byte boundary: 10.1.2.200/30
      net("10.1.2.201", 30),          // the same network again
      net("2001:db8:bb:ff::9", 60),   // a delegated /60: 2001:db8:bb:f0::/60
      net("2002::", 16),              // 6to4: never a local network
      net("2001::", 32),              // Teredo
      net("10.0.0.1", 33),            // not a prefix of its family
  };
  return s;
}

}  // namespace

TEST_CASE("driftwire vpn: routes on a Mac claim IPv6 too, the relay and the local networks left out",
          "[driftwire][driftwire-vpn]") {
  const auto s = leaky_machine();
  CHECK_FALSE(s.kill_switch);
  CHECK(sp::net::parse_address(s.address6)->v6);
  CHECK_FALSE(sp::net::is_public(*sp::net::parse_address(s.address6)));  // unique local: never on the internet
  const auto p = vpn::plan_routes(s, vpn::route_style::default_route);
  // Network Extension: NEIPv4Route.default() and NEIPv6Route.default() (prefix 0).
  CHECK(texts(p.include4) == std::vector<std::string>{"0.0.0.0/0"});
  CHECK(texts(p.include6) == std::vector<std::string>{"::/0"});
  CHECK(texts(p.exclude4) == std::vector<std::string>{"203.0.113.7/32", "192.168.1.0/24", "10.1.2.200/30"});
  CHECK(texts(p.exclude6) ==
        std::vector<std::string>{"2001:db8:7::7/128", "2001:db8:aa:1::/64", "fe80::/64", "2001:db8:bb:f0::/60"});
}

TEST_CASE("driftwire vpn: routes on Windows claim IPv6 in two halves, the relay and the local networks left out",
          "[driftwire][driftwire-vpn]") {
  const auto s = leaky_machine();
  const auto p = vpn::plan_routes(s, vpn::route_style::halves);
  CHECK(texts(p.include4) == std::vector<std::string>{"0.0.0.0/1", "128.0.0.0/1"});
  CHECK(texts(p.include6) == std::vector<std::string>{"::/1", "8000::/1"});
  // Together the halves are every address of their family.
  const auto holds = [](const sp::net::subnet& n, const sp::net::address& a) {
    for (int bit = 0; bit < n.prefix; ++bit) {
      const auto i = static_cast<std::size_t>(bit / 8);
      const int m = 0x80 >> (bit % 8);
      if ((a.bytes[i] & m) != (n.base.bytes[i] & m)) return false;
    }
    return true;
  };
  for (const char* a : {"2606:4700::1111", "::ffff:1.2.3.4", "fe00::1", "8000::", "7fff:ffff::1", "ff02::1"}) {
    const auto addr = *sp::net::parse_address(a);
    CHECK(std::count_if(p.include6.begin(), p.include6.end(), [&](const auto& n) { return holds(n, addr); }) == 1);
  }
  CHECK(texts(p.exclude4) == texts(vpn::plan_routes(s, vpn::route_style::default_route).exclude4));
  CHECK(texts(p.exclude6) ==
        std::vector<std::string>{"2001:db8:7::7/128", "2001:db8:aa:1::/64", "fe80::/64", "2001:db8:bb:f0::/60"});
  // The kill switch changes what is blocked, never the routes.
  auto blocked = s;
  blocked.kill_switch = true;
  CHECK(texts(vpn::plan_routes(blocked, vpn::route_style::halves).exclude6) == texts(p.exclude6));
}

TEST_CASE("driftwire vpn: an app's IPv6 goes through the relay, and is never passed on untouched",
          "[driftwire][driftwire-vpn]") {
  tcp_echo te(true);
  udp_echo ue6(true);
  // A machine without IPv6 loopback (some CI containers) cannot run this.
  if (!sp::net::connect({"::1", te.port()}, 2000)) SKIP("no IPv6 loopback here");
  auto server = relay::server::start(relay_options(te.port(), ue6.port()));
  REQUIRE(server);
  auto [ours, os_side] = loopback_pair();
  auto os = vpn::framed_device(std::move(os_side));
  vpn::runner::options o;
  o.dial.relay = "127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey;
  o.dev = vpn::framed_device(std::move(ours));
  auto r = vpn::runner::start(std::move(o));
  REQUIRE(r);
  std::vector<std::uint8_t> buf(65536);
  std::vector<std::uint8_t> keep;
  const auto next_from_vpn = [&](std::uint8_t proto) -> std::optional<vpn::parsed> {
    for (int i = 0; i < 50; ++i) {
      auto n = os->read(buf, 5000);
      if (!n) return std::nullopt;
      keep.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
      auto p = vpn::parse(keep);
      if (p && p->f.proto == proto) return p;
    }
    return std::nullopt;
  };
  const char* me = "fd5d:7269:6674::1";  // the tunnel's IPv6 address (vpn.h settings)
  // A TCP SYN to an IPv6 address: answered by the stack, carried by the relay.
  app_tcp a{v6_flow(me, 40200, "::1", te.port())};
  REQUIRE(os->write(a.packet(vpn::kSyn)));
  auto synack = next_from_vpn(vpn::kTcp);
  REQUIRE(synack);
  CHECK(synack->f.v6);
  CHECK(synack->f == reversed(a.f));
  CHECK(synack->tcp.flags == (vpn::kSyn | vpn::kAck));
  a.seq = 1001;
  a.ack = synack->tcp.seq + 1;
  REQUIRE(os->write(a.packet(vpn::kAck)));
  REQUIRE(os->write(a.packet(vpn::kAck | vpn::kPsh, "over ipv6")));
  std::string got;
  for (int i = 0; i < 20 && got.size() < 9; ++i) {
    auto p = next_from_vpn(vpn::kTcp);
    REQUIRE(p);
    CHECK(p->f.v6);
    got.append(p->payload.begin(), p->payload.end());
  }
  CHECK(got == "OVER IPV6");
  CHECK(eventually([&] { return r->get()->state().tcp_made == 1; }));
  // A UDP datagram to an IPv6 address: a datagram flow at the relay.
  const auto d = v6_flow(me, 5400, "::1", ue6.port(), vpn::kUdp);
  REQUIRE(os->write(vpn::make_udp(reversed(d), bytes("datagram over ipv6"))));
  auto back = next_from_vpn(vpn::kUdp);
  REQUIRE(back);
  CHECK(back->f == reversed(d));
  CHECK(std::string(back->payload.begin(), back->payload.end()) == "DATAGRAM OVER IPV6");
  const auto forwarded = (*server)->snapshot().forwarded;
  CHECK(forwarded >= 2);
  // Somewhere the relay will not go (a port it does not forward): the app's
  // connection is reset, and nothing reaches the address but by the relay.
  app_tcp refused{v6_flow(me, 40201, "::1", static_cast<std::uint16_t>(te.port() == 9 ? 10 : 9))};
  REQUIRE(os->write(refused.packet(vpn::kSyn)));
  bool reset = false;
  for (int i = 0; i < 10 && !reset; ++i) {
    auto p = next_from_vpn(vpn::kTcp);
    REQUIRE(p);
    if (p->f.sport != refused.f.dport || p->f.dport != refused.f.sport) continue;
    if (p->tcp.flags & vpn::kSyn) {  // accepted first: the stack answers before the relay does
      refused.seq = 1001;
      refused.ack = p->tcp.seq + 1;
      REQUIRE(os->write(refused.packet(vpn::kAck)));
      continue;
    }
    reset = (p->tcp.flags & (vpn::kRst | vpn::kFin)) != 0;
  }
  CHECK(reset);
  CHECK(eventually([&] { return r->get()->state().failed >= 1; }));
  CHECK((*server)->snapshot().forwarded == forwarded);  // refused there, not carried around it
  CHECK((*server)->snapshot().forward_refused >= 1);
  r->reset();
}
