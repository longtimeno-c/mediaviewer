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
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <thread>

#include "addons/vpn/netstack.h"
#include "wire/splice.h"
#include "addons/vpn/vpn.h"
#include "vpn_fixture.h"

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

TEST_CASE("vpn: packets made and read with care, checksums both ways", "[vpn]") {
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

TEST_CASE("vpn: the stack takes an app's TCP connection as a link, both ways, and closes it",
          "[vpn]") {
  wire w;
  vpn::flow accepted_flow;
  std::unique_ptr<sp::link> accepted;  // destroyed before the stack, below it
  vpn::stack s({[&](std::vector<std::uint8_t> p) { w.push(std::move(p)); },
                [&](std::unique_ptr<sp::link> l, const vpn::flow& f) {
                  accepted = std::move(l);
                  accepted_flow = f;
                },
                {}, {}});
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

TEST_CASE("vpn: the stack says when its timer is next due, and when that moves sooner",
          "[vpn]") {
  // The runner's ticker sleeps until next_deadline(), not every 100 ms: a
  // phone's battery (docs/plans/phone-vpn.md §2.2).
  using clock = std::chrono::steady_clock;
  wire w;
  std::atomic<int> kicks{0};
  std::shared_ptr<sp::link> accepted;
  vpn::stack::callbacks cb;
  cb.out = [&](std::vector<std::uint8_t> p) { w.push(std::move(p)); };
  cb.on_tcp = [&](std::unique_ptr<sp::link> l, const vpn::flow&) { accepted = std::move(l); };
  std::optional<clock::time_point> due;  // the last kick's
  cb.timer = [&](clock::time_point t) {
    ++kicks;
    due = t;
  };
  vpn::stack st(std::move(cb));
  CHECK_FALSE(st.next_deadline());  // nothing to do, ever
  // A SYN: the SYN-ACK's resend is due in one RTO, and the timer was told.
  app_tcp a{v4_flow("198.18.0.1", 40200, "93.184.216.34", 443)};
  const auto t0 = clock::now();
  st.input(a.packet(vpn::kSyn));
  CHECK(kicks.load() == 1);
  auto d = st.next_deadline();
  REQUIRE(d);
  REQUIRE(due);
  CHECK(*due == *d);  // the kick says when, so a ticker sleeping sooner is left alone
  CHECK(*d > t0 + std::chrono::milliseconds(vpn::kRtoMs));
  CHECK(*d < clock::now() + std::chrono::milliseconds(vpn::kRtoMs + 50));
  auto synack = next_tcp(w);
  REQUIRE(synack);
  a.seq = 1001;
  a.ack = synack->t.seq + 1;
  st.input(a.packet(vpn::kAck));
  REQUIRE(accepted);
  // Established, nothing outstanding: only the idle limit is ahead.
  d = st.next_deadline();
  REQUIRE(d);
  CHECK(*d > clock::now() + std::chrono::milliseconds(vpn::kIdleMs - 1000));
  const int before = kicks.load();
  // Bytes to the app: a resend timer starts, and the ticker hears of it.
  REQUIRE(accepted->write_all(bytes("hello")));
  CHECK(kicks.load() == before + 1);
  d = st.next_deadline();
  REQUIRE(d);
  CHECK(*d < clock::now() + std::chrono::milliseconds(vpn::kRtoMs + 50));
  // More while it is outstanding: no kick (the deadline only moves later).
  REQUIRE(accepted->write_all(bytes(" again")));
  CHECK(kicks.load() == before + 1);
  d = st.next_deadline();  // from the later send
  REQUIRE(d);
  // Not resent before it is due; resent once it is.
  st.tick();
  auto p1 = next_tcp(w);
  REQUIRE(p1);
  CHECK(p1->payload == "hello");
  auto p2 = next_tcp(w);
  REQUIRE(p2);
  CHECK(p2->payload == " again");
  CHECK(w.size() == 0);
  std::this_thread::sleep_until(*d);
  st.tick();
  auto again = next_tcp(w);
  REQUIRE(again);
  CHECK(again->payload == "hello again");  // go-back-N: from snd_una
  // The resend backed the timer off (2 RTOs): an ACK of part resets it with
  // the rest still out, a deadline sooner than the ticker sleeps to: a kick.
  const int backed = kicks.load();
  a.ack = again->t.seq + 5;
  st.input(a.packet(vpn::kAck));
  CHECK(kicks.load() == backed + 1);
  REQUIRE(due);
  CHECK(*due < clock::now() + std::chrono::milliseconds(vpn::kRtoMs + 50));
  // All acked: back to the idle limit alone.
  a.ack = again->t.seq + static_cast<std::uint32_t>(again->payload.size());
  st.input(a.packet(vpn::kAck));
  d = st.next_deadline();
  REQUIRE(d);
  CHECK(*d > clock::now() + std::chrono::milliseconds(vpn::kIdleMs - 1000));
  accepted.reset();
}

TEST_CASE("vpn: an idle runner's ticker sleeps, and still resends on time", "[vpn]") {
  // Before this, the ticker woke every 100 ms with nothing to do. Count the
  // stack's ticks through its timer: none while idle, prompt when a TCP
  // segment is outstanding.
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  auto [ours, os_side] = loopback_pair();
  auto os = vpn::framed_device(std::move(os_side));
  vpn::runner::options o;
  o.dial.relay = "127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey;
  o.dev = vpn::framed_device(std::move(ours));
  auto r = vpn::runner::start(std::move(o));
  REQUIRE(r);
  // Idle: nothing open, nothing outstanding. The ticker wakes once at start
  // and then sleeps (it woke 10 times a second before).
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(r->get()->state().ticks <= 2);
  std::vector<std::uint8_t> buf(65536);
  const auto next_tcp_from_vpn = [&](int ms) -> std::optional<seen> {
    static thread_local std::vector<std::uint8_t> keep;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
      auto n = os->read(buf, 250);
      if (!n) {
        if (n.error() == mv::status::timeout) continue;
        return std::nullopt;
      }
      keep.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
      auto p = vpn::parse(keep);
      if (p && p->f.proto == vpn::kTcp) return seen{p->tcp, std::string(p->payload.begin(), p->payload.end())};
    }
    return std::nullopt;
  };
  // An app connects and never acknowledges the SYN-ACK: the runner's ticker
  // must resend it about one RTO later, though nothing else wakes it.
  app_tcp a{v4_flow("198.18.0.1", 40300, "127.0.0.1", te.port())};
  const auto t0 = std::chrono::steady_clock::now();  // the SYN-ACK goes after this
  REQUIRE(os->write(a.packet(vpn::kSyn)));
  auto first = next_tcp_from_vpn(3000);
  REQUIRE(first);
  CHECK(first->t.flags == (vpn::kSyn | vpn::kAck));
  auto second = next_tcp_from_vpn(3000);
  REQUIRE(second);
  CHECK(second->t.flags == (vpn::kSyn | vpn::kAck));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited >= std::chrono::milliseconds(vpn::kRtoMs));
  CHECK(waited < std::chrono::milliseconds(2 * vpn::kRtoMs + 300));  // backed off once, at most
  // Woken for the connection, its resends and its worker, not 10 times a second.
  CHECK(r->get()->state().ticks < 15);
  r->reset();  // and stopping does not wait for a deadline 10 minutes away
}

