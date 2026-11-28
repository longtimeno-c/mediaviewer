// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Driftwire VPN under a speed test's load: the machine's own TCP (a real
// Linux TUN interface, the kernel's stack standing in for the phone's) through
// vpn::runner and a relay server reached over an emulated phone path (a round
// trip, a rate each way) to a stand-in site and resolver on loopback. One JSON
// line per run.
//
//   sudo mv_import_tests "[.driftwire-vpn-bench]"     (needs /dev/net/tun and `ip`)
//     MV_VPN_BENCH_RTT_MS     the phone <-> relay round trip (default 40)
//     MV_VPN_BENCH_MBPS       the path's rate each way, Mbit/s (default 100; 0: none)
//     MV_VPN_BENCH_STREAMS    parallel downloads and uploads (default 16)
//     MV_VPN_BENCH_MB         MB per stream (default 4)
//     MV_VPN_BENCH_LOOKUPS    DNS lookups at once (default 32)
//
// Loopback, so the machine's own load shows: run base and new alternated.
#include "driftwire_fixture.h"

#ifdef __linux__

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "addons/vpn/vpn.h"
#include "wire/splice.h"

namespace vpn = mv::driftwire::vpn;
namespace relay = mv::driftwire::relay;
using bench_clock = std::chrono::steady_clock;

namespace {

constexpr const char* kKey = "TEST-RELAY-KEY-0123456789";
constexpr const char* kIf = "mvbench0";
constexpr const char* kSite = "203.0.113.10";  // TEST-NET-3, routed into the TUN

double env_num(const char* name, double fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atof(v) : fallback;
}

double ms_since(bench_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(bench_clock::now() - t).count();
}

// The OS's interface: a TUN without packet info.
struct tun_device final : vpn::device {
  int fd = -1;
  ~tun_device() override {
    if (fd >= 0) ::close(fd);
  }
  mv::result<std::size_t> read(std::span<std::uint8_t> into, int timeout_ms) override {
    pollfd p{fd, POLLIN, 0};
    const int r = ::poll(&p, 1, timeout_ms);
    if (r == 0) return mv::err(mv::status::timeout);
    if (r < 0 || !(p.revents & POLLIN)) return mv::err(mv::status::io);
    const auto n = ::read(fd, into.data(), into.size());
    if (n <= 0) return mv::err(mv::status::io);
    return static_cast<std::size_t>(n);
  }
  mv::expected write(std::span<const std::uint8_t> packet) override {
    const auto n = ::write(fd, packet.data(), packet.size());
    if (n != static_cast<ssize_t>(packet.size())) return mv::err(mv::status::io);
    return {};
  }
  void shutdown() noexcept override {}
};

void drop_tun() {
  if (std::system((std::string("ip link del ") + kIf + " 2>/dev/null").c_str()) != 0) {}  // none: fine
}

std::unique_ptr<tun_device> open_tun() {
  const int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
  if (fd < 0) return nullptr;
  ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", kIf);
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    ::close(fd);
    return nullptr;
  }
  auto d = std::make_unique<tun_device>();
  d->fd = fd;
  const std::string up = std::string("ip addr add 198.18.0.1/30 dev ") + kIf + " && ip link set " + kIf +
                         " mtu 1500 up && ip route add 203.0.113.0/24 dev " + kIf;
  if (std::system(up.c_str()) != 0) return nullptr;
  return d;
}

// The phone's one path: a rate each way that every connection over it shares.
struct bottleneck {
  std::mutex m;
  bench_clock::time_point up{}, down{};  // when each direction is next free
};

