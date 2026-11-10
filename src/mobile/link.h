// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's end of the NAS service (docs/plans/mobile.md §7): `remote_file`,
// the one port every byte from the NAS comes through, and its implementation
// over a Driftwire library channel. Worker threads only: every call blocks.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/serve_client.h"
#include "core/result.h"

namespace mv::mobile {

using key32 = std::array<std::uint8_t, 32>;

// Set from any thread; every blocking call here checks it.
struct cancel_flag {
  std::atomic<bool> stop{false};
  [[nodiscard]] bool stopped() const noexcept { return stop.load(std::memory_order_relaxed); }
};

// What a whole-file or range read hands over, in order. Returning false stops
// the read (status::cancelled).
using byte_sink = std::function<bool(std::uint64_t offset, std::span<const std::uint8_t> bytes)>;

// The port. Everything from the NAS goes through this, and tests can fake it.
class remote_file {
 public:
  virtual ~remote_file() = default;
  [[nodiscard]] virtual result<std::vector<driftwire::serve::root_info>> roots() = 0;
  [[nodiscard]] virtual result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                               const std::string& rel) = 0;
  [[nodiscard]] virtual result<driftwire::serve::stat_info> stat(const std::string& root, const std::string& rel) = 0;
  // Reads `length` bytes from `at` (or to the end of the file). status::not_found,
  // ::permission_denied (outside the named folders), ::io (the link went: the
  // caller asks again from where it got to), ::cancelled.
  [[nodiscard]] virtual expected read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                            std::uint64_t length, const byte_sink& sink,
                                            const cancel_flag* cancel = nullptr, bool bulk = false) = 0;
};

// "host:port"; a bracketed IPv6 literal is allowed. False when it is not one.
[[nodiscard]] bool split_host_port(std::string_view text, std::string& host, std::uint16_t& port);

// Who the phone is, and where the service is said to be.
struct service_config {
  std::string local;        // "host:port", may be empty
  std::string public_name;  // "name:port", may be empty
  key32 identity{};         // the service's pinned public key
  key32 access_key{};
  key32 device_key{};       // this phone's X25519 private key
  std::string device_id = "0000000000000000";
  std::string device_name = "iPhone";
};

enum class route_problem : std::uint8_t {
  unreachable,    // no connection, or no answer within the time
  wrong_service,  // completed a handshake with a key that is not the pinned one
  not_paired,
  too_old,
  busy,
  bad_code,       // the pairing code was wrong, spent or expired
};

struct route_failure {
  std::string address;
  route_problem problem = route_problem::unreachable;
};

struct route_report {
  std::string address;     // the one that answered
  bool over_internet = false;
  std::vector<route_failure> failures;  // the ones that did not
};

// Redeems the QR's single-use code on whichever route answers first, then
// proves it with a real connection. status::permission_denied: wrong code, or
// the wrong service; status::io: nothing answered.
[[nodiscard]] expected pair_device(const service_config& c, std::string_view code, route_report& report,
                                   const cancel_flag* cancel = nullptr, int timeout_ms = 5000);


class link_session final : public remote_file {
 public:
  explicit link_session(service_config c);
  ~link_session() override;

  // Keeps a live link (and says how it was reached); races the routes only
  // when there is none. Single-flight: concurrent callers share one race.
  [[nodiscard]] expected connect(route_report& report, const cancel_flag* cancel = nullptr, int timeout_ms = 5000);
  // The network changed: races again and swaps the winner in. Reads running on
  // the old client finish on it. On failure the old client stays.
  [[nodiscard]] expected reconnect(route_report& report, const cancel_flag* cancel = nullptr, int timeout_ms = 5000);
  // Cuts everything now (reads end with status::io).
  void disconnect() noexcept;
  // "Forget this library": disconnects and refuses every later connect.
  void shut_down() noexcept;
  [[nodiscard]] bool connected() const;

  [[nodiscard]] result<std::vector<driftwire::serve::root_info>> roots() override;
  [[nodiscard]] result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                       const std::string& rel) override;
  [[nodiscard]] result<driftwire::serve::stat_info> stat(const std::string& root, const std::string& rel) override;
  [[nodiscard]] expected read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                    std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel,
                                    bool bulk) override;

 private:
  // The live client, connecting first if there is none.
  [[nodiscard]] result<std::shared_ptr<driftwire::serve::client>> client(const cancel_flag* cancel);
  void dropped(const std::shared_ptr<driftwire::serve::client>& c) noexcept;
  [[nodiscard]] expected race_locked(route_report& report, const cancel_flag* cancel, int timeout_ms);  // connect_m_ held

  service_config config_;
  mutable std::mutex connect_m_;  // single-flight connect; held across the race, on a worker
  mutable std::mutex m_;          // guards live_
  std::shared_ptr<driftwire::serve::client> live_;
  route_report last_report_;  // under m_
  bool shut_ = false;         // under m_
};

}  // namespace mv::mobile
