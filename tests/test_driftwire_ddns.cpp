// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The DuckDNS keeper (addons/driftwire/ddns.h, docs/plans/self-hosted.md §4):
// what the person types, DuckDNS's answers, the schedule, and the keeper's
// thread against a stand-in DuckDNS on loopback. Never the real duckdns.org:
// the provider's base is the stand-in's http://127.0.0.1:N, fetched by a
// plain HTTP GET here (the host's real fetch, cli::https_get, takes https
// only).
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/ddns.h"
#include "addons/driftwire/mapping.h"
#include "import_fixture.h"
#include "wire/net.h"
#include "wire/secrets.h"

namespace ddns = mv::driftwire::ddns;
namespace net = mv::driftwire::net;
namespace secrets = mv::driftwire::secrets;
using namespace std::chrono_literals;

namespace {

constexpr const char* kToken = "0a1b2c3d-4e5f-6a7b-8c9d-0e1f2a3b4c5d";
constexpr const char* kSub = "myhome-42";

bool has(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

net::address addr(const char* text) { return *net::parse_address(text); }

// A stand-in DuckDNS on 127.0.0.1: each request's path is kept, and answered
// by the next scripted reply (the last one again once the script runs out).
struct reply {
  int status = 200;
  std::string body = "OK";
  int hang_ms = 0;  // wait this long before answering (a timeout)
};

class stand_in {
 public:
  explicit stand_in(std::vector<reply> script) : script_(std::move(script)) {
    auto l = net::listen(true);
    REQUIRE(l.has_value());
    listener_ = std::move(*l);
    thread_ = std::thread([this] { serve(); });
  }
  ~stand_in() {
    stop_ = true;
    listener_->shutdown();
    thread_.join();
  }
  [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(listener_->port()); }
  [[nodiscard]] std::vector<std::string> paths() {
    std::lock_guard lock(m_);
    return paths_;
  }
  [[nodiscard]] std::size_t count() {
    std::lock_guard lock(m_);
    return paths_.size();
  }
  // Waits until `n` requests have come, or `within` passes.
  bool wait_for(std::size_t n, std::chrono::milliseconds within) {
    std::unique_lock lock(m_);
    return cv_.wait_for(lock, within, [&] { return paths_.size() >= n; });
  }
  void set_script(std::vector<reply> script) {
    std::lock_guard lock(m_);
    script_ = std::move(script);
    next_ = 0;
  }

 private:
  void serve() {
    while (!stop_) {
      auto c = listener_->accept(50);
      if (!c) continue;
      auto link = std::move(*c);
      std::string req;
      std::uint8_t buf[2048];
      while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        auto n = link->read_some(buf, 2000);
        if (!n || *n == 0) break;
        req.append(reinterpret_cast<const char*>(buf), *n);
      }
      const std::size_t sp1 = req.find(' ');
      const std::size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
      reply r;
      {
        std::lock_guard lock(m_);
        paths_.push_back(sp2 == std::string::npos ? std::string() : req.substr(sp1 + 1, sp2 - sp1 - 1));
        if (!script_.empty()) r = script_[std::min(next_++, script_.size() - 1)];
      }
      cv_.notify_all();
      for (int waited = 0; waited < r.hang_ms && !stop_; waited += 10) std::this_thread::sleep_for(10ms);
      std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Length: " +
                        std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
      (void)link->write_all(std::span(reinterpret_cast<const std::uint8_t*>(out.data()), out.size()));
      link->shutdown();
    }
  }

  std::unique_ptr<net::listener> listener_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<reply> script_;
  std::size_t next_ = 0;
  std::vector<std::string> paths_;
};

// A plain HTTP GET of http://127.0.0.1:N/..., within `timeout_ms`: the
// stand-in for cli::https_get.
ddns::fetch_answer loopback_get(const std::string& url, int timeout_ms) {
  const std::string prefix = "http://127.0.0.1:";
  if (url.rfind(prefix, 0) != 0) return {};
  const std::size_t slash = url.find('/', prefix.size());
  const auto port = static_cast<std::uint16_t>(std::stoi(url.substr(prefix.size(), slash - prefix.size())));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  auto c = net::connect({"127.0.0.1", port}, timeout_ms);
  if (!c) return {};
  auto link = std::move(*c);
  const std::string req = "GET " + url.substr(slash) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (!link->write_all(std::span(reinterpret_cast<const std::uint8_t*>(req.data()), req.size()))) return {};
  std::string raw;
  std::uint8_t buf[2048];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return {};
    auto n = link->read_some(buf, static_cast<int>(left.count()));
    if (!n) return {};
    if (*n == 0) break;
    raw.append(reinterpret_cast<const char*>(buf), *n);
  }
  const auto parsed = mv::driftwire::mapping::parse_http(raw);
  if (!parsed) return {};
  return {parsed->status, parsed->body};
}

