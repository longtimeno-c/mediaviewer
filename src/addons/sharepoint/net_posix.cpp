// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net.h over BSD sockets (macOS, and the portable build).
#include "addons/sharepoint/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <net/route.h>
#include <sys/sysctl.h>
#endif

#include <atomic>
#include <fstream>
#include <vector>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "addons/sharepoint/wire.h"

namespace mv::sharepoint::net {

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void no_sigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}

void close_on_exec(int fd) noexcept { (void)::fcntl(fd, F_SETFD, FD_CLOEXEC); }

// Waits for `events` on `fd`, or a timeout (< 0: forever). Retries EINTR.
int wait_for(int fd, short events, int timeout_ms) noexcept {
  pollfd p{fd, events, 0};
  for (;;) {
    const int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return r;
    return p.revents;
  }
}

class tcp_link final : public link {
 public:
  explicit tcp_link(int fd) : fd_(fd) {
    no_sigpipe(fd_);
    close_on_exec(fd_);
    int one = 1;
    (void)::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    // A stalled write is a dropped link: the channel pings, so a healthy
    // receiver never leaves the send buffer full this long.
    timeval tv{kSilenceMs / 1000, 0};
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  }
  ~tcp_link() override {
    shutdown();
    ::close(fd_);
  }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      if (down_.load()) return err(status::cancelled);
      const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, kSendFlags);
      if (n < 0) {
        if (errno == EINTR) continue;
        return err(errno == EAGAIN || errno == EWOULDBLOCK ? status::timeout : status::io);
      }
      sent += static_cast<std::size_t>(n);
    }
    return {};
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    if (into.empty()) return std::size_t{0};
    for (;;) {
      if (down_.load()) return err(status::cancelled);
      const int ev = wait_for(fd_, POLLIN, timeout_ms);
      if (ev == 0) return err(status::timeout);
      if (ev < 0) return err(status::io);
      const ssize_t n = ::recv(fd_, into.data(), into.size(), 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return err(down_.load() ? status::cancelled : status::io);
      }
      return static_cast<std::size_t>(n);
    }
  }

  void shutdown() noexcept override {
    if (!down_.exchange(true)) (void)::shutdown(fd_, SHUT_RDWR);
  }

  transport via() const noexcept override { return transport::wifi; }

 private:
  int fd_;
  std::atomic<bool> down_{false};
};

class tcp_listener final : public listener {
 public:
  tcp_listener(int fd, std::uint16_t port) : fd_(fd), port_(port) {}
  ~tcp_listener() override {
    shutdown();
    ::close(fd_);
  }
  std::uint16_t port() const noexcept override { return port_; }

  result<std::unique_ptr<link>> accept(int timeout_ms) override {
    // Short polls, so shutdown() is noticed without closing the fd under a
    // thread that is using it.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
      if (down_.load()) return err(status::cancelled);
      const int ev = wait_for(fd_, POLLIN, 200);
      if (ev < 0) return err(status::io);
      if (ev > 0) {
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c >= 0) return std::unique_ptr<link>(std::make_unique<tcp_link>(c));
        if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) return err(status::io);
      }
      if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= until) return err(status::timeout);
    }
  }

  void shutdown() noexcept override { down_.store(true); }

 private:
  int fd_;
  std::uint16_t port_;
  std::atomic<bool> down_{false};
};

class udp_beacon final : public beacon_socket {
 public:
  udp_beacon(int fd, std::uint16_t port) : fd_(fd), port_(port) {}
  ~udp_beacon() override {
    shutdown();
    ::close(fd_);
  }

  expected send(std::span<const std::uint8_t> datagram) override {
    bool any = false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port_);
    if (::inet_pton(AF_INET, kMulticastGroup, &to.sin_addr) == 1 &&
        ::sendto(fd_, datagram.data(), datagram.size(), kSendFlags, reinterpret_cast<sockaddr*>(&to), sizeof to) >= 0) {
      any = true;
    }
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (::sendto(fd_, datagram.data(), datagram.size(), kSendFlags, reinterpret_cast<sockaddr*>(&to), sizeof to) >= 0) {
      any = true;
    }
    return any ? expected{} : err(status::io);
  }

  result<std::size_t> receive(std::span<std::uint8_t> into, int timeout_ms, std::string& from_address) override {
    if (down_.load()) return err(status::cancelled);
    const int ev = wait_for(fd_, POLLIN, timeout_ms);
    if (ev == 0) return err(status::timeout);
    if (ev < 0) return err(status::io);
    sockaddr_in from{};
    socklen_t len = sizeof from;
    const ssize_t n = ::recvfrom(fd_, into.data(), into.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0) return err(errno == EINTR || errno == EAGAIN ? status::timeout : status::io);
    char text[INET_ADDRSTRLEN] = {};
    if (!::inet_ntop(AF_INET, &from.sin_addr, text, sizeof text)) return err(status::io);
    from_address = text;
    return static_cast<std::size_t>(n);
  }

  void shutdown() noexcept override { down_.store(true); }

 private:
  int fd_;
  std::uint16_t port_;
  std::atomic<bool> down_{false};
};

}  // namespace