TEST_CASE("vpn: a reset ends the link, a stray segment is reset, and UDP goes up and down",
          "[vpn]") {
  wire w;
  std::unique_ptr<sp::link> accepted;
  std::vector<std::pair<vpn::flow, std::string>> datagrams;
  vpn::stack s({[&](std::vector<std::uint8_t> p) { w.push(std::move(p)); },
                [&](std::unique_ptr<sp::link> l, const vpn::flow&) { accepted = std::move(l); },
                [&](const vpn::flow& f, std::span<const std::uint8_t> d) {
                  datagrams.emplace_back(f, std::string(d.begin(), d.end()));
                },
                {}});
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

TEST_CASE("vpn: the relay carries a key holder's datagram flows, DNS to its own resolver",
          "[vpn]") {
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

TEST_CASE("vpn: a VPN host's limits: UDP flows apart from TCP, its outside address, UDP's rate", "[vpn]") {
  relay::server::options d;
  relay::vpn_host_defaults(d);
  CHECK(d.forward_ports == std::vector<std::uint16_t>{relay::kAnyPort});
  CHECK(d.forward_udp_ports == std::vector<std::uint16_t>{relay::kAnyPort});
  CHECK(d.max_forwards == 128);
  CHECK(d.max_udp_flows == 128);
  CHECK(d.udp_out_rate == relay::kVpnHostUdpOutRate);

  tcp_echo te;
  udp_echo ue;
  relay::forward_answer said{};
  SECTION("UDP flows have their own cap: a full one never stops a TCP forward") {
    auto o = relay_options(te.port(), ue.port());
    o.max_forwards = 1;
    o.max_udp_flows = 1;
    auto s = relay::server::start(o);
    REQUIRE(s);
    const auto dial = [&] { return std::move(*sp::net::connect({"127.0.0.1", (*s)->port()}, 3000)); };
    auto u = relay::forward(dial(), kKey, "127.0.0.1", ue.port(), relay::kForwardWaitMs, said, relay::kForwardUdp);
    REQUIRE(u);
    CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", ue.port(), 5000, said, relay::kForwardUdp));
    CHECK(said == relay::forward_answer::busy);
    auto t = relay::forward(dial(), kKey, "127.0.0.1", te.port(), relay::kForwardWaitMs, said);
    REQUIRE(t);
    CHECK(said == relay::forward_answer::ok);
    CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", te.port(), 5000, said));
    CHECK(said == relay::forward_answer::busy);
  }
  SECTION("the home's outside name: its addresses refused, TCP and UDP") {
    auto o = relay_options(te.port(), ue.port());
    o.forward_refused = relay::refuse_name_addresses("127.0.0.1");  // a literal looks up as itself
    auto s = relay::server::start(o);
    REQUIRE(s);
    const auto dial = [&] { return std::move(*sp::net::connect({"127.0.0.1", (*s)->port()}, 3000)); };
    CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", te.port(), 5000, said));
    CHECK(said == relay::forward_answer::not_public);
    CHECK_FALSE(relay::forward(dial(), kKey, "127.0.0.1", ue.port(), 5000, said, relay::kForwardUdp));
    CHECK(said == relay::forward_answer::not_public);
    CHECK(!relay::refuse_name_addresses(""));  // no name: nothing refused
  }
  SECTION("UDP out is held to the rate, all flows together") {
    auto o = relay_options(te.port(), ue.port());
    o.udp_out_rate = 64 * 1024;  // a 16 KiB burst, then 64 KiB a second
    auto s = relay::server::start(o);
    REQUIRE(s);
    auto u = relay::forward(std::move(*sp::net::connect({"127.0.0.1", (*s)->port()}, 3000)), kKey, "127.0.0.1",
                            ue.port(), relay::kForwardWaitMs, said, relay::kForwardUdp);
    REQUIRE(u);
    const std::vector<std::uint8_t> one(1400, 'a');
    std::vector<std::uint8_t> part, back;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 48; ++i) {  // 67 200 bytes: about 51 KiB past the burst
      REQUIRE(relay::write_datagram(**u, one));
      REQUIRE(relay::read_datagram(**u, part, back, 5000));
    }
    // A floor, never a ceiling: at least the debt's time (0.78 s), with room.
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(600));
  }
}

