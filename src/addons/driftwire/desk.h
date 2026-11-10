// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop's half of phone access (docs/plans/mobile.md §7.2, §7.4, §7.5):
// claiming `driftwire serve`, its devices and named folders, single-use
// pairing codes, and the nearby service that hands a paired phone the newest
// catalogue. Local search reaches all of this through the host table
// (route_request, docs/design/18 "Add-ons calling add-ons").
//
// Worker threads only: every call may wait on the network or the OS's secret
// store. No address, path or file name is logged. Portable (D9): only the
// Bonjour advert differs per OS (advert.h).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/link.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/secrets.h"
#include "addons/driftwire/serve.h"
#include "addons/driftwire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::desk {

using key32 = std::array<std::uint8_t, 32>;

// In the store. forget() removes them with the rest of the add-on's secrets.
inline constexpr const char* kKeyName = "phone_desk_key";        // this desktop's X25519 key
inline constexpr const char* kServiceName = "phone_service";     // what claiming learned
inline constexpr const char* kDevicesName = "phone_devices";     // the phones this desktop has paired
inline constexpr const char* kPendingName = "phone_revoke_pending";  // revoked here, the NAS not told yet
void forget(secrets::store& store);

inline constexpr int kConnectMs = 5000;
inline constexpr int kDefaultPairTtlMs = 300000;

// "host:port", "[v6]:port". nullopt: not that shape, or port 0.
[[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text);

// §7.3: what the addresses a public name resolves to mean. `none` when at
// least one could be reached from outside.
enum class address_warning : std::uint8_t { none, private_range, carrier_nat, unresolved };
[[nodiscard]] address_warning classify_addresses(std::span<const net::address> addresses) noexcept;
[[nodiscard]] const char* warning_name(address_warning w) noexcept;

struct service_info {
  std::string local;        // "host:port", on the NAS's own network
  std::string public_name;  // "name:port" from outside, or ""
  key32 identity{};
  key32 access_key{};
};

struct pairing {
  service_info service;
  std::string code;
  std::int64_t expires_unix = 0;
};

struct device_row {
  std::string name;
  key32 key{};
};

struct root_row {
  std::string id, label, path;
};

class desk {
 public:
  struct options {
    secrets::store* store = nullptr;  // borrowed; outlives this
    std::string device_name = "MediaViewer";
    // A connection to a host and port. Empty: net::resolve and net::connect.
    std::function<result<std::unique_ptr<link>>(const std::string& host, std::uint16_t port, int timeout_ms)> connect;
    // The nearby service, the tests: who counts as on this network.
    std::function<bool(const net::address&)> own_network;
    bool nearby_loopback = false;  // the tests: the nearby service on loopback only
    bool advertise = true;         // false: no Bonjour (the tests)
  };
  explicit desk(options o);
  ~desk();
  desk(const desk&) = delete;
  desk& operator=(const desk&) = delete;

  [[nodiscard]] std::optional<service_info> service();

  // §7.2: writes the one-time token into `share_folder`/.mediaviewer, claims
  // the service at `local`, and takes the token away again whatever happened.
  // status::permission_denied: the service said no, or stayed silent because
  // it already has an owner or this is not its own network (it never says
  // which). status::io: the share cannot be written, or nothing answered.
  [[nodiscard]] expected claim(const std::string& share_folder, const std::string& local,
                               const std::string& public_name);
  // A new public name for the QR ("" for none).
  [[nodiscard]] expected set_public_name(const std::string& public_name);
  // The service's address on its own network changed ("host:port").
  [[nodiscard]] expected set_local(const std::string& local);

  // A single-use code the service will take for `ttl_ms`.
  [[nodiscard]] result<pairing> start_pairing(int ttl_ms = kDefaultPairTtlMs);
  // Withdraws the code waiting on the service (Cancel, or it ran out here).
  [[nodiscard]] expected stop_pairing();
  // Whether the last code is still waiting on the service: false once a phone
  // used it (a re-scan adds nothing to the list) or it ran out. As last read
  // by refresh_devices.
  [[nodiscard]] bool pairing_waiting();
  // Revokes made here that the service has not heard yet.
  [[nodiscard]] std::size_t revokes_pending();

  // The phones here, as last read.
  [[nodiscard]] std::vector<device_row> devices();
  // Lets go of the session kept open while a code was showing.
  void release();
  // Tells the service about any revoke it missed, then takes its list as this
  // desktop's. status::io and the list is left as it was when it is out of reach.
  [[nodiscard]] result<std::vector<device_row>> refresh_devices();
  [[nodiscard]] expected add_device(const std::string& name, const key32& key);
  // From this desktop at once (the nearby service cuts it), then the service.
  // status::io: the service could not be told; it is told at the next refresh.
  [[nodiscard]] expected revoke(const key32& key);

  [[nodiscard]] result<std::vector<root_row>> roots();
  [[nodiscard]] result<std::string> add_root(const std::string& label, const std::string& path);
  [[nodiscard]] expected remove_root(const std::string& id);

  // The nearby service over `folder` (the Catalogue root) for the paired phones,
  // advertised as _mediaviewer._tcp. Again with the same arguments: nothing.
  [[nodiscard]] expected start_nearby(const std::string& folder, const std::string& library_id);
  void stop_nearby();
  [[nodiscard]] std::uint16_t nearby_port() const;
  [[nodiscard]] std::optional<serve::counters> nearby_stats() const;
  // The advert's text, `lib=` value: the first 16 hex digits of the id's SHA-256.
  [[nodiscard]] static std::string library_tag(std::string_view library_id);

 private:
  struct state;
  std::unique_ptr<state> st_;
};

// What Local search asks (docs/design/18 "Add-ons calling add-ons"): one method
// and one JSON object in, one JSON object out. status::permission_denied for
// any caller but Local search; status::not_found for a method it does not know.
[[nodiscard]] result<std::string> route_request(desk& d, std::string_view caller, std::string_view method,
                                                std::string_view request_json);

}  // namespace mv::driftwire::desk
