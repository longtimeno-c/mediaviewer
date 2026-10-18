// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net.h over Winsock 2. The first listen asks Windows Defender Firewall, as
// any app that accepts a connection does; the person decides there.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
// mstcpip.h names it only for some targets; the value is fixed (Winsock).
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "addons/sharepoint/net.h"
#include "addons/sharepoint/wire.h"

namespace mv::sharepoint::net {

namespace {

bool winsock_ready() noexcept {
  static const bool ok = [] {
    WSADATA d{};
    return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  return ok;
}

int wait_for(SOCKET s, short events, int timeout_ms) noexcept {
  WSAPOLLFD p{};
  p.fd = s;
  p.events = events;
  const int r = ::WSAPoll(&p, 1, timeout_ms);
  if (r <= 0) return r;
  return p.revents;
}

void no_inherit(SOCKET s) noexcept {
  (void)::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
}

class tcp_link final : public link {
 public:
  explicit tcp_link(SOCKET s) : s_(s) {
    no_inherit(s_);
    BOOL one = TRUE;
    (void)::setsockopt(s_, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof one);
    // Non-blocking, waited on in slices: Windows' shutdown() does not wake a
    // thread blocked in send(), and a rejoin (#472) must not wait behind a
    // write on a link that is gone.
    u_long nb = 1;
    (void)::ioctlsocket(s_, FIONBIO, &nb);
  }
  ~tcp_link() override {
    shutdown();
    // A graceful close: whatever we sent reaches the peer. Closing a socket
    // with unread data makes Windows reset the connection, and the peer can
    // lose bytes it had not read yet (a "busy" answer, a cancel). Read off
    // what is left, briefly, first.
    char sink[4096];
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < until) {
      if (wait_for(s_, POLLRDNORM, 50) <= 0) continue;
      const int n = ::recv(s_, sink, sizeof sink, 0);
      if (n <= 0) break;
    }
    ::closesocket(s_);
  }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::size_t sent = 0;
    // A stalled write is a dropped link: the channel pings, so a healthy
    // receiver never leaves the send buffer full this long.
    auto stalled_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSilenceMs);
    while (sent < bytes.size()) {
      if (down_.load()) return err(status::cancelled);
      const int want = static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 1u << 20));
      const int n = ::send(s_, reinterpret_cast<const char*>(bytes.data() + sent), want, 0);
      if (n == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return err(down_.load() ? status::cancelled : status::io);
        // Full: wait for room in slices, so shutdown() is noticed.
        if (std::chrono::steady_clock::now() >= stalled_at) return err(status::timeout);
        const int ev = wait_for(s_, POLLWRNORM, 100);
        if (ev < 0) return err(status::io);
        continue;
      }
      sent += static_cast<std::size_t>(n);
      stalled_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSilenceMs);
    }
    return {};
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    if (into.empty()) return std::size_t{0};
    // In slices: closing the sending side does not wake a WSAPoll that waits
    // to read, so shutdown() is noticed here instead.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
      int ev = 0;
      for (;;) {
        if (down_.load()) return err(status::cancelled);
        int slice = 100;
        if (timeout_ms >= 0) {
          // Rounded up, so a timeout of 1 ms polls once rather than none.
          const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
          if (left.count() <= 0 && timeout_ms > 0) return err(status::timeout);
          slice = static_cast<int>(std::min<long long>(slice, std::max<long long>(left.count(), 0)));
        }
        ev = wait_for(s_, POLLRDNORM, slice);
        if (ev != 0) break;
        if (timeout_ms == 0) return err(status::timeout);
      }
      if (ev < 0) return err(status::io);
      if (down_.load()) return err(status::cancelled);
      const int want = static_cast<int>(std::min<std::size_t>(into.size(), 1u << 20));
      const int n = ::recv(s_, reinterpret_cast<char*>(into.data()), want, 0);
      if (n != SOCKET_ERROR) return static_cast<std::size_t>(n);
      // Readable but nothing after all (non-blocking): wait again.
      if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        if (timeout_ms == 0) return err(status::timeout);
        continue;
      }
      return err(down_.load() ? status::cancelled : status::io);
    }
  }

  void shutdown() noexcept override {
    // SD_SEND, not SD_BOTH: on Windows, data arriving after SD_RECEIVE
    // resets the connection, and the peer loses what it had not read.
    if (!down_.exchange(true)) (void)::shutdown(s_, SD_SEND);
  }

  transport via() const noexcept override { return transport::wifi; }

 private:
  SOCKET s_;
  std::atomic<bool> down_{false};
};

