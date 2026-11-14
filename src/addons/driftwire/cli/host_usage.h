// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the host's phones read and wrote today (docs/plans/self-hosted.md
// "Tray": the menu, the This computer card and `host status` all show it, from
// state.json). The service counts file bytes per session (serve::server::usage,
// one relaxed atomic add per chunk); at each of the host's 2 s passes this book
// takes the service's totals, adds what is new to today's (per device and in
// all), and works out the rate since the last pass. A service started again
// (a new port, a pause) counts from 0: what it had is kept here. Today's totals
// are kept in <host>/usage.json (the device's public key in hex, never a name or
// an address there) so a restart or an update does not lose them; a new local
// day starts from 0. Pure: the tests give the day and the clock.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wire/serve.h"

namespace mv::driftwire::cli {

struct usage_book {
  using key = std::array<std::uint8_t, 32>;
  struct bytes {
    std::uint64_t read = 0, written = 0;
    bool operator==(const bytes&) const = default;
  };
  std::string day;               // "2026-11-14", the computer's own date
  std::map<key, bytes> today;    // per device
  bytes total;
  // Bytes a second over the last pass, per device and in all.
  std::map<key, bytes> rate;
  bytes total_rate;

  // One pass. `now`: the service's totals (empty with no service running: a
  // paused host; the next service counts from 0).
  void take(const std::vector<serve::server::device_usage>& now, const std::string& day_now,
            std::chrono::steady_clock::time_point t);
  // usage.json: the day and its totals (no rates).
  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] static std::optional<usage_book> from_json(std::string_view text);

 private:
  std::map<key, bytes> last_;  // the service's totals at the last pass
  std::optional<std::chrono::steady_clock::time_point> last_t_;
};

// The computer's local date, "YYYY-MM-DD".
[[nodiscard]] std::string local_day(std::chrono::system_clock::time_point t);

}  // namespace mv::driftwire::cli