ddns::request req(std::optional<net::address> v4 = std::nullopt, std::optional<net::address> v6 = std::nullopt) {
  return {.subdomain = kSub, .token = kToken, .ipv4 = v4, .ipv6 = v6};
}

// Every log line, kept.
struct log_lines {
  std::mutex m;
  std::vector<std::string> lines;
  std::function<void(std::string_view)> sink() {
    return [this](std::string_view l) {
      std::lock_guard lock(m);
      lines.emplace_back(l);
    };
  }
  std::vector<std::string> all() {
    std::lock_guard lock(m);
    return lines;
  }
};

ddns::timing fast() {
  return {.every = 800ms, .min_gap = 60ms, .backoff_first = 120ms, .backoff_max = 480ms};
}

}  // namespace

TEST_CASE("ddns: the subdomain is one DuckDNS label, however it was typed", "[driftwire][ddns]") {
  CHECK(ddns::parse_subdomain("home") == "home");
  CHECK(ddns::parse_subdomain("  Home-42 \n") == "home-42");
  CHECK(ddns::parse_subdomain("home.duckdns.org") == "home");
  CHECK(ddns::parse_subdomain("HOME.DuckDNS.org.") == "home");
  CHECK(ddns::duckdns_name("home") == "home.duckdns.org");

  CHECK_FALSE(ddns::parse_subdomain(""));
  CHECK_FALSE(ddns::parse_subdomain("duckdns.org"));
  CHECK_FALSE(ddns::parse_subdomain(".duckdns.org"));
  CHECK_FALSE(ddns::parse_subdomain("photos.home.duckdns.org"));
  CHECK_FALSE(ddns::parse_subdomain("home.example.com"));
  CHECK_FALSE(ddns::parse_subdomain("home.duckdns.org.evil"));
  CHECK_FALSE(ddns::parse_subdomain("home.duckdns.org.duckdns.org"));
  CHECK_FALSE(ddns::parse_subdomain("https://home.duckdns.org"));
  CHECK_FALSE(ddns::parse_subdomain("my home"));
  CHECK_FALSE(ddns::parse_subdomain("home&token=x"));
  CHECK_FALSE(ddns::parse_subdomain("-home"));
  CHECK_FALSE(ddns::parse_subdomain("home-"));
  CHECK_FALSE(ddns::parse_subdomain("h\xc3\xa9me"));
  CHECK_FALSE(ddns::parse_subdomain(std::string(64, 'a')));
  CHECK(ddns::parse_subdomain(std::string(63, 'a')));
}

TEST_CASE("ddns: the token is a UUID and nothing else", "[driftwire][ddns]") {
  CHECK(ddns::parse_token(kToken) == kToken);
  CHECK(ddns::parse_token(" 0A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C5D\r\n") == kToken);
  CHECK_FALSE(ddns::parse_token(""));
  CHECK_FALSE(ddns::parse_token("0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d"));
  CHECK_FALSE(ddns::parse_token("0a1b2c3d-4e5f-6a7b-8c9d-0e1f2a3b4c5g"));
  CHECK_FALSE(ddns::parse_token("0a1b2c3d-4e5f-6a7b-8c9d-0e1f2a3b4c5d&x"));
  CHECK_FALSE(ddns::parse_token("0a1b2c3d-4e5f-6a7b-8c9d&0e1f2a3b4c5d"));
  CHECK_FALSE(ddns::parse_token("https://www.duckdns.org/update?token=0a1b2c3d-4e5f-6a7b-8c9d-0e1f2a3b4c5d"));
}