// A link over the emulated path: `one_way_ms` each way, and `bytes_per_s`
// each way (0: no cap) shared through `b`, with up to 256 KB queued per link each way.
class path_link final : public sp::link {
 public:
  path_link(std::unique_ptr<sp::link> inner, int one_way_ms, double bytes_per_s, std::shared_ptr<bottleneck> b)
      : inner_(std::move(inner)), delay_(std::chrono::milliseconds(one_way_ms)), rate_(bytes_per_s), b_(std::move(b)) {
    tx_ = std::thread([this] { send_loop(); });
    rx_ = std::thread([this] { receive_loop(); });
  }
  ~path_link() override {
    shutdown();
    tx_.join();
    rx_.join();
  }
  mv::expected write_all(std::span<const std::uint8_t> b) override {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return over_ || queued_ < kQueue; });
    if (over_) return mv::err(mv::status::io);
    out_.push_back({due(true, b.size()), {b.begin(), b.end()}});
    queued_ += b.size();
    cv_.notify_all();
    return {};
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    const auto deadline = bench_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(m_);
    for (;;) {
      if (!in_.empty() && in_.front().at <= bench_clock::now()) {
        auto& c = in_.front();
        const std::size_t n = std::min(into.size(), c.bytes.size() - c.used);
        std::copy_n(c.bytes.begin() + static_cast<std::ptrdiff_t>(c.used), n, into.begin());
        c.used += n;
        if (c.used == c.bytes.size()) {
          in_queued_ -= c.bytes.size();
          in_.pop_front();
          cv_.notify_all();
        }
        return n;
      }
      if (in_.empty() && in_ended_) return std::size_t{0};
      if (over_ && in_.empty()) return mv::err(mv::status::io);
      const auto wake = in_.empty() ? deadline : std::min(deadline, in_.front().at);
      if (bench_clock::now() >= deadline) return mv::err(mv::status::timeout);
      cv_.wait_until(lock, wake);
    }
  }
  void shutdown() noexcept override {
    {
      std::lock_guard lock(m_);
      over_ = true;
      cv_.notify_all();
    }
    inner_->shutdown();
  }
  [[nodiscard]] sp::transport via() const noexcept override { return inner_->via(); }

 private:
  static constexpr std::size_t kQueue = 256 * 1024;
  struct chunk {
    bench_clock::time_point at;
    std::vector<std::uint8_t> bytes;
    std::size_t used = 0;
  };
  // When `n` bytes through a bottleneck free at `free` arrive at the far end.
  bench_clock::time_point due(bool up, std::size_t n) {
    std::lock_guard lock(b_->m);
    auto& free = up ? b_->up : b_->down;
    const auto now = bench_clock::now();
    if (free < now) free = now;
    if (rate_ > 0) {
      free += std::chrono::duration_cast<bench_clock::duration>(std::chrono::duration<double>(n / rate_));
    }
    return free + delay_;
  }
  void send_loop() {
    std::unique_lock lock(m_);
    for (;;) {
      cv_.wait(lock, [&] { return over_ || !out_.empty(); });
      if (over_) return;
      const auto at = out_.front().at;
      if (at > bench_clock::now()) {
        cv_.wait_until(lock, at, [&] { return over_.load(); });
        continue;
      }
      auto c = std::move(out_.front());
      out_.pop_front();
      queued_ -= c.bytes.size();
      cv_.notify_all();
      lock.unlock();
      const bool ok = static_cast<bool>(inner_->write_all(c.bytes));
      lock.lock();
      if (!ok) {
        over_ = true;
        cv_.notify_all();
        return;
      }
    }
  }
  void receive_loop() {
    std::vector<std::uint8_t> buf(65536);
    for (;;) {
      if (over_.load()) return;
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return over_ || in_queued_ < kQueue; });
      }
      auto n = inner_->read_some(buf, 100);
      if (!n && n.error() == mv::status::timeout) continue;
      std::lock_guard lock(m_);
      if (n && *n) in_queued_ += *n;
      if (!n || *n == 0) {
        in_ended_ = true;
        cv_.notify_all();
        return;
      }
      in_.push_back({due(false, *n), {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)}});
      cv_.notify_all();
    }
  }

  std::unique_ptr<sp::link> inner_;
  bench_clock::duration delay_;
  double rate_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<chunk> out_, in_;  // under m_
  std::size_t queued_ = 0, in_queued_ = 0;  // under m_
  std::shared_ptr<bottleneck> b_;
  std::atomic<bool> over_{false};
  bool in_ended_ = false;
  std::thread tx_, rx_;
};

