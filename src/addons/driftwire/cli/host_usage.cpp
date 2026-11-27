// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_usage.h"

#include <cstdio>
#include <ctime>

#include "core/json.h"

namespace mv::driftwire::cli {

namespace {

std::string hex_of(const usage_book::key& k) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (std::uint8_t b : k) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}

std::optional<usage_book::key> key_of(std::string_view hex) {
  if (hex.size() != 64) return std::nullopt;
  usage_book::key k{};
  for (std::size_t i = 0; i < 32; ++i) {
    int v = 0;
    for (int j = 0; j < 2; ++j) {
      const char c = hex[i * 2 + static_cast<std::size_t>(j)];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return std::nullopt;
      v = v * 16 + d;
    }
    k[i] = static_cast<std::uint8_t>(v);
  }
  return k;
}

}  // namespace

void usage_book::take(const std::vector<serve::server::device_usage>& now, const std::string& day_now,
                      std::chrono::steady_clock::time_point t) {
  if (day_now != day) {
    day = day_now;
    today.clear();
    total = {};
  }
  const double dt = last_t_ ? std::chrono::duration<double>(t - *last_t_).count() : 0.0;
  last_t_ = t;
  std::map<key, bytes> seen;
  rate.clear();
  bytes moved;
  for (const auto& u : now) {
    bytes cur{u.read, u.written};
    bytes prev{};
    if (auto it = last_.find(u.device); it != last_.end()) prev = it->second;
    // Below what was seen: a service started again since, counting from 0.
    const bytes d{cur.read >= prev.read ? cur.read - prev.read : cur.read,
                  cur.written >= prev.written ? cur.written - prev.written : cur.written};
    seen[u.device] = cur;
    if (d.read == 0 && d.written == 0) continue;
    auto& day_bytes = today[u.device];
    day_bytes.read += d.read;
    day_bytes.written += d.written;
    total.read += d.read;
    total.written += d.written;
    moved.read += d.read;
    moved.written += d.written;
    if (dt > 0.0) {
      rate[u.device] = {static_cast<std::uint64_t>(static_cast<double>(d.read) / dt),
                        static_cast<std::uint64_t>(static_cast<double>(d.written) / dt)};
    }
  }
  last_ = std::move(seen);
  total_rate = dt > 0.0 ? bytes{static_cast<std::uint64_t>(static_cast<double>(moved.read) / dt),
                                static_cast<std::uint64_t>(static_cast<double>(moved.written) / dt)}
                        : bytes{};
}

std::string usage_book::to_json() const {
  json::writer w;
  w.begin_object().key("day").string(day);
  w.key("read").integer(static_cast<std::int64_t>(total.read)).key("written").integer(static_cast<std::int64_t>(total.written));
  w.key("devices").begin_array();
  for (const auto& [k, b] : today) {
    w.begin_object().key("key").string(hex_of(k));
    w.key("read").integer(static_cast<std::int64_t>(b.read)).key("written").integer(static_cast<std::int64_t>(b.written));
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::optional<usage_book> usage_book::from_json(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const std::string* day = doc->str("day");
  if (!day) return std::nullopt;
  usage_book b;
  b.day = *day;
  const auto num = [](const json::value& v, const char* k) {
    const auto n = v.integer(k).value_or(0);
    return n > 0 ? static_cast<std::uint64_t>(n) : std::uint64_t{0};
  };
  b.total = {num(*doc, "read"), num(*doc, "written")};
  if (const json::value* d = doc->find("devices"); d && d->k == json::kind::array) {
    for (const auto& v : d->a) {
      const std::string* hex = v.k == json::kind::object ? v.str("key") : nullptr;
      const auto k = hex ? key_of(*hex) : std::nullopt;
      if (k && b.today.size() < 256) b.today[*k] = {num(v, "read"), num(v, "written")};
    }
  }
  return b;
}

std::string local_day(std::chrono::system_clock::time_point t) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#if defined(_WIN32)
  (void)localtime_s(&tm, &tt);
#else
  (void)localtime_r(&tt, &tm);
#endif
  // Room for any int in each field: GCC's -Wformat-truncation counts them as
  // up to 11 characters, though a real date needs 10 in all.
  char buf[40];
  (void)std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

}  // namespace mv::driftwire::cli