TEST_CASE("vpn: an app's TCP and DNS go through the VPN and the relay, end to end", "[vpn]") {
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

TEST_CASE("vpn: All traffic (VPN) runs instead of the proxy, from the page and the menu", "[vpn]") {
  tcp_echo te;
  udp_echo ue;
  auto server = relay::server::start(relay_options(te.port(), ue.port()));
  REQUIRE(server);
  auto fv = std::make_unique<fake_vpn>();
  auto starts = fv->starts, stops = fv->stops;
  auto last = fv->last;
  auto holder = std::make_shared<std::unique_ptr<fake_vpn>>(std::move(fv));
  vpn_test::fake_host host;
  vpn::service::options o;
  o.make_vpn = [holder]() -> std::unique_ptr<vpn::os_port> { return std::move(*holder); };
  vpn::service eng(host, o);
  CHECK(eng.snapshot().all_traffic_available);
  CHECK_FALSE(eng.snapshot().all_traffic);  // off by default
  CHECK_FALSE(eng.snapshot().block);
  eng.on_event("server", 3, "\"127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey + "\"");
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));  // the proxy first
  auto v = eng.snapshot();
  CHECK(vpn::build_settings_tree(v).find("\"id\":\"all_traffic\"") != std::string::npos);
  CHECK(vpn::build_menu(v).find("\"id\":\"all_traffic\"") != std::string::npos);
  // The menu row's event is the page's: the VPN takes over.
  eng.on_event("all_traffic", 2, "true");
  REQUIRE(eventually([&] { return eng.snapshot().mode == "vpn" && eng.snapshot().tunnel_address.empty(); }));
  CHECK(*starts == 1);  // and no proxy now
  // What the OS side was told: the relay left out, and the local networks.
  REQUIRE_FALSE(last->exclude.empty());
  CHECK(sp::net::address_text(last->exclude[0]) == "127.0.0.1");
  CHECK(sp::net::parse_address(last->address6).has_value());  // IPv6 claimed too
  CHECK_FALSE(last->kill_switch);
  v = eng.snapshot();
  const auto tree = vpn::build_settings_tree(v);
  CHECK(tree.find("All traffic through your VPN server") != std::string::npos);
  CHECK(tree.find("\"id\":\"block\"") != std::string::npos);
  REQUIRE(eventually([&] { return host.label().rfind("VPN: all traffic through 127.0.0.1:", 0) == 0; }));
  // The kill switch restarts it with the switch on.
  eng.on_event("block", 2, "true");
  REQUIRE(eventually([&] { return *starts == 2; }));
  CHECK(last->kill_switch);
  // Off: the interface goes, the proxy comes back.
  eng.on_event("all_traffic", 2, "false");
  REQUIRE(eventually([&] { return eng.snapshot().mode == "proxy" && !eng.snapshot().tunnel_address.empty(); }));
  CHECK(*stops >= 2);
  eng.on_event("tunnel", 2, "false");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_address.empty(); }));
}

