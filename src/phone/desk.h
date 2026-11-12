// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop's half of phone access (docs/plans/mobile.md §7.2, §7.4, §7.5):
// claiming `driftwire serve`, its devices and named folders, single-use
// pairing codes, and the nearby service that hands a paired phone the newest
// catalogue. Part of the app since 2026-11-12 (docs/plans/phone-in-the-app.md):
// phone/link.h drives it through route_request, one JSON request at a time.
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

#include "wire/link.h"
#include "wire/grants.h"
#include "wire/owner_pass.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "wire/serve_wire.h"
#include "core/result.h"

namespace mv::driftwire::desk {

using key32 = std::array<std::uint8_t, 32>;

// In the store. forget() removes them with the rest of the add-on's secrets.
inline constexpr const char* kKeyName = "phone_desk_key";        // this desktop's X25519 key
inline constexpr const char* kServiceName = "phone_service";     // what claiming learned
inline constexpr const char* kDevicesName = "phone_devices";     // the phones this desktop has paired
inline constexpr const char* kPendingName = "phone_revoke_pending";  // revoked here, the NAS not told yet
inline constexpr const char* kGrantsName = "phone_grants";          // the timed phones, as the NAS last said
void forget(secrets::store& store);
// The items above, moved from `from` (Driftwire's store, where they were kept
// until 2026-11-12) to `to` where `to` has none: written, read back, then
// removed from `from`. One that will not move stays there.
void take_items(secrets::store& from, secrets::store& to);

inline constexpr int kConnectMs = 5000;
inline constexpr int kClaimTotalMs = 27000;  // under the host's 30 s per request
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
  // A timed or scoped phone (docs/plans/nas-owner-and-guests.md §5), as the
  // NAS last said; `limited` false for an unlimited one.
  bool limited = false;
  std::int64_t expires = 0;  // unix, the NAS's clock; 0: no end
  bool expired = false, guest = false, scoped = false;
};

// The NAS's owner side (§3): what it can do and where the owner stands.
struct owner_state {
  std::string passphrase;  // "set", "none", "broken"; "" from a service too old to say
  std::vector<std::string> features;
  std::int64_t unlocked_s = 0;    // left on this desktop's unlock; 0: locked
  std::int64_t locked_until = 0;  // unix: too many wrong answers until then
  std::string relay;              // the person's own relay, "host:port" ("" for none)
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
    // The owner's passphrase (§3.1): the cost of a new one, and the least a
    // challenge may ask (the tests use owner::kMinOps/kMinMem).
    std::uint64_t pass_ops = serve::owner::kOps, pass_mem = serve::owner::kMem;
    std::uint64_t floor_ops = serve::owner::kFloorOps, floor_mem = serve::owner::kFloorMem;
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
  //
  // The owner's passphrase (§3.3): with one, the claim takes the passphrase
  // step; `new_passphrase` chooses it for a NAS that has none (an existing one
  // is proven, never replaced). Without one, it claims as before, and a NAS
  // with a passphrase refuses it (last_refusal says "passphrase").
  [[nodiscard]] expected claim(const std::string& share_folder, const std::string& local,
                               const std::string& public_name, const std::string& passphrase = {},
                               bool new_passphrase = false);
  // The owner's unlock: Argon2id here, a signature there. The key it makes is
  // kept in memory for owner::kUnlockSeconds (call C2: never stored), and a
  // request the NAS refuses for want of an unlock is retried with it once.
  [[nodiscard]] expected unlock(const std::string& passphrase);
  // Forgets that key, and ends the NAS's unlock of this session.
  [[nodiscard]] expected lock();
  // Sets the passphrase (none yet: the device key is enough; set: unlocked).
  [[nodiscard]] expected set_passphrase(const std::string& next);
  [[nodiscard]] result<owner_state> owner_status();
  // The person's own relay (docs/plans/nas-relay.md): the service waits there
  // for its phones. "" clears it. The key goes to the service and nowhere else.
  [[nodiscard]] expected set_relay(const std::string& address, const std::string& key);
  // A device's limit from now: `for_s` seconds, or 0 for no limit at all.
  [[nodiscard]] expected grant(const key32& key, std::int64_t for_s);
  // The NAS's last refusal, {"why":...,"until":...} ("{}" when the last request went through).
  [[nodiscard]] std::string last_refusal();
  // Forgets it: each request through route_request starts with none, so a
  // failure that never reached the NAS never reads an older one's reason.
  void clear_refusal();
  // The last claim took the passphrase step (false from a NAS too old to: it
  // was claimed, and no passphrase protects it).
  [[nodiscard]] bool claim_took_passphrase();
  // A new public name for the QR ("" for none).
  [[nodiscard]] expected set_public_name(const std::string& public_name);
  // The service's address on its own network changed ("host:port").
  [[nodiscard]] expected set_local(const std::string& local);

  // A single-use code the service will take for `ttl_ms`.
  // `for_s` > 0: a guest code, whose phone gets that long from when it scans
  // (needs a NAS that has grants; status::unsupported_format from one too old).
  [[nodiscard]] result<pairing> start_pairing(int ttl_ms = kDefaultPairTtlMs, std::int64_t for_s = 0);
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

// What the phone link asks (phone/link.h): one method and one JSON object in,
// one JSON object out. status::permission_denied for any caller but "phone";
// status::not_found for a method it does not know.
[[nodiscard]] result<std::string> route_request(desk& d, std::string_view caller, std::string_view method,
                                                std::string_view request_json);

}  // namespace mv::driftwire::desk