result<std::unique_ptr<listener>> listen(bool loopback_only) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return err(status::io);
  close_on_exec(fd);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0 ||
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
    ::close(fd);
    return err(status::io);
  }
  return std::unique_ptr<listener>(std::make_unique<tcp_listener>(fd, ntohs(a.sin_port)));
}

namespace {

// A sockaddr for `to` (either family); its length, 0 when it does not parse.
socklen_t sockaddr_of(const endpoint& to, sockaddr_storage& out) noexcept {
  const auto ip = parse_address(to.address);
  if (!ip || to.port == 0) return 0;
  std::memset(&out, 0, sizeof out);
  if (ip->v6) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&out);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(to.port);
    std::memcpy(&a->sin6_addr, ip->bytes.data(), 16);
    return sizeof *a;
  }
  auto* a = reinterpret_cast<sockaddr_in*>(&out);
  a->sin_family = AF_INET;
  a->sin_port = htons(to.port);
  std::memcpy(&a->sin_addr, ip->bytes.data(), 4);
  return sizeof *a;
}

endpoint endpoint_of(const sockaddr_storage& s) {
  address ip;
  std::uint16_t port = 0;
  if (s.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&s);
    ip.v6 = true;
    std::memcpy(ip.bytes.data(), &a->sin6_addr, 16);
    port = ntohs(a->sin6_port);
  } else {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&s);
    std::memcpy(ip.bytes.data(), &a->sin_addr, 4);
    port = ntohs(a->sin_port);
  }
  return {address_text(ip), port};
}

}  // namespace

result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms) {
  sockaddr_storage a;
  const socklen_t alen = sockaddr_of(to, a);
  if (alen == 0) return err(status::invalid_arg);
  const int fd = ::socket(a.ss_family, SOCK_STREAM, 0);
  if (fd < 0) return err(status::io);
  close_on_exec(fd);
  no_sigpipe(fd);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), alen);
  if (r != 0 && errno == EINPROGRESS) {
    const int ev = wait_for(fd, POLLOUT, timeout_ms);
    int so = 0;
    socklen_t sl = sizeof so;
    r = ev > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && so == 0 ? 0 : -1;
    if (ev == 0) {
      ::close(fd);
      return err(status::timeout);
    }
  }
  if (r != 0) {
    ::close(fd);
    return err(status::io);
  }
  (void)::fcntl(fd, F_SETFL, flags);
  return std::unique_ptr<link>(std::make_unique<tcp_link>(fd));
}

result<std::unique_ptr<listener>> listen_internet(std::uint16_t port, bool loopback_only) {
  // Dual stack where IPv6 exists; IPv4 alone where it does not.
  int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  sockaddr_storage s{};
  socklen_t len = 0;
  int one = 1;
  if (fd >= 0) {
    int zero = 0;
    (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, loopback_only ? &one : &zero, sizeof zero);
    auto* a = reinterpret_cast<sockaddr_in6*>(&s);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    a->sin6_addr = loopback_only ? in6addr_loopback : in6addr_any;
    len = sizeof *a;
  } else {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return err(status::io);
    auto* a = reinterpret_cast<sockaddr_in*>(&s);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    len = sizeof *a;
  }
  close_on_exec(fd);
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&s), len) != 0 || ::listen(fd, 8) != 0 ||
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&s), &len) != 0) {
    ::close(fd);
    return err(status::io);
  }
  return std::unique_ptr<listener>(std::make_unique<tcp_listener>(fd, endpoint_of(s).port));
}

std::vector<address> local_addresses() {
  std::vector<address> out;
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return out;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    address a;
    if (i->ifa_addr->sa_family == AF_INET) {
      std::memcpy(a.bytes.data(), &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, 4);
    } else if (i->ifa_addr->sa_family == AF_INET6) {
      a.v6 = true;
      std::memcpy(a.bytes.data(), &reinterpret_cast<const sockaddr_in6*>(i->ifa_addr)->sin6_addr, 16);
    } else {
      continue;
    }
    bool seen = false;
    for (const auto& o : out) seen = seen || o == a;
    if (!seen) out.push_back(a);
  }
  ::freeifaddrs(list);
  return out;
}