// The site: 'D' + 4-byte count: that many bytes back; 'U' + count: reads
// them, then one byte back.
struct site {
  std::unique_ptr<sp::net::listener> l;
  std::thread t;
  std::atomic<bool> stop{false};
  std::vector<std::thread> conns;
  site() {
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
        conns.emplace_back([this, one] { serve(*one); });
      }
    });
  }
  void serve(sp::link& one) {
    std::vector<std::uint8_t> head;
    std::vector<std::uint8_t> b(65536);
    while (head.size() < 5 && !stop.load()) {
      auto n = one.read_some(b, 200);
      if (!n && n.error() == mv::status::timeout) continue;
      if (!n || *n == 0) return one.shutdown();
      head.insert(head.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    if (head.size() < 5) return one.shutdown();
    const std::uint64_t count = std::uint64_t{head[1]} << 24 | std::uint64_t{head[2]} << 16 |
                                std::uint64_t{head[3]} << 8 | head[4];
    if (head[0] == 'D') {
      std::fill(b.begin(), b.end(), 'x');
      for (std::uint64_t sent = 0; sent < count && !stop.load();) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), count - sent));
        if (!one.write_all(std::span<const std::uint8_t>(b.data(), n))) break;
        sent += n;
      }
    } else {
      std::uint64_t got = head.size() - 5;
      while (got < count && !stop.load()) {
        auto n = one.read_some(b, 200);
        if (!n && n.error() == mv::status::timeout) continue;
        if (!n || *n == 0) break;
        got += *n;
      }
      const std::uint8_t done = 1;
      (void)one.write_all(std::span<const std::uint8_t>(&done, 1));
    }
    // Lingers for the client's close, so the last bytes are not reset away.
    for (int i = 0; i < 50; ++i) {
      auto n = one.read_some(b, 200);
      if (!(n || n.error() == mv::status::timeout) || (n && *n == 0)) break;
    }
    one.shutdown();
  }
  ~site() {
    stop.store(true);
    l->shutdown();
    t.join();
    for (auto& c : conns) c.join();
  }
  std::uint16_t port() const { return l->port(); }
};

// The resolver: each datagram back as it came (its ID kept, as a real one's).
struct resolver {
  std::unique_ptr<sp::net::udp_socket> s;
  std::thread t;
  std::atomic<bool> stop{false};
  resolver() {
    auto x = sp::net::open_udp(false, 1, true);
    REQUIRE(x);
    s = std::move(*x);
    t = std::thread([this] {
      std::vector<std::uint8_t> b(2048);
      while (!stop.load()) {
        sp::net::endpoint from;
        auto n = s->receive_from(b, 200, from);
        if (!n) continue;
        (void)s->send_to(std::span<const std::uint8_t>(b.data(), *n), from);
      }
    });
  }
  ~resolver() {
    stop.store(true);
    s->shutdown();
    t.join();
  }
};