namespace {

// The OS's interface while the system asks the person (the Mac's extension
// approval): start() waits until the test answers with `answer`, or until
// cancel() (quitting).
struct asking_vpn final : vpn::os_port {
  std::shared_ptr<std::mutex> m = std::make_shared<std::mutex>();
  std::shared_ptr<std::condition_variable> cv = std::make_shared<std::condition_variable>();
  std::shared_ptr<bool> answered = std::make_shared<bool>(false);
  std::shared_ptr<mv::status> answer = std::make_shared<mv::status>(mv::status::permission_denied);
  std::shared_ptr<std::atomic<bool>> asking = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<int>> starts = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
  // Answer at once with `answer` (a start the test expects never to happen
  // fails its count instead of hanging the test).
  std::shared_ptr<std::atomic<bool>> at_once = std::make_shared<std::atomic<bool>>(false);
  std::string unavailable() override { return {}; }
  mv::result<std::unique_ptr<vpn::device>> start(const vpn::settings&) override {
    ++*starts;
    std::unique_lock lock(*m);
    if (at_once->load()) return mv::err(*answer);
    asking->store(true);
    cv->wait(lock, [&] { return *answered || *cancelled; });
    asking->store(false);
    if (*cancelled) return mv::err(mv::status::cancelled);
    *answered = false;  // the next start waits again
    return mv::err(*answer);
  }
  void stop() override {}
  void cancel() override {
    {
      std::lock_guard lock(*m);
      *cancelled = true;
    }
    cv->notify_all();
  }
};

void answer_with(asking_vpn& v, mv::status s) {
  {
    std::lock_guard lock(*v.m);
    *v.answer = s;
    *v.answered = true;
  }
  v.cv->notify_all();
}

}  // namespace

