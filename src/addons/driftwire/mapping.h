// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Port mapping on the receiver's router (issue #472 PR 2, path 3): asks the
// router to forward a public port to this machine, so a code can carry an
// address a sender anywhere reaches. Three protocols, the first that answers
// wins: PCP (RFC 6887), NAT-PMP (RFC 6886), UPnP-IGD (WANIPConnection,
// WANPPPConnection; IGDv2's WANIPv6FirewallControl for an IPv6 pinhole).
// Only one side needs it. It cannot help behind double NAT or carrier NAT
// (the mapping is on the nearest NAT): a router whose outside address is not
// public says so in notes().
//
// Opt-in (the "Receive over the internet" toggle), only while a code waits
// or a transfer over it runs, and every mapping is removed when that ends.
// Leases are short and renewed while held (keeper, #499), so a crash leaves
// a forward or pinhole open for minutes, not an hour.
// It talks to the gateway only (#499): only the gateway's own SSDP answer is
// read, and its description, URLBase and control URLs must name the gateway
// itself (never loopback, 0/8, link-local or a public address); a service
// type is "urn:" and [A-Za-z0-9.:-] only, as it goes into a header. A port
// someone else forwards (the person's own 443) is not taken over. Worker
// threads only; nothing logged with an address. Portable (D9).
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/net.h"

namespace mv::driftwire::mapping {

enum class method : std::uint8_t { none, pcp, nat_pmp, upnp };
[[nodiscard]] const char* method_name(method m) noexcept;

struct mapped {
  method how = method::none;
  net::endpoint external;  // what a sender connects to
  std::uint16_t internal_port = 0;
  bool pinhole = false;    // an IPv6 pinhole, not a NAT mapping
  // To remove it.
  std::array<std::uint8_t, 12> pcp_nonce{};
  std::string upnp_control;  // "http://192.168.1.1:5000/ctl/IPConn"
  std::string upnp_service;  // its service type
  std::string pinhole_id;
};

struct options {
  std::optional<net::address> gateway;  // none: net::default_gateway()
  std::uint16_t gateway_port = 5351;    // PCP and NAT-PMP
  net::endpoint ssdp{"239.255.255.250", 1900};
  int timeout_ms = 2500;                // each protocol
  std::function<bool()> cancelled;      // may be empty
};

class mapper {
 public:
  explicit mapper(options o);

  // TCP `internal_port` mapped, at `external_port` when the router allows
  // (0: any), for `lifetime_s`. nullopt when no protocol answered with a
  // public address; notes() says what each did.
  [[nodiscard]] std::optional<mapped> map_tcp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s);
  // An IPv6 pinhole for `local` (one of this machine's public IPv6
  // addresses), UPnP IGDv2 only.
  [[nodiscard]] std::optional<mapped> pinhole_v6(const net::address& local, std::uint16_t port, int lifetime_s);
  // The lease asked again for `lifetime_s` (the same ports); false when the
  // router did not take it.
  [[nodiscard]] bool renew(const mapped& m, int lifetime_s);
  // Best effort: the router is asked to drop it.
  void remove(const mapped& m);

  // What each protocol answered, plain ASCII without addresses (the window's
  // "why", and the log).
  [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
  // A router answered but its outside address was not public (double NAT,
  // carrier NAT).
  [[nodiscard]] bool behind_another_nat() const noexcept { return behind_nat_; }

 private:
  struct upnp_device {
    std::string control, service;          // the WAN connection service
    std::string fw_control, fw_service;    // IGDv2's IPv6 firewall, if any
  };
  [[nodiscard]] std::optional<mapped> try_pcp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s);
  [[nodiscard]] std::optional<mapped> try_nat_pmp(std::uint16_t internal_port, std::uint16_t external_port,
                                                  int lifetime_s);
  [[nodiscard]] std::optional<mapped> try_upnp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s);
  [[nodiscard]] std::optional<upnp_device> find_upnp();
  [[nodiscard]] bool cancelled() const { return opts_.cancelled && opts_.cancelled(); }

  options opts_;
  std::optional<net::address> gateway_;
  std::optional<net::address> me_;  // this machine toward the gateway
  std::optional<upnp_device> upnp_;
  bool upnp_searched_ = false;
  bool behind_nat_ = false;
  std::vector<std::string> notes_;
};

// What a code or a way in holds (#499): each mapping renewed at half its
// lease on a thread of its own, and every one removed when this goes.
class keeper {
 public:
  keeper(std::unique_ptr<mapper> m, std::vector<mapped> held, int lifetime_s);
  ~keeper();
  keeper(const keeper&) = delete;
  keeper& operator=(const keeper&) = delete;

 private:
  void run();
  std::unique_ptr<mapper> mapper_;
  std::vector<mapped> held_;
  int lifetime_s_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;  // under m_
  std::thread thread_;
};

// The pieces, exposed for the tests and the fuzzer: parse a gateway's reply
// (nullopt for anything malformed or unsuccessful).
struct pcp_answer {
  std::uint8_t result = 0;
  std::uint16_t external_port = 0;
  net::address external;
  std::uint32_t lifetime = 0;
};
[[nodiscard]] std::optional<pcp_answer> parse_pcp_map(std::span<const std::uint8_t> reply,
                                                      const std::array<std::uint8_t, 12>& nonce);
struct nat_pmp_answer {
  std::uint16_t result = 0;
  std::uint16_t external_port = 0;
  std::uint32_t lifetime = 0;
};
[[nodiscard]] std::optional<nat_pmp_answer> parse_nat_pmp_map(std::span<const std::uint8_t> reply);
[[nodiscard]] std::optional<net::address> parse_nat_pmp_address(std::span<const std::uint8_t> reply);
// SSDP: the LOCATION of an answer, if it is an http URL at `gateway`.
[[nodiscard]] std::optional<std::string> parse_ssdp_location(std::string_view reply, const net::address& gateway);
// A device description: the WAN connection's (service type, control URL),
// resolved against `location`; and IGDv2's IPv6 firewall service if listed.
// Every URL must be at `gateway`, every service type service_type().
struct described {
  std::string service, control, fw_service, fw_control;
};
[[nodiscard]] std::optional<described> parse_description(std::string_view xml, std::string_view location,
                                                         const net::address& gateway);
// "urn:" then [A-Za-z0-9.:-] only, at most 256 bytes.
[[nodiscard]] bool service_type(std::string_view s);
// An HTTP response: its status and body (chunked decoded); nullopt if malformed.
struct http_reply {
  int status = 0;
  std::string body;
};
[[nodiscard]] std::optional<http_reply> parse_http(std::string_view raw);
// The text of the first <tag>...</tag> (any namespace prefix) in `xml`.
[[nodiscard]] std::optional<std::string> xml_value(std::string_view xml, std::string_view tag);

}  // namespace mv::driftwire::mapping