int tcp_to_site() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  ::inet_pton(AF_INET, kSite, &a.sin_addr);
  timeval tv{20, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool send_head(int fd, char what, std::uint32_t count) {
  const std::uint8_t h[5] = {static_cast<std::uint8_t>(what), static_cast<std::uint8_t>(count >> 24),
                             static_cast<std::uint8_t>(count >> 16), static_cast<std::uint8_t>(count >> 8),
                             static_cast<std::uint8_t>(count)};
  return ::send(fd, h, 5, MSG_NOSIGNAL) == 5;
}

struct stream_result {
  bool ok = false;
  double first_ms = 0;  // connect to the first byte back
  double total_ms = 0;
};

stream_result download(std::uint32_t bytes) {
  stream_result r;
  const auto t0 = bench_clock::now();
  const int fd = tcp_to_site();
  if (fd < 0) return r;
  std::vector<char> b(65536);
  std::uint64_t got = 0;
  if (send_head(fd, 'D', bytes)) {
    while (got < bytes) {
      const auto n = ::recv(fd, b.data(), b.size(), 0);
      if (n <= 0) break;
      if (got == 0) r.first_ms = ms_since(t0);
      got += static_cast<std::uint64_t>(n);
    }
  }
  ::close(fd);
  r.ok = got == bytes;
  r.total_ms = ms_since(t0);
  return r;
}

stream_result upload(std::uint32_t bytes) {
  stream_result r;
  const auto t0 = bench_clock::now();
  const int fd = tcp_to_site();
  if (fd < 0) return r;
  std::vector<char> b(65536, 'y');
  bool ok = send_head(fd, 'U', bytes);
  for (std::uint64_t sent = 0; ok && sent < bytes;) {
    const auto n = ::send(fd, b.data(), std::min<std::uint64_t>(b.size(), bytes - sent), MSG_NOSIGNAL);
    if (n <= 0) ok = false;
    else sent += static_cast<std::uint64_t>(n);
  }
  char done = 0;
  r.ok = ok && ::recv(fd, &done, 1, 0) == 1;
  ::close(fd);
  r.total_ms = ms_since(t0);
  return r;
}

// `n` lookups at once, each from its own port (as an OS sends them): the
// time each took to be answered (-1: not answered in 5 s).
std::vector<double> lookups(int n) {
  std::vector<double> took(static_cast<std::size_t>(n), -1);
  std::vector<std::thread> ts;
  for (int i = 0; i < n; ++i) {
    ts.emplace_back([i, &took] {
      const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(53);
      ::inet_pton(AF_INET, "198.18.0.2", &a.sin_addr);
      timeval tv{5, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      std::uint8_t q[32] = {static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i), 1, 0, 0, 1};
      q[12] = static_cast<std::uint8_t>(i);
      const auto t0 = bench_clock::now();
      ::sendto(fd, q, sizeof q, 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
      std::uint8_t back[64];
      const auto n = ::recv(fd, back, sizeof back, 0);
      if (n == static_cast<ssize_t>(sizeof q) && std::equal(q, q + sizeof q, back)) {
        took[static_cast<std::size_t>(i)] = ms_since(t0);
      }
      ::close(fd);
    });
  }
  for (auto& t : ts) t.join();
  return took;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))];
}

template <typename F>
std::vector<stream_result> at_once(int n, F f) {
  std::vector<stream_result> out(static_cast<std::size_t>(n));
  std::vector<std::thread> ts;
  for (int i = 0; i < n; ++i) ts.emplace_back([&, i] { out[static_cast<std::size_t>(i)] = f(); });
  for (auto& t : ts) t.join();
  return out;
}

}  // namespace