class tcp_listener final : public listener {
 public:
  tcp_listener(SOCKET s, std::uint16_t port) : s_(s), port_(port) {}
  ~tcp_listener() override {
    shutdown();
    ::closesocket(s_);
  }
  std::uint16_t port() const noexcept override { return port_; }

  result<std::unique_ptr<link>> accept(int timeout_ms) override {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
      if (down_.load()) return err(status::cancelled);
      const int ev = wait_for(s_, POLLRDNORM, 200);
      if (ev < 0) return err(status::io);
      if (ev > 0) {
        const SOCKET c = ::accept(s_, nullptr, nullptr);
        if (c != INVALID_SOCKET) return std::unique_ptr<link>(std::make_unique<tcp_link>(c));
        const int e = ::WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAECONNRESET && e != WSAEINTR) return err(status::io);
      }
      if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= until) return err(status::timeout);
    }
  }

  void shutdown() noexcept override { down_.store(true); }

 private:
  SOCKET s_;
  std::uint16_t port_;
  std::atomic<bool> down_{false};
};

class udp_beacon final : public beacon_socket {
 public:
  udp_beacon(SOCKET s, std::uint16_t port) : s_(s), port_(port) {}
  ~udp_beacon() override {
    shutdown();
    ::closesocket(s_);
  }

  expected send(std::span<const std::uint8_t> datagram) override {
    bool any = false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = ::htons(port_);
    const int len = static_cast<int>(datagram.size());
    const char* bytes = reinterpret_cast<const char*>(datagram.data());
    if (::inet_pton(AF_INET, kMulticastGroup, &to.sin_addr) == 1 &&
        ::sendto(s_, bytes, len, 0, reinterpret_cast<sockaddr*>(&to), sizeof to) != SOCKET_ERROR) {
      any = true;
    }
    to.sin_addr.s_addr = ::htonl(INADDR_BROADCAST);
    if (::sendto(s_, bytes, len, 0, reinterpret_cast<sockaddr*>(&to), sizeof to) != SOCKET_ERROR) any = true;
    return any ? expected{} : err(status::io);
  }

  result<std::size_t> receive(std::span<std::uint8_t> into, int timeout_ms, std::string& from_address) override {
    if (down_.load()) return err(status::cancelled);
    const int ev = wait_for(s_, POLLRDNORM, timeout_ms);
    if (ev == 0) return err(status::timeout);
    if (ev < 0) return err(status::io);
    sockaddr_in from{};
    int flen = sizeof from;
    const int n = ::recvfrom(s_, reinterpret_cast<char*>(into.data()), static_cast<int>(into.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
    if (n == SOCKET_ERROR) {
      // A datagram longer than the buffer is not a beacon; a port-unreachable
      // reply to our own send is noise.
      const int e = ::WSAGetLastError();
      return err(e == WSAEMSGSIZE || e == WSAECONNRESET ? status::timeout : status::io);
    }
    char text[INET_ADDRSTRLEN] = {};
    if (!::inet_ntop(AF_INET, &from.sin_addr, text, sizeof text)) return err(status::io);
    from_address = text;
    return static_cast<std::size_t>(n);
  }

  void shutdown() noexcept override { down_.store(true); }

 private:
  SOCKET s_;
  std::uint16_t port_;
  std::atomic<bool> down_{false};
};

}  // namespace

result<std::unique_ptr<listener>> listen(bool loopback_only) {
  if (!winsock_ready()) return err(status::io);
  const SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  // Nobody else may take our port while we hold it.
  BOOL excl = TRUE;
  (void)::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ::htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  a.sin_port = 0;
  int len = sizeof a;
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0 ||
      ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  return std::unique_ptr<listener>(std::make_unique<tcp_listener>(s, ::ntohs(a.sin_port)));
}

namespace {

// A sockaddr for `to` (either family); its length, 0 when it does not parse.
int sockaddr_of(const endpoint& to, sockaddr_storage& out) noexcept {
  const auto ip = parse_address(to.address);
  if (!ip || to.port == 0) return 0;
  std::memset(&out, 0, sizeof out);
  if (ip->v6) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&out);
    a->sin6_family = AF_INET6;
    a->sin6_port = ::htons(to.port);
    std::memcpy(&a->sin6_addr, ip->bytes.data(), 16);
    return static_cast<int>(sizeof *a);
  }
  auto* a = reinterpret_cast<sockaddr_in*>(&out);
  a->sin_family = AF_INET;
  a->sin_port = ::htons(to.port);
  std::memcpy(&a->sin_addr, ip->bytes.data(), 4);
  return static_cast<int>(sizeof *a);
}

