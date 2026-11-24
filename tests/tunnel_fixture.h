// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A laptop's Tunnel reaching a "site" through a VPN host's relay, on loopback:
// the site (an echo that upper-cases) and an HTTP CONNECT through the Tunnel's
// proxy. Shared by the host's tests and the NAS's (docs/design/28).
#pragma once

#include "catch_compat.h"

#include <atomic>
#include <cctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire/link.h"
#include "wire/net.h"

// An echo server on loopback (upper-cases what it hears), the far end of a forward.
struct echo_server {
  std::unique_ptr<mv::driftwire::net::listener> l;
  std::thread t;
  std::atomic<bool> stop{false};
  std::vector<std::thread> conns;
  echo_server() {
    auto x = mv::driftwire::net::listen(true);
    REQUIRE(x);
    l = std::move(*x);
    t = std::thread([this] {
      while (!stop.load()) {
        auto c = l->accept(200);
        if (!c) {
          if (c.error() == mv::status::cancelled) return;
          continue;
        }
        std::shared_ptr<mv::driftwire::link> one(std::move(*c));
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

// An HTTP CONNECT through the laptop's Tunnel proxy, and bytes both ways:
// the proxy's status line ("" when nothing answered), echo checked when 200.
inline std::string connect_through(std::uint16_t proxy_port, const std::string& authority, bool* echoed = nullptr) {
  if (echoed) *echoed = false;
  auto c = mv::driftwire::net::connect({"127.0.0.1", proxy_port}, 2000);
  if (!c) return {};
  auto& l = **c;
  const std::string ask = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n";
  if (!l.write_all({reinterpret_cast<const std::uint8_t*>(ask.data()), ask.size()})) return {};
  std::string head;
  std::uint8_t b = 0;
  while (head.find("\r\n\r\n") == std::string::npos && head.size() < 4096) {
    if (!mv::driftwire::read_exact(l, {&b, 1}, 15000)) return head.substr(0, head.find("\r\n"));
    head.push_back(static_cast<char>(b));
  }
  const std::string status = head.substr(0, head.find("\r\n"));
  if (status.rfind("HTTP/1.1 200", 0) == 0 && echoed) {
    const std::string hello = "through the home computer";
    if (l.write_all({reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size()})) {
      std::vector<std::uint8_t> back(hello.size());
      if (mv::driftwire::read_exact(l, back, 10000)) *echoed = std::string(back.begin(), back.end()) == "THROUGH THE HOME COMPUTER";
    }
  }
  return status;
}