TEST_CASE("ddns: the token is kept in the secret store", "[driftwire][ddns]") {
  mv::test::scratch_dir dir("ddns_secrets");
  const std::string folder = mv::test::utf8(dir / "secrets");
  REQUIRE(secrets::owner_only_folder(folder));
  auto store = secrets::file_store(folder);
  REQUIRE(store);
  CHECK_FALSE(ddns::load_token(*store));
  CHECK_FALSE(ddns::save_token(*store, "not a token"));
  CHECK_FALSE(store->exists(ddns::kTokenSecret));
  REQUIRE(ddns::save_token(*store, " 0A1B2C3D-4E5F-6A7B-8C9D-0E1F2A3B4C5D "));
  CHECK(ddns::load_token(*store) == kToken);
  CHECK(ddns::forget_token(*store));
  CHECK_FALSE(ddns::load_token(*store));
}

TEST_CASE("ddns: DuckDNS's answers", "[driftwire][ddns]") {
  auto a = ddns::parse_duckdns("OK\n198.51.100.7\n2a01:4f8::7\nUPDATED");
  CHECK(a.what == ddns::outcome::ok);
  CHECK(a.ipv4 == addr("198.51.100.7"));
  CHECK(a.ipv6 == addr("2a01:4f8::7"));
  CHECK(a.changed);

  a = ddns::parse_duckdns("OK\r\n198.51.100.7\r\n\r\nNOCHANGE\r\n");
  CHECK(a.what == ddns::outcome::ok);
  CHECK(a.ipv4 == addr("198.51.100.7"));
  CHECK_FALSE(a.ipv6);
  CHECK_FALSE(a.changed);

  CHECK(ddns::parse_duckdns("OK").what == ddns::outcome::ok);
  CHECK(ddns::parse_duckdns("KO").what == ddns::outcome::refused);
  CHECK(ddns::parse_duckdns("KO\n").what == ddns::outcome::refused);
  CHECK(ddns::parse_duckdns("").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("<html>Sign in to the hotel wifi</html>").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("OK\nnot-an-address\n\nUPDATED").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("OK\n2a01:4f8::7\n\nUPDATED").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("OK\n198.51.100.7\n198.51.100.8\nUPDATED").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("OK\n198.51.100.7\n\nMAYBE").what == ddns::outcome::bad_answer);
  CHECK(ddns::parse_duckdns("OK\n1.2.3.4\n\nUPDATED\nmore").what == ddns::outcome::bad_answer);
}

TEST_CASE("ddns: the update URL", "[driftwire][ddns]") {
  CHECK(ddns::duckdns_url("https://www.duckdns.org", req()) ==
        std::string("https://www.duckdns.org/update?domains=myhome-42&token=") + kToken + "&ip=&ipv6=&verbose=true");
  CHECK(ddns::duckdns_url("B", req(addr("198.51.100.7"), addr("2a01:4f8::7"))) ==
        std::string("B/update?domains=myhome-42&token=") + kToken + "&ip=198.51.100.7&ipv6=2a01:4f8::7&verbose=true");
  CHECK(ddns::duckdns_url("B", req(), true) ==
        std::string("B/update?domains=myhome-42&token=") + kToken + "&clear=true&verbose=true");
}