address address_of(const sockaddr* sa) noexcept {
  address ip;
  if (sa->sa_family == AF_INET6) {
    const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
    static constexpr std::uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(&a6->sin6_addr, kMapped, 12) == 0) {
      // An IPv4 peer on a dual-stack socket: its plain address.
      std::memcpy(ip.bytes.data(), reinterpret_cast<const std::uint8_t*>(&a6->sin6_addr) + 12, 4);
      return ip;
    }
    ip.v6 = true;
    std::memcpy(ip.bytes.data(), &a6->sin6_addr, 16);
  } else {
    std::memcpy(ip.bytes.data(), &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
  }
  return ip;
}

std::uint16_t port_of(const sockaddr_storage& s) noexcept {
  return s.ss_family == AF_INET6 ? ::ntohs(reinterpret_cast<const sockaddr_in6*>(&s)->sin6_port)
                                 : ::ntohs(reinterpret_cast<const sockaddr_in*>(&s)->sin_port);
}

}  // namespace

result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms) {
  if (!winsock_ready()) return err(status::io);
  sockaddr_storage a;
  const int alen = sockaddr_of(to, a);
  if (alen == 0) return err(status::invalid_arg);
  const SOCKET s = ::WSASocketW(a.ss_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  u_long nb = 1;
  (void)::ioctlsocket(s, FIONBIO, &nb);
  int r = ::connect(s, reinterpret_cast<sockaddr*>(&a), alen);
  if (r != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK) {
    const int ev = wait_for(s, POLLWRNORM, timeout_ms);
    if (ev == 0) {
      ::closesocket(s);
      return err(status::timeout);
    }
    int so = 0;
    int sl = sizeof so;
    r = ev > 0 && !(ev & (POLLERR | POLLHUP)) &&
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl) == 0 && so == 0
            ? 0
            : -1;
  }
  if (r != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  nb = 0;
  (void)::ioctlsocket(s, FIONBIO, &nb);
  return std::unique_ptr<link>(std::make_unique<tcp_link>(s));
}

result<std::unique_ptr<listener>> listen_internet(std::uint16_t port, bool loopback_only) {
  if (!winsock_ready()) return err(status::io);
  // Dual stack where IPv6 exists; IPv4 alone where it does not.
  SOCKET s = ::WSASocketW(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  sockaddr_storage st{};
  int len = 0;
  if (s != INVALID_SOCKET) {
    DWORD v6only = loopback_only ? 1 : 0;
    (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    auto* a = reinterpret_cast<sockaddr_in6*>(&st);
    a->sin6_family = AF_INET6;
    a->sin6_port = ::htons(port);
    if (loopback_only) a->sin6_addr.s6_addr[15] = 1;  // ::1 (else ::, all zeros)
    len = static_cast<int>(sizeof *a);
  } else {
    s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return err(status::io);
    auto* a = reinterpret_cast<sockaddr_in*>(&st);
    a->sin_family = AF_INET;
    a->sin_port = ::htons(port);
    a->sin_addr.s_addr = ::htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    len = static_cast<int>(sizeof *a);
  }
  BOOL excl = TRUE;
  (void)::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  if (::bind(s, reinterpret_cast<sockaddr*>(&st), len) != 0 || ::listen(s, 8) != 0 ||
      ::getsockname(s, reinterpret_cast<sockaddr*>(&st), &len) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  return std::unique_ptr<listener>(std::make_unique<tcp_listener>(s, port_of(st)));
}

std::vector<address> local_addresses() {
  std::vector<address> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf;
  ULONG r = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
    buf.resize(size);
    r = ::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                               nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (r != NO_ERROR) return out;
  for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
    if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
      const sockaddr* sa = u->Address.lpSockaddr;
      if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
      const address a = address_of(sa);
      bool seen = false;
      for (const auto& o : out) seen = seen || o == a;
      if (!seen) out.push_back(a);
    }
  }
  return out;
}

std::optional<address> default_gateway() {
  // The best IPv4 route to anywhere public: its next hop is the router.
  SOCKADDR_INET dest{};
  dest.Ipv4.sin_family = AF_INET;
  dest.Ipv4.sin_addr.s_addr = ::htonl(0x08080808);  // a destination only; nothing is sent
  MIB_IPFORWARD_ROW2 row{};
  SOCKADDR_INET source{};
  if (::GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &row, &source) != NO_ERROR) return std::nullopt;
  if (row.NextHop.si_family != AF_INET || row.NextHop.Ipv4.sin_addr.s_addr == 0) return std::nullopt;
  address a;
  std::memcpy(a.bytes.data(), &row.NextHop.Ipv4.sin_addr, 4);
  return a;
}

std::optional<address> local_address_toward(const address& peer) {
  if (!winsock_ready()) return std::nullopt;
  sockaddr_storage a;
  const int alen = sockaddr_of({address_text(peer), 9}, a);
  if (alen == 0) return std::nullopt;
  const SOCKET s = ::WSASocketW(a.ss_family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return std::nullopt;
  std::optional<address> out;
  sockaddr_storage st{};
  int len = sizeof st;
  if (::connect(s, reinterpret_cast<sockaddr*>(&a), alen) == 0 &&
      ::getsockname(s, reinterpret_cast<sockaddr*>(&st), &len) == 0) {
    out = address_of(reinterpret_cast<const sockaddr*>(&st));
  }
  ::closesocket(s);
  return out;
}

namespace {

class udp_port final : public udp_socket {
 public:
  udp_port(SOCKET s, std::uint16_t port, bool dual = false) : s_(s), port_(port), dual_(dual) {}
  std::uint16_t port() const noexcept override { return port_; }
  ~udp_port() override {
    shutdown();
    ::closesocket(s_);
  }
  expected send_to(std::span<const std::uint8_t> d, const endpoint& to) override {
    sockaddr_storage a;
    int alen = sockaddr_of(to, a);
    if (alen == 0) return err(status::invalid_arg);
    if (dual_ && a.ss_family == AF_INET) {
      // An IPv4 peer from a dual-stack socket: ::ffff:a.b.c.d.
      const sockaddr_in v4 = *reinterpret_cast<const sockaddr_in*>(&a);
      auto* m = reinterpret_cast<sockaddr_in6*>(&a);
      std::memset(m, 0, sizeof *m);
      m->sin6_family = AF_INET6;
      m->sin6_port = v4.sin_port;
      m->sin6_addr.s6_addr[10] = 0xff;
      m->sin6_addr.s6_addr[11] = 0xff;
      std::memcpy(&m->sin6_addr.s6_addr[12], &v4.sin_addr, 4);
      alen = static_cast<int>(sizeof *m);
    }
    if (::sendto(s_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
                 reinterpret_cast<sockaddr*>(&a), alen) == SOCKET_ERROR) {
      return err(status::io);
    }
    return {};
  }
  result<std::size_t> receive_from(std::span<std::uint8_t> into, int timeout_ms, endpoint& from) override {
    if (down_.load()) return err(status::cancelled);
    const int ev = wait_for(s_, POLLRDNORM, timeout_ms);
    if (ev == 0) return err(status::timeout);
    if (ev < 0) return err(status::io);
    sockaddr_storage st{};
    int flen = sizeof st;
    const int n = ::recvfrom(s_, reinterpret_cast<char*>(into.data()), static_cast<int>(into.size()), 0,
                             reinterpret_cast<sockaddr*>(&st), &flen);
    if (n == SOCKET_ERROR) {
      const int e = ::WSAGetLastError();
      return err(e == WSAEMSGSIZE || e == WSAECONNRESET ? status::timeout : status::io);
    }
    from = {address_text(address_of(reinterpret_cast<const sockaddr*>(&st))), port_of(st)};
    return static_cast<std::size_t>(n);
  }
  void shutdown() noexcept override { down_.store(true); }
  std::intptr_t handle() const noexcept override { return static_cast<std::intptr_t>(s_); }

 private:
  SOCKET s_;
  std::uint16_t port_;
  bool dual_;
  std::atomic<bool> down_{false};
};

}  // namespace

result<std::size_t> wait_readable(std::span<udp_socket* const> socks, std::span<bool> ready, int timeout_ms) {
  std::vector<WSAPOLLFD> p(socks.size());
  for (std::size_t i = 0; i < socks.size(); ++i) {
    p[i].fd = static_cast<SOCKET>(socks[i]->handle());
    p[i].events = POLLRDNORM;
  }
  const int r = ::WSAPoll(p.data(), static_cast<ULONG>(p.size()), timeout_ms);
  if (r == SOCKET_ERROR) return err(status::io);
  if (r == 0) return err(status::timeout);
  std::size_t n = 0;
  for (std::size_t i = 0; i < socks.size() && i < ready.size(); ++i) {
    ready[i] = p[i].revents != 0;
    n += ready[i] ? 1 : 0;
  }
  return n;
}

void reserve_sockets(int) noexcept {}  // Winsock has no per-process socket limit to raise

namespace {

// A v4-mapped IPv6 address for an IPv4 peer of a dual-stack socket.
int mapped(sockaddr_storage& a, int alen) noexcept {
  if (a.ss_family != AF_INET) return alen;
  const sockaddr_in v4 = *reinterpret_cast<const sockaddr_in*>(&a);
  auto* m = reinterpret_cast<sockaddr_in6*>(&a);
  std::memset(m, 0, sizeof *m);
  m->sin6_family = AF_INET6;
  m->sin6_port = v4.sin_port;
  m->sin6_addr.s6_addr[10] = 0xff;
  m->sin6_addr.s6_addr[11] = 0xff;
  std::memcpy(&m->sin6_addr.s6_addr[12], &v4.sin_addr, 4);
  return static_cast<int>(sizeof *m);
}

class tcp_meet final : public tcp_rendezvous {
 public:
  tcp_meet(SOCKET s, const sockaddr_storage& local, int len, bool dual)
      : s_(s), local_(local), len_(len), dual_(dual), port_(port_of(local)) {}
  ~tcp_meet() override {
    for (auto& a : tries_) {
      if (a.s != INVALID_SOCKET) ::closesocket(a.s);
    }
    ::closesocket(s_);
  }
  std::uint16_t port() const noexcept override { return port_; }
  void shutdown() noexcept override { down_.store(true); }

  result<std::unique_ptr<link>> next(std::span<const endpoint> peers, int timeout_ms) override {
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
      if (down_.load()) return err(status::cancelled);
      const auto now = clock::now();
      for (const auto& p : peers) {
        attempt* a = find(p);
        if (!a) {
          tries_.push_back({p});
          a = &tries_.back();
        }
        // A SYN that was dropped is sent again soon, not at the OS's backoff.
        if (a->s != INVALID_SOCKET && now - a->started > std::chrono::milliseconds(4 * kRetryMs)) {
          ::closesocket(a->s);
          a->s = INVALID_SOCKET;
        }
        if (a->s == INVALID_SOCKET && now >= a->retry_at) start(*a, now);
      }
      std::vector<WSAPOLLFD> pf(1);
      pf[0].fd = s_;
      pf[0].events = POLLRDNORM;
      for (const auto& a : tries_) {
        if (a.s == INVALID_SOCKET) continue;
        WSAPOLLFD w{};
        w.fd = a.s;
        w.events = POLLWRNORM;
        pf.push_back(w);
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
      const int r = ::WSAPoll(pf.data(), static_cast<ULONG>(pf.size()),
                              static_cast<int>(std::clamp<long long>(left, 0, 50)));
      if (r == SOCKET_ERROR) return err(status::io);
      if (r > 0 && pf[0].revents != 0) {
        sockaddr_storage from{};
        int flen = sizeof from;
        const SOCKET c = ::accept(s_, reinterpret_cast<sockaddr*>(&from), &flen);
        if (c != INVALID_SOCKET) {
          const std::string at = address_text(address_of(reinterpret_cast<const sockaddr*>(&from)));
          const bool known =
              std::any_of(peers.begin(), peers.end(), [&](const endpoint& p) { return p.address == at; });
          if (known) return std::unique_ptr<link>(std::make_unique<tcp_link>(c));
          ::closesocket(c);
        }
      }
      if (r > 0) {
        for (std::size_t i = 1; i < pf.size(); ++i) {
          if (pf[i].revents == 0) continue;
          attempt* a = by_socket(pf[i].fd);
          if (!a) continue;
          int so = 0;
          int sl = sizeof so;
          const bool ok = !(pf[i].revents & (POLLERR | POLLHUP)) &&
                          ::getsockopt(a->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl) == 0 &&
                          so == 0;
          const SOCKET c = a->s;
          a->s = INVALID_SOCKET;
          a->retry_at = clock::now() + std::chrono::milliseconds(kRetryMs);
          if (ok) return std::unique_ptr<link>(std::make_unique<tcp_link>(c));
          ::closesocket(c);
        }
      }
      if (clock::now() >= until) return err(status::timeout);
    }
  }

 private:
  struct attempt {
    endpoint to;
    SOCKET s = INVALID_SOCKET;
    std::chrono::steady_clock::time_point started{}, retry_at{};
  };

  attempt* find(const endpoint& p) {
    for (auto& a : tries_) {
      if (a.to.address == p.address && a.to.port == p.port) return &a;
    }
    return nullptr;
  }
  attempt* by_socket(SOCKET s) {
    for (auto& a : tries_) {
      if (a.s == s) return &a;
    }
    return nullptr;
  }

  void start(attempt& a, std::chrono::steady_clock::time_point now) {
    a.retry_at = now + std::chrono::milliseconds(kRetryMs);
    sockaddr_storage to;
    int tlen = sockaddr_of(a.to, to);
    if (tlen == 0) return;
    if (dual_) tlen = mapped(to, tlen);
    if (to.ss_family != local_.ss_family) return;  // an IPv6 peer from an IPv4-only port
    const SOCKET c = ::WSASocketW(local_.ss_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (c == INVALID_SOCKET) return;
    BOOL one = TRUE;
    (void)::setsockopt(c, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    if (dual_) {
      DWORD v6only = 0;
      (void)::setsockopt(c, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    }
    u_long nb = 1;
    (void)::ioctlsocket(c, FIONBIO, &nb);
    if (::bind(c, reinterpret_cast<const sockaddr*>(&local_), len_) != 0) {
      ::closesocket(c);
      return;
    }
    if (::connect(c, reinterpret_cast<sockaddr*>(&to), tlen) != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
      ::closesocket(c);
      return;
    }
    a.s = c;
    a.started = now;
  }

  SOCKET s_;
  sockaddr_storage local_;
  int len_;
  bool dual_;
  std::uint16_t port_;
  std::vector<attempt> tries_;  // next()'s thread only
  std::atomic<bool> down_{false};
};

}  // namespace

result<std::unique_ptr<tcp_rendezvous>> open_rendezvous(bool loopback_only) {
  if (!winsock_ready()) return err(status::io);
  sockaddr_storage st{};
  int len = 0;
  bool dual = false;
  SOCKET s = loopback_only ? INVALID_SOCKET
                           : ::WSASocketW(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s != INVALID_SOCKET) {
    DWORD v6only = 0;
    (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    auto* a = reinterpret_cast<sockaddr_in6*>(&st);
    a->sin6_family = AF_INET6;  // :: (all zeros)
    len = static_cast<int>(sizeof *a);
    dual = true;
  } else {
    s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return err(status::io);
    auto* a = reinterpret_cast<sockaddr_in*>(&st);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ::htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    len = static_cast<int>(sizeof *a);
  }
  // Windows has no SO_REUSEPORT: SO_REUSEADDR on the listener and on every
  // connecting socket lets them share the port.
  BOOL one = TRUE;
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
  if (::bind(s, reinterpret_cast<sockaddr*>(&st), len) != 0 || ::listen(s, 8) != 0 ||
      ::getsockname(s, reinterpret_cast<sockaddr*>(&st), &len) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  return std::unique_ptr<tcp_rendezvous>(std::make_unique<tcp_meet>(s, st, len, dual));
}

result<std::unique_ptr<udp_socket>> open_udp_dual(std::uint16_t port, bool loopback_only) {
  (void)loopback_only;  // :: still, so 127.0.0.1 reaches it too; the tests send only to loopback
  if (!winsock_ready()) return err(status::io);
  const SOCKET s = ::WSASocketW(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  DWORD v6only = 0;
  (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
  int buf = 4 * 1024 * 1024;
  (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
  (void)::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
  BOOL no_reset = FALSE;
  DWORD unused = 0;
  (void)::WSAIoctl(s, SIO_UDP_CONNRESET, &no_reset, sizeof no_reset, nullptr, 0, &unused, nullptr, nullptr);
  sockaddr_in6 a{};
  a.sin6_family = AF_INET6;
  a.sin6_port = ::htons(port);
  int len = static_cast<int>(sizeof a);
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), len) != 0 || ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  return std::unique_ptr<udp_socket>(std::make_unique<udp_port>(s, ::ntohs(a.sin6_port), true));
}

std::vector<address> resolve(const std::string& name) {
  std::vector<address> out;
  if (!winsock_ready()) return out;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0) return out;
  for (addrinfo* r = res; r && out.size() < 8; r = r->ai_next) {
    if (r->ai_addr && (r->ai_family == AF_INET || r->ai_family == AF_INET6)) out.push_back(address_of(r->ai_addr));
  }
  ::freeaddrinfo(res);
  return out;
}

result<std::unique_ptr<udp_socket>> open_udp(bool v6, int multicast_ttl, bool loopback_only) {
  if (!winsock_ready()) return err(status::io);
  const SOCKET s = ::WSASocketW(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  DWORD ttl = static_cast<DWORD>(multicast_ttl);
  if (v6) {
    (void)::setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, reinterpret_cast<const char*>(&ttl), sizeof ttl);
  } else {
    (void)::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
  }
  // Windows reports an ICMP port-unreachable on the next receive; it is noise here.
  BOOL no_reset = FALSE;
  DWORD unused = 0;
  (void)::WSAIoctl(s, SIO_UDP_CONNRESET, &no_reset, sizeof no_reset, nullptr, 0, &unused, nullptr, nullptr);
  // An unbound UDP socket cannot receive until it sends; bind to the OS's pick.
  sockaddr_storage st{};
  int len = 0;
  if (v6) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&st);
    a->sin6_family = AF_INET6;
    if (loopback_only) a->sin6_addr.s6_addr[15] = 1;
    len = static_cast<int>(sizeof *a);
  } else {
    auto* a = reinterpret_cast<sockaddr_in*>(&st);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ::htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    len = static_cast<int>(sizeof *a);
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&st), len) != 0 ||
      ::getsockname(s, reinterpret_cast<sockaddr*>(&st), &len) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  return std::unique_ptr<udp_socket>(std::make_unique<udp_port>(s, port_of(st)));
}

result<std::unique_ptr<beacon_socket>> open_beacon_socket(std::uint16_t port) {
  if (!winsock_ready()) return err(status::io);
  const SOCKET s = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  BOOL one = TRUE;
  // Shared with other MediaViewer windows on this machine.
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
  (void)::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
  DWORD ttl = 1;
  (void)::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
  DWORD loop = 1;
  (void)::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof loop);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ::htonl(INADDR_ANY);
  a.sin_port = ::htons(port);
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::closesocket(s);
    return err(status::io);
  }
  ip_mreq m{};
  if (::inet_pton(AF_INET, kMulticastGroup, &m.imr_multiaddr) == 1) {
    m.imr_interface.s_addr = ::htonl(INADDR_ANY);
    (void)::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m);
  }
  return std::unique_ptr<beacon_socket>(std::make_unique<udp_beacon>(s, port));
}

namespace {
class handle_lock final : public instance_lock {
 public:
  explicit handle_lock(HANDLE h) : h_(h) {}
  ~handle_lock() override { ::CloseHandle(h_); }

 private:
  HANDLE h_;
};
}  // namespace

std::unique_ptr<instance_lock> try_lock_instance(const std::string& path) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return nullptr;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), n);
  // No sharing: a second process's open fails while this handle is open.
  HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  return std::make_unique<handle_lock>(h);
}

std::string downloads_folder() {
  PWSTR wide = nullptr;
  std::string out;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &wide)) && wide) {
    const int n = static_cast<int>(::wcslen(wide));
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide, n, nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
      out.assign(static_cast<std::size_t>(bytes), '\0');
      ::WideCharToMultiByte(CP_UTF8, 0, wide, n, out.data(), bytes, nullptr, nullptr);
    }
  }
  ::CoTaskMemFree(wide);
  return out;
}

std::string machine_name() {
  wchar_t name[256] = {};
  DWORD n = 256;
  if (!::GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n) || n == 0) return clean_name("");
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  std::string out(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
  if (bytes > 0) ::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
  return clean_name(out);
}

}  // namespace mv::sharepoint::net
