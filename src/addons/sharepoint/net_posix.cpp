// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net.h over BSD sockets (macOS, and the portable build).
#include "addons/sharepoint/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
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

result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(to.port);
  if (to.port == 0 || ::inet_pton(AF_INET, to.address.c_str(), &a.sin_addr) != 1) return err(status::invalid_arg);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return err(status::io);
  close_on_exec(fd);
  no_sigpipe(fd);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
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