TEST_CASE("ddns: one update against a stand-in DuckDNS: OK, KO, an error, a timeout", "[driftwire][ddns]") {
  SECTION("OK") {
    stand_in s({{.body = "OK\n198.51.100.7\n\nUPDATED"}});
    ddns::duckdns d(loopback_get, s.base(), 2000);
    const auto a = d.update(req());
    CHECK(a.what == ddns::outcome::ok);
    CHECK(a.ipv4 == addr("198.51.100.7"));
    CHECK(a.changed);
    const auto p = s.paths();
    REQUIRE(p.size() == 1);
    CHECK(p[0] == std::string("/update?domains=myhome-42&token=") + kToken + "&ip=&ipv6=&verbose=true");
  }
  SECTION("KO") {
    stand_in s({{.body = "KO"}});
    ddns::duckdns d(loopback_get, s.base(), 2000);
    CHECK(d.update(req()).what == ddns::outcome::refused);
  }
  SECTION("an HTTP error") {
    stand_in s({{.status = 502, .body = "Bad Gateway"}});
    ddns::duckdns d(loopback_get, s.base(), 2000);
    CHECK(d.update(req()).what == ddns::outcome::bad_answer);
  }
  SECTION("a timeout") {
    stand_in s({{.body = "OK", .hang_ms = 1500}});
    ddns::duckdns d(loopback_get, s.base(), 300);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(d.update(req()).what == ddns::outcome::no_answer);
    CHECK(std::chrono::steady_clock::now() - t0 < 1200ms);
  }
  SECTION("nobody there") {
    std::string base;
    {
      stand_in s({});
      base = s.base();
    }
    ddns::duckdns d(loopback_get, base, 500);
    CHECK(d.update(req()).what == ddns::outcome::no_answer);
  }
}

TEST_CASE("ddns: an IPv6 record DuckDNS still holds is cleared when the port is not open on v6", "[driftwire][ddns]") {
  stand_in s({{.body = "OK\n198.51.100.7\n2a01:4f8::7\nNOCHANGE"},
              {.body = "OK\n\n\nUPDATED"},
              {.body = "OK\n198.51.100.7\n\nUPDATED"}});
  ddns::duckdns d(loopback_get, s.base(), 2000);
  const auto a = d.update(req());
  CHECK(a.what == ddns::outcome::ok);
  CHECK(a.ipv4 == addr("198.51.100.7"));
  CHECK_FALSE(a.ipv6);
  CHECK(a.changed);
  const auto p = s.paths();
  REQUIRE(p.size() == 3);
  CHECK(has(p[0], "&ipv6=&"));
  CHECK(has(p[1], "&clear=true"));
  CHECK_FALSE(has(p[2], "clear"));

  // Told one may be left (the keeper's first update), it clears even when the answer does not show it.
  s.set_script({{.body = "OK\n198.51.100.7\n\nNOCHANGE"}});
  auto hinted = req();
  hinted.clear_ipv6 = true;
  CHECK(d.update(hinted).what == ddns::outcome::ok);
  REQUIRE(s.count() == 6);
  CHECK(has(s.paths()[4], "&clear=true"));
  // Without the hint and with nothing shown: one request.
  CHECK(d.update(req()).what == ddns::outcome::ok);
  REQUIRE(s.count() == 7);

  // With the port open on v6, the record is sent, never cleared.
  s.set_script({{.body = "OK\n198.51.100.7\n2a01:4f8::7\nUPDATED"}});
  const auto b = d.update(req(std::nullopt, addr("2a01:4f8::7")));
  CHECK(b.what == ddns::outcome::ok);
  CHECK(b.ipv6 == addr("2a01:4f8::7"));
  const auto q = s.paths();
  REQUIRE(q.size() == 8);
  CHECK(has(q[7], "&ipv6=2a01:4f8::7&"));
}

TEST_CASE("ddns: the schedule: every 5 minutes, at once on a change, at most once a minute, backing off", "[driftwire][ddns]") {
  ddns::schedule s;  // the real timing
  constexpr std::int64_t min = 60 * 1000;
  CHECK(s.due() == 0);  // at start
  s.ran(1000, true);
  CHECK(s.due() == 1000 + 5 * min);
  s.changed(1000 + 10 * 1000);  // a change 10 s later waits for the minute
  CHECK(s.due() == 1000 + min);
  s.ran(1000 + min, true);
  s.changed(1000 + 3 * min);  // long after: at once
  CHECK(s.due() == 1000 + 3 * min);

  // Failures: 1, 2, 4, 8, 16, 30, 30 minutes.
  std::int64_t t = 10 * min;
  const std::int64_t waits[] = {1, 2, 4, 8, 16, 30, 30};
  for (const std::int64_t w : waits) {
    s.ran(t, false);
    CHECK(s.due() == t + w * min);
    t = s.due();
  }
  CHECK(s.failures() == 7);
  // A change during a backoff tries again a minute after the last try.
  s.ran(t, false);
  s.changed(t + 1000);
  CHECK(s.due() == t + min);
  // Success ends the backoff.
  s.ran(t + min, true);
  CHECK(s.failures() == 0);
  CHECK(s.due() == t + min + 5 * min);
}