TEST_CASE("vpn: a speed test's load through a TUN, the relay over a phone path",
          "[.driftwire-vpn-bench]") {
  if (::geteuid() != 0 || ::access("/dev/net/tun", R_OK | W_OK) != 0) SKIP("needs root and /dev/net/tun");
  const int rtt = static_cast<int>(env_num("MV_VPN_BENCH_RTT_MS", 40));
  const double mbps = env_num("MV_VPN_BENCH_MBPS", 100);
  const int streams = static_cast<int>(env_num("MV_VPN_BENCH_STREAMS", 16));
  const auto per = static_cast<std::uint32_t>(env_num("MV_VPN_BENCH_MB", 4) * 1e6);
  const int nlookups = static_cast<int>(env_num("MV_VPN_BENCH_LOOKUPS", 32));

  drop_tun();
  site st;
  resolver dns;
  relay::server::options so;
  so.key = kKey;
  so.loopback_only = true;
  so.forward_private = true;
  so.forward_ports = {80};
  so.dns_upstream = sp::net::endpoint{"127.0.0.1", dns.s->port()};
  const auto site_port = st.port();
  so.forward_connect = [site_port](const std::string&, std::uint16_t, int timeout_ms, relay::forward_answer&)
      -> mv::result<std::unique_ptr<sp::link>> {
    return sp::net::connect(sp::net::endpoint{"127.0.0.1", site_port}, timeout_ms);
  };
  auto server = relay::server::start(so);
  REQUIRE(server);

  auto tun = open_tun();
  REQUIRE(tun);
  vpn::runner::options o;
  o.dial.relay = "127.0.0.1:" + std::to_string((*server)->port()) + "#" + kKey;
  o.dial.connect = [rtt, mbps, path = std::make_shared<bottleneck>()](const sp::net::endpoint& to, int timeout_ms) -> mv::result<std::unique_ptr<sp::link>> {
    std::this_thread::sleep_for(std::chrono::milliseconds(rtt));  // TCP's own handshake
    auto c = sp::net::connect(to, timeout_ms);
    if (!c) return mv::err(c.error());
    return std::unique_ptr<sp::link>(std::make_unique<path_link>(std::move(*c), rtt / 2, mbps * 1e6 / 8, path));
  };
  o.dev = std::move(tun);
  auto r = vpn::runner::start(std::move(o));
  REQUIRE(r);

  // DNS: a page's worth of lookups at once.
  auto t0 = bench_clock::now();
  const auto took = lookups(nlookups);
  const double dns_all_ms = ms_since(t0);
  std::vector<double> answered;
  for (double x : took) {
    if (x >= 0) answered.push_back(x);
  }
  // Opening: `streams` tiny fetches at once (a page's, a speed test's
  // first moment): what it costs to open, the path's rate aside.
  const auto opens = at_once(streams, [&] { return download(1); });
  std::vector<double> open_ms;
  for (const auto& d : opens) {
    if (d.ok) open_ms.push_back(d.total_ms);
  }
  // Download, then upload: `streams` at once, as a speed test opens them.
  t0 = bench_clock::now();
  const auto down = at_once(streams, [&] { return download(per); });
  const double down_s = ms_since(t0) / 1000;
  t0 = bench_clock::now();
  const auto up = at_once(streams, [&] { return upload(per); });
  const double up_s = ms_since(t0) / 1000;
  std::vector<double> first;
  int down_ok = 0, up_ok = 0;
  for (const auto& d : down) {
    if (d.ok) {
      ++down_ok;
      first.push_back(d.first_ms);
    }
  }
  for (const auto& u : up) up_ok += u.ok ? 1 : 0;
  const auto snap = r->get()->state();
  std::printf(
      "{\"rtt_ms\":%d,\"mbps\":%.0f,\"streams\":%d,\"mb\":%.1f,"
      "\"dns_answered\":%zu,\"dns_of\":%d,\"dns_p50_ms\":%.0f,\"dns_max_ms\":%.0f,\"dns_all_ms\":%.0f,"
      "\"open_ok\":%zu,\"open_p50_ms\":%.0f,\"open_max_ms\":%.0f,"
      "\"down_ok\":%d,\"down_mbps\":%.1f,\"first_byte_p50_ms\":%.0f,\"first_byte_max_ms\":%.0f,"
      "\"up_ok\":%d,\"up_mbps\":%.1f,\"forwards\":%llu,\"failed\":%llu}\n",
      rtt, mbps, streams, per / 1e6, answered.size(), nlookups, percentile(answered, 0.5),
      percentile(answered, 0.999), dns_all_ms, open_ms.size(), percentile(open_ms, 0.5),
      percentile(open_ms, 0.999), down_ok, down_ok * 8.0 * per / 1e6 / down_s,
      percentile(first, 0.5), percentile(first, 0.999), up_ok, up_ok * 8.0 * per / 1e6 / up_s,
      static_cast<unsigned long long>((*server)->snapshot().forwarded),
      static_cast<unsigned long long>(snap.failed));
  std::fflush(stdout);
  r->reset();
  drop_tun();
  CHECK(open_ms.size() == static_cast<std::size_t>(streams));
  CHECK(down_ok == streams);
  CHECK(up_ok == streams);
}

#endif  // __linux__
