// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire VPN (slice 5; docs/plans/driftwire-menu-and-tunnel.md, docs/design/28
// "The Tunnel"): every app's traffic, not only those that follow the proxy.
// A virtual interface catches the machine's packets; netstack.h turns them
// back into connections; each TCP connection is one forward at the relay
// server and each UDP flow one datagram flow (splice.h kForwardUdp), through
// the same dialer as the proxy (tunnel.h). The relay never routes raw IP.
//
// The interface is the OS's, behind `os_port` (D9), one file per OS:
//   vpn_mac.mm   a Network Extension packet tunnel (NETunnelProviderManager
//                here; the provider itself, a thin packet pump in the app's
//                bundle, is src/addons/driftwire/mac_tunnel). It needs Apple's
//                Network Extension entitlement on the signing team: owed.
//   vpn_win.cpp  Wintun (wintun.dll beside the add-on, loaded at run time;
//                shipping it waits for the owner's licence call), routes and
//                DNS through IP Helper.
//   vpn_none.cpp Linux (until its host reaches Driftwire) and the tests.
// What the OS side sets (settings): an address and a DNS server inside the
// tunnel, the default route through it for IPv4 and IPv6 both (plan_routes),
// the relay's own addresses and the local networks left out (the tunnel
// never carries itself, and the printer still works), and with the kill switch, nothing but the tunnel while it is
// down (Block when the tunnel is down, off by default).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/netstack.h"
#include "addons/driftwire/tunnel.h"

namespace mv::driftwire::vpn {

// The packets of the OS's virtual interface.
class device {
 public:
  virtual ~device() = default;
  // One whole packet; status::timeout when none came; status::io once the
  // interface is gone.
  [[nodiscard]] virtual result<std::size_t> read(std::span<std::uint8_t> into, int timeout_ms) = 0;
  [[nodiscard]] virtual expected write(std::span<const std::uint8_t> packet) = 0;
  virtual void shutdown() noexcept = 0;
};

// A device whose packets go over a link, each as a 2-byte length (big
// endian) and itself: how the Mac's packet tunnel provider, a process of its
// own, hands packets to the add-on (over loopback, after a token), and the
// tests' stand-in. Thread-safe: one reader, any writers.
[[nodiscard]] std::unique_ptr<device> framed_device(std::unique_ptr<link> l);

// What the OS side sets up.
struct settings {
  std::string address4 = "198.18.0.1";  // the interface's (RFC 2544 benchmarking: never a real network)
  int prefix4 = 30;
  std::string dns4 = "198.18.0.2";      // answered through the relay's resolver (kDnsHost)
  // IPv6 too, or an app's IPv6 would go around the tunnel: an address
  // in a unique local /64 of Driftwire's own (RFC 4193; never routed on the
  // internet). A unique local source makes the OS try a site's IPv4 first
  // (RFC 6724 rule 5), so IPv6 goes through the relay mostly where a site
  // has nothing else, and the relay connects out over IPv6 (splice.h).
  std::string address6 = "fd5d:7269:6674::1";
  int prefix6 = 64;
  std::uint16_t mtu = static_cast<std::uint16_t>(kMtu);
  std::vector<net::address> exclude;     // the relay server's own addresses, IPv4 and IPv6
  std::vector<net::subnet> exclude_nets;  // the local networks, IPv4 and IPv6
  bool kill_switch = false;
  std::string relay_name;                // shown by the OS ("Driftwire through <relay>")
};

// The routes both OS halves set, from `settings` (pure; tested): every
// address of both families through the tunnel, and around it the relay's own
// addresses (/32, /128) and the local networks (each its network address).
// Left out of the exclusions: what would take everything (a prefix of 0), an
// IPv6 network wider than a /48 (kMinLocalPrefix6),
// what is not an address of its family, duplicates, and a network holding
// the tunnel's own address (a stale interface of ours). An exclusion is by
// the route it had before; the OS keeps a more specific one first.
inline constexpr int kMinLocalPrefix6 = 48;  // an IPv6 network wider is never left out
struct route_plan {
  std::vector<net::subnet> include4, include6;
  std::vector<net::subnet> exclude4, exclude6;
};
enum class route_style {
  default_route,  // 0.0.0.0/0 and ::/0 (Network Extension: NEIPv4Route.default())
  halves,         // 0.0.0.0/1 + 128.0.0.0/1, ::/1 + 8000::/1: beside the default route, never replacing it (Windows)
};
[[nodiscard]] route_plan plan_routes(const settings& s, route_style style);

class os_port {
 public:
  virtual ~os_port() = default;
  // "" when the interface can start; else why not, in the window's words
  // (the extension not approved, no entitlement in this build, no driver).
  [[nodiscard]] virtual std::string unavailable() = 0;
  // Starts the interface with `s` (may ask the person once: approve the
  // extension, an administrator); a worker's call, never the UI's.
  [[nodiscard]] virtual result<std::unique_ptr<device>> start(const settings& s) = 0;
  // Takes the interface, its routes and DNS away (the device then reads io).
  virtual void stop() = 0;
  // "" when Block when the tunnel is down is kept here; else why not (the
  // switch is then not offered: a kill switch that does not hold is worse
  // than none).
  [[nodiscard]] virtual std::string block_unavailable() { return {}; }
};
// This OS's (vpn_<os>); none on Linux. The add-on hands it to the engine.
[[nodiscard]] std::unique_ptr<os_port> make_os_port();

// The VPN while it runs: the device's packets through the stack to forwards.
class runner {
 public:
  struct options {
    tunnel::dialer::options dial;
    std::unique_ptr<device> dev;
    settings s;
    std::function<void()> changed;  // streams opened or ended; any thread, never under the runner's lock
  };
  struct snapshot {
    std::uint32_t tcp_open = 0;
    std::uint64_t tcp_made = 0, udp_flows = 0, failed = 0;
    std::string problem;  // the relay's (the key, forwarding off, no answer)
  };
  [[nodiscard]] static result<std::unique_ptr<runner>> start(options o);
  ~runner();  // every stream reset, every thread joined; the device shut down
  [[nodiscard]] snapshot state() const;

 private:
  struct udp_flow;
  explicit runner(options o);
  void pump();
  void ticker();
  void on_tcp(std::unique_ptr<link> l, const flow& f);
  void on_udp(const flow& f, std::span<const std::uint8_t> payload);
  void serve_udp(std::shared_ptr<udp_flow> u);
  void note(relay::forward_answer a, bool opened);
  void reap_locked();

  options opts_;
  std::unique_ptr<tunnel::dialer> dial_;
  std::unique_ptr<stack> stack_;
  std::atomic<bool> stop_{false};
  std::thread pump_thread_, tick_thread_;
  mutable std::mutex m_;
  std::condition_variable tick_cv_;
  struct worker {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<tunnel::remote_slot> remote;
    std::shared_ptr<link> local;
  };
  std::vector<worker> workers_;                           // under m_
  std::map<flow, std::shared_ptr<udp_flow>> udp_;          // under m_
  snapshot snap_;                                         // under m_
};

}  // namespace mv::driftwire::vpn