TEST_CASE("ddns: the keeper updates at start, on its period, and at once on a change", "[driftwire][ddns]") {
  stand_in s({{.body = "OK\n198.51.100.7\n\nUPDATED"}});
  log_lines log;
  ddns::keeper k({.subdomain = kSub,
                  .token = kToken,
                  .dns = std::make_unique<ddns::duckdns>(loopback_get, s.base(), 2000),
                  .log = log.sink(),
                  .when = fast()});
  CHECK(k.status().name == "myhome-42.duckdns.org");
  CHECK(k.status().last == ddns::outcome::none);
  k.start();
  // The first update also clears an IPv6 record a run before may have left.
  REQUIRE(s.wait_for(3, 2s));
  {
    const auto p = s.paths();
    CHECK(has(p[0], "&ipv6=&"));
    CHECK(has(p[1], "&clear=true"));
    CHECK(has(p[2], "&ip=&ipv6=&"));
  }
  // The status follows the update.
  for (int i = 0; i < 100 && k.status().last == ddns::outcome::none; ++i) std::this_thread::sleep_for(10ms);
  auto st = k.status();
  CHECK(st.last == ddns::outcome::ok);
  CHECK(st.ipv4 == addr("198.51.100.7"));
  CHECK(st.last_ok > 0);
  CHECK(st.failures == 0);
  CHECK_FALSE(st.ipv6_sent);

  // The period (800 ms here, 5 minutes for real): one request, no clear now.
  REQUIRE(s.wait_for(4, 2s));
  CHECK_FALSE(has(s.paths()[3], "clear"));

  // A change: at once (once the minimum gap has passed), with the new address.
  const auto before = s.count();
  k.address_changed(addr("93.184.216.34"));
  REQUIRE(s.wait_for(before + 1, 300ms));
  CHECK(has(s.paths()[before], "&ip=93.184.216.34&"));

  // None afterwards, or a non-public address (carrier NAT), replaces it: DuckDNS sees it itself.
  std::this_thread::sleep_for(100ms);
  k.address_changed(std::nullopt);
  REQUIRE(s.wait_for(before + 2, 300ms));
  CHECK(has(s.paths()[before + 1], "&ip=&"));
  k.address_changed(addr("93.184.216.34"));
  REQUIRE(s.wait_for(before + 3, 300ms));
  std::this_thread::sleep_for(100ms);
  k.address_changed(addr("100.64.0.9"));
  REQUIRE(s.wait_for(before + 4, 300ms));
  CHECK(has(s.paths()[before + 3], "&ip=&"));

  // IPv6: only a public v6 address counts; a private one is none (no update for it).
  std::this_thread::sleep_for(100ms);
  const auto before6 = s.count();
  k.set_ipv6(addr("fd00::1"));
  k.set_ipv6(addr("198.51.100.7"));
  std::this_thread::sleep_for(150ms);
  CHECK(s.count() == before6);
  k.set_ipv6(addr("2a01:4f8::7"));
  REQUIRE(s.wait_for(before6 + 1, 300ms));
  CHECK(has(s.paths()[before6], "&ipv6=2a01:4f8::7&"));
  k.stop();
  k.stop();  // twice is fine
  for (const auto& l : log.all()) {
    CHECK_FALSE(has(l, kToken));
    CHECK_FALSE(has(l, kSub));
    CHECK_FALSE(has(l, "198.51"));
    CHECK_FALSE(has(l, "93.184"));
    CHECK_FALSE(has(l, "2a01"));
  }
  CHECK_FALSE(log.all().empty());
}

