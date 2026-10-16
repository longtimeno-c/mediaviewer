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
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

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
    DWORD ms = kSilenceMs;
    (void)::setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
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
    while (sent < bytes.size()) {
      if (down_.load()) return err(status::cancelled);
      const int want = static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 1u << 20));
      const int n = ::send(s_, reinterpret_cast<const char*>(bytes.data() + sent), want, 0);
      if (n == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        return err(e == WSAETIMEDOUT ? status::timeout : status::io);
      }
      sent += static_cast<std::size_t>(n);
    }
    return {};
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    if (into.empty()) return std::size_t{0};
    // In slices: closing the sending side does not wake a WSAPoll that waits
    // to read, so shutdown() is noticed here instead.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
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
    if (n == SOCKET_ERROR) return err(down_.load() ? status::cancelled : status::io);
    return static_cast<std::size_t>(n);
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

result<std::unique_ptr<link>> connect(const endpoint& to, int timeout_ms) {
  if (!winsock_ready()) return err(status::io);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = ::htons(to.port);
  if (to.port == 0 || ::inet_pton(AF_INET, to.address.c_str(), &a.sin_addr) != 1) return err(status::invalid_arg);
  const SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return err(status::io);
  u_long nb = 1;
  (void)::ioctlsocket(s, FIONBIO, &nb);
  int r = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
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