std::optional<address> default_gateway() {
#if defined(__APPLE__)
  // The routing table's default IPv4 route (what `route -n get default` reads).
  int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
  std::size_t len = 0;
  if (::sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0) return std::nullopt;
  std::vector<char> buf(len);
  if (::sysctl(mib, 6, buf.data(), &len, nullptr, 0) != 0) return std::nullopt;
  for (std::size_t at = 0; at + sizeof(rt_msghdr) <= len;) {
    const auto* rt = reinterpret_cast<const rt_msghdr*>(buf.data() + at);
    if (rt->rtm_msglen == 0) break;
    const auto* sa = reinterpret_cast<const sockaddr*>(rt + 1);
    const sockaddr* parts[RTAX_MAX] = {};
    for (int k = 0; k < RTAX_MAX; ++k) {
      if (!(rt->rtm_addrs & (1 << k))) continue;
      parts[k] = sa;
      const std::size_t sl = sa->sa_len ? ((sa->sa_len + sizeof(std::uint32_t) - 1) & ~(sizeof(std::uint32_t) - 1))
                                        : sizeof(std::uint32_t);
      sa = reinterpret_cast<const sockaddr*>(reinterpret_cast<const char*>(sa) + sl);
    }
    const auto* dst = reinterpret_cast<const sockaddr_in*>(parts[RTAX_DST]);
    const auto* gw = reinterpret_cast<const sockaddr_in*>(parts[RTAX_GATEWAY]);
    if (dst && gw && dst->sin_family == AF_INET && gw->sin_family == AF_INET && dst->sin_addr.s_addr == 0) {
      address a;
      std::memcpy(a.bytes.data(), &gw->sin_addr, 4);
      return a;
    }
    at += rt->rtm_msglen;
  }
  return std::nullopt;
#else
  // Linux: /proc/net/route, the line whose destination is 0.
  std::ifstream f("/proc/net/route");
  std::string iface, dest, gw;
  std::string line;
  std::getline(f, line);
  while (f >> iface >> dest >> gw) {
    std::getline(f, line);
    if (dest != "00000000") continue;
    const unsigned long v = std::strtoul(gw.c_str(), nullptr, 16);
    address a;
    std::memcpy(a.bytes.data(), &v, 4);  // little-endian text of a network-order word
    return a;
  }
  return std::nullopt;
#endif
}

namespace {

class udp_port final : public udp_socket {
 public:
  explicit udp_port(int fd) : fd_(fd) {}
  ~udp_port() override {
    shutdown();
    ::close(fd_);
  }
  expected send_to(std::span<const std::uint8_t> d, const endpoint& to) override {
    sockaddr_storage a;
    const socklen_t alen = sockaddr_of(to, a);
    if (alen == 0) return err(status::invalid_arg);
    if (::sendto(fd_, d.data(), d.size(), kSendFlags, reinterpret_cast<sockaddr*>(&a), alen) < 0) return err(status::io);
    return {};
  }
  result<std::size_t> receive_from(std::span<std::uint8_t> into, int timeout_ms, endpoint& from) override {
    if (down_.load()) return err(status::cancelled);
    const int ev = wait_for(fd_, POLLIN, timeout_ms);
    if (ev == 0) return err(status::timeout);
    if (ev < 0) return err(status::io);
    sockaddr_storage s{};
    socklen_t len = sizeof s;
    const ssize_t n = ::recvfrom(fd_, into.data(), into.size(), 0, reinterpret_cast<sockaddr*>(&s), &len);
    if (n < 0) return err(errno == EINTR || errno == EAGAIN ? status::timeout : status::io);
    from = endpoint_of(s);
    return static_cast<std::size_t>(n);
  }
  void shutdown() noexcept override { down_.store(true); }

 private:
  int fd_;
  std::atomic<bool> down_{false};
};

}  // namespace

result<std::unique_ptr<udp_socket>> open_udp(bool v6, int multicast_ttl) {
  const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return err(status::io);
  close_on_exec(fd);
  no_sigpipe(fd);
  if (v6) {
    int hops = multicast_ttl;
    (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof hops);
  } else {
    unsigned char ttl = static_cast<unsigned char>(multicast_ttl);
    (void)::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  }
  return std::unique_ptr<udp_socket>(std::make_unique<udp_port>(fd));
}

result<std::unique_ptr<beacon_socket>> open_beacon_socket(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return err(status::io);
  close_on_exec(fd);
  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  (void)::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  unsigned char ttl = 1;
  (void)::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  unsigned char loop = 1;  // two windows of one machine (and the tests) see each other
  (void)::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return err(status::io);
  }
  ip_mreq m{};
  if (::inet_pton(AF_INET, kMulticastGroup, &m.imr_multiaddr) == 1) {
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    (void)::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);  // broadcast still works without it
  }
  return std::unique_ptr<beacon_socket>(std::make_unique<udp_beacon>(fd, port));
}

namespace {
class flock_lock final : public instance_lock {
 public:
  explicit flock_lock(int fd) : fd_(fd) {}
  ~flock_lock() override { ::close(fd_); }  // closing lets go of the lock

 private:
  int fd_;
};
}  // namespace

std::unique_ptr<instance_lock> try_lock_instance(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return nullptr;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<flock_lock>(fd);
}

std::string downloads_folder() {
  std::string home;
  if (const char* h = std::getenv("HOME"); h && *h) {
    home = h;
  } else if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) {
    home = pw->pw_dir;
  }
  if (home.empty()) return {};
  while (home.size() > 1 && home.back() == '/') home.pop_back();
  return home + "/Downloads";
}

#if !defined(__APPLE__)
std::string machine_name() {
  char host[256] = {};
  if (::gethostname(host, sizeof host - 1) != 0) return clean_name("");
  std::string s(host);
  if (const auto dot = s.find('.'); dot != std::string::npos) s.resize(dot);
  return clean_name(s);
}
#endif

}  // namespace mv::sharepoint::net