TEST_CASE("ddns: the keeper backs off while DuckDNS refuses or does not answer", "[driftwire][ddns]") {
  stand_in s({{.body = "KO"}});
  log_lines log;
  ddns::keeper k({.subdomain = kSub,
                  .token = kToken,
                  .dns = std::make_unique<ddns::duckdns>(loopback_get, s.base(), 300),
                  .log = log.sink(),
                  .when = fast()});
  k.start();
  // Tries at 0, 120, 360, 840 ms: four within a second, not a storm.
  std::this_thread::sleep_for(1000ms);
  const auto n = s.count();
  CHECK(n >= 3);
  CHECK(n <= 5);
  auto st = k.status();
  CHECK(st.last == ddns::outcome::refused);
  CHECK(st.failures >= 3);
  CHECK(st.last_ok == 0);

  // DuckDNS answers again: the next try succeeds and the count resets.
  s.set_script({{.body = "OK\n198.51.100.7\n\nNOCHANGE"}});
  REQUIRE(s.wait_for(n + 1, 1500ms));
  for (int i = 0; i < 100 && k.status().last != ddns::outcome::ok; ++i) std::this_thread::sleep_for(10ms);
  st = k.status();
  CHECK(st.last == ddns::outcome::ok);
  CHECK(st.failures == 0);
  k.stop();

  bool said_refused = false;
  for (const auto& l : log.all()) {
    said_refused = said_refused || has(l, "refused");
    CHECK_FALSE(has(l, kToken));
    CHECK_FALSE(has(l, "0a1b2c3d"));
    CHECK_FALSE(has(l, kSub));
  }
  CHECK(said_refused);
}

TEST_CASE("ddns: a keeper whose DuckDNS hangs still stops", "[driftwire][ddns]") {
  stand_in s({{.body = "OK", .hang_ms = 3000}});
  ddns::keeper k({.subdomain = kSub,
                  .token = kToken,
                  .dns = std::make_unique<ddns::duckdns>(loopback_get, s.base(), 300),
                  .when = fast()});
  k.start();
  REQUIRE(s.wait_for(1, 1s));
  const auto t0 = std::chrono::steady_clock::now();
  k.stop();  // the fetch's timeout bounds it
  CHECK(std::chrono::steady_clock::now() - t0 < 1500ms);
  CHECK(k.status().last == ddns::outcome::no_answer);
}

TEST_CASE("ddns: the name check compares what resolves with what DuckDNS set", "[driftwire][ddns]") {
  ddns::answer reported;
  reported.what = ddns::outcome::ok;
  reported.ipv4 = addr("198.51.100.7");
  const std::vector<net::address> right{addr("198.51.100.7")};
  const std::vector<net::address> stale{addr("198.51.100.1")};
  CHECK(ddns::compare_name(right, reported).ok());
  CHECK_FALSE(ddns::compare_name(stale, reported).ok());
  CHECK(ddns::compare_name(stale, reported).resolves);
  CHECK_FALSE(ddns::compare_name({}, reported).resolves);

  reported.ipv6 = addr("2a01:4f8::7");
  CHECK_FALSE(ddns::compare_name(right, reported).ok());  // the v6 record has not shown yet
  const std::vector<net::address> both{addr("2a01:4f8::7"), addr("198.51.100.7")};
  CHECK(ddns::compare_name(both, reported).ok());
  CHECK_FALSE(ddns::compare_name(both, ddns::answer{}).ok());  // nothing reported, nothing to match

  std::string asked;
  const auto c = ddns::check_name("myhome-42.duckdns.org", reported, [&](const std::string& n) {
    asked = n;
    return both;
  });
  CHECK(asked == "myhome-42.duckdns.org");
  CHECK(c.ok());

  // The real resolver, on a name that never leaves the machine.
  ddns::answer local;
  local.ipv4 = addr("127.0.0.1");
  CHECK(ddns::check_name("localhost", local).v4_matches);
}

TEST_CASE("ddns: a subdomain or token that is not parse_*'s never reaches a request", "[driftwire][ddns]") {
  stand_in s({{.body = "OK"}});
  ddns::duckdns d(loopback_get, s.base(), 2000);
  auto r = req();
  r.subdomain = "home&ip=1.2.3.4";
  CHECK(d.update(r).what == ddns::outcome::refused);
  r = req();
  r.subdomain = "Home";
  CHECK(d.update(r).what == ddns::outcome::refused);
  r = req();
  r.token = std::string(kToken) + "&x";
  CHECK(d.update(r).what == ddns::outcome::refused);
  CHECK(s.count() == 0);
}