TEST_CASE("vpn: All traffic says it is starting while the system asks, and why it wasn't allowed", "[vpn]") {
  auto av = std::make_unique<asking_vpn>();
  asking_vpn& port = *av;
  auto asking = av->asking;
  auto starts = av->starts;
  auto holder = std::make_shared<std::unique_ptr<asking_vpn>>(std::move(av));
  vpn_test::fake_host host;
  vpn::service::options o;
  o.make_vpn = [holder]() -> std::unique_ptr<vpn::os_port> { return std::move(*holder); };
  vpn::service eng(host, o);
  eng.settings_opened();
  eng.on_event("server", 3, std::string("\"127.0.0.1:9#") + kKey + "\"");
  eng.on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !eng.snapshot().tunnel_address.empty(); }));
  eng.on_event("all_traffic", 2, "true");
  // While the system asks: the page, posted from the waiting thread, says what to expect.
  REQUIRE(eventually([&] { return asking->load(); }));
  REQUIRE(eventually([&] { return host.settings().find("Starting All traffic") != std::string::npos; }));
  CHECK(host.settings().find("\"id\":\"all_traffic\"") != std::string::npos);  // the switch stays
  answer_with(port, mv::status::permission_denied);
  // Refused: the proxy carries on, the page says where to allow it, and the
  // switch goes off, so nothing asks the person again until they turn it on.
  REQUIRE(eventually([&] { return host.settings().find("wasn't allowed") != std::string::npos; }));
  CHECK(host.settings().find("Starting All traffic") == std::string::npos);
  CHECK(host.settings().find("Login Items & Extensions") != std::string::npos);
  CHECK(eng.snapshot().mode == "proxy");
  REQUIRE(eventually([&] { return !eng.snapshot().all_traffic; }));
  CHECK(host.settings().find(R"x("id":"all_traffic","label":"All traffic (VPN)","value":false)x") != std::string::npos);
  port.at_once->store(true);           // from here a start answers at once: counted, never hanging
  eng.on_event("tls", 2, "true");      // an unrelated change: no new start, no new prompt
  eng.on_event("tls", 2, "false");
  eng.settle();
  CHECK(*starts == 1);
  CHECK(host.settings().find("wasn't allowed") != std::string::npos);  // the words stay until the switch moves
  port.at_once->store(false);
  // On again, and the system says it finishes at the next restart: off too.
  eng.on_event("all_traffic", 2, "true");
  REQUIRE(eventually([&] { return asking->load(); }));
  answer_with(port, mv::status::busy);
  REQUIRE(eventually([&] { return host.settings().find("Restart this computer") != std::string::npos; }));
  CHECK(host.settings().find("wasn't allowed") == std::string::npos);
  REQUIRE(eventually([&] { return !eng.snapshot().all_traffic; }));
  // On again, and it fails for a passing reason (a timeout at launch): the
  // switch and the setting stay on, and it isn't tried again this run...
  eng.on_event("all_traffic", 2, "true");
  REQUIRE(eventually([&] { return asking->load(); }));
  answer_with(port, mv::status::timeout);
  REQUIRE(eventually([&] { return host.settings().find("could not start") != std::string::npos; }));
  CHECK(eng.snapshot().all_traffic);
  const int tried = *starts;
  {
    std::lock_guard lock(*port.m);
    *port.answer = mv::status::timeout;
  }
  port.at_once->store(true);
  eng.on_event("tls", 2, "true");
  eng.on_event("tls", 2, "false");
  eng.settle();
  CHECK(*starts == tried);
  CHECK(host.settings().find("could not start") != std::string::npos);
  // ...until the person flips it: off clears the words, on tries again.
  eng.on_event("all_traffic", 2, "false");
  REQUIRE(eventually([&] { return host.settings().find("could not start") == std::string::npos; }));
  eng.on_event("all_traffic", 2, "true");
  REQUIRE(eventually([&] { return *starts == tried + 1; }));
  eng.on_event("tunnel", 2, "false");
  REQUIRE(eventually([&] { return eng.snapshot().tunnel_address.empty(); }));
}

TEST_CASE("vpn: quitting while the system asks gives up at once", "[vpn]") {
  auto av = std::make_unique<asking_vpn>();
  auto asking = av->asking;
  auto holder = std::make_shared<std::unique_ptr<asking_vpn>>(std::move(av));
  vpn_test::fake_host host;
  vpn::service::options o;
  o.make_vpn = [holder]() -> std::unique_ptr<vpn::os_port> { return std::move(*holder); };
  auto eng = std::make_unique<vpn::service>(host, o);
  eng->on_event("server", 3, std::string("\"127.0.0.1:9#") + kKey + "\"");
  eng->on_event("tunnel", 2, "true");
  REQUIRE(eventually([&] { return !eng->snapshot().tunnel_address.empty(); }));
  eng->on_event("all_traffic", 2, "true");
  REQUIRE(eventually([&] { return asking->load(); }));
  // The prompt never answered: without cancel() this would never return.
  std::atomic<bool> gone{false};
  std::thread quit([&] {
    eng.reset();
    gone.store(true);
  });
  CHECK(eventually([&] { return gone.load(); }));
  quit.join();
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

TEST_CASE("vpn: routes on a Mac claim IPv6 too, the relay and the local networks left out",
          "[vpn][vpn-routes]") {
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

TEST_CASE("vpn: routes on Windows claim IPv6 in two halves, the relay and the local networks left out",
          "[vpn][vpn-routes]") {
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

TEST_CASE("vpn: an app's IPv6 goes through the relay, and is never passed on untouched",
          "[vpn][vpn-routes]") {
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
