// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Keeping a name pointed at a home whose address changes
// (docs/plans/self-hosted.md §4, docs/design/28 "DuckDNS"): the self-hosting
// `driftwire host` keeps `SUB.duckdns.org` on the home's public address, so
// the phone's pairing QR and the laptop's relay line can carry a name.
//
//   provider   one update of a dynamic-DNS name. DuckDNS is the only one
//              built (a "custom update URL" provider is a later slice, if
//              asked). DuckDNS's whole API is one HTTPS GET:
//                https://www.duckdns.org/update?domains=SUB&token=TOKEN
//                  &ip=[V4]&ipv6=[V6]&verbose=true
//              answered "OK\nV4\nV6\nUPDATED|NOCHANGE" or "KO". An empty ip=
//              is "the address you saw me from" (IPv4 only).
//   keeper     a thread of its own that updates at start, every 5 minutes
//              while nothing changes, at once when told the address moved
//              (address_changed, set_ipv6), never more than once a minute,
//              and backs off on failure (1, 2, 4 … 30 minutes). status() is
//              what `host status` shows.
//   the check  check_name(): does SUB.duckdns.org resolve to what DuckDNS
//              said it set (`host setup`'s check, plan §2 step 1)?
//
// The HTTPS client is the command line's (cli::https_get: WinHTTP,
// NSURLSession, libcurl), which sits above the engine; the host hands it in
// as a fetch_fn. That function must not keep the transport's error text:
// some transports put the URL, and so the token, in it. The answer it gives
// here has a status and a body only, so nothing of the request can reach a
// log line through it.
//
// Secrets: the token lives in the secret store under kTokenSecret (as the
// relay's key does), the subdomain in the host's config. The token is typed
// or pasted (the command line's rule: never an argument), held in memory by
// the keeper, put in the request URL and nowhere else: never in a log line,
// a status, an error or a crash report. Log lines carry no address, no
// subdomain and no token either (rule 6; the person's own `host status`
// shows the addresses).
//
// Privacy: DuckDNS learns the home's public address (it sees the request)
// and that the name is in use, which is the point of it; nothing about
// files. Opt-in; without a name the host is reachable on the LAN only.
//
// Worker threads only (the keeper's own, and whoever calls check_name or a
// provider directly); nothing here is on a UI or render thread. Portable
// (D9): no OS call of its own beyond net::resolve.
//
// For `driftwire host` (cli/host_cmd.*, slice 2), the whole use is:
//
//   auto token = ddns::load_token(store);                 // or ask, then save_token
//   auto sub = ddns::parse_subdomain(host_json_value);    // kept parsed in host.json
//   ddns::keeper k({.subdomain = *sub, .token = *token,
//                   .dns = std::make_unique<ddns::duckdns>(
//                       [](const std::string& url, int ms) {
//                         auto a = cli::https_get(url, ddns::kMaxAnswer, ms);
//                         return ddns::fetch_answer{a.status, std::move(a.body)};  // never a.error
//                       }),
//                   .log = host_log});
//   k.start();                       // updates at once, then on its own
//   k.address_changed(outside_v4);   // the router keeper saw a new address (or nullopt)
//   k.set_ipv6(v6_open ? v6 : std::nullopt);  // the port is open on this global v6
//   auto s = k.status();             // for `host status`
//   k.stop();                        // or the destructor
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "wire/net.h"
#include "wire/secrets.h"

namespace mv::driftwire::ddns {

inline constexpr const char* kTokenSecret = "ddns_token";
inline constexpr const char* kDuckDnsZone = "duckdns.org";
inline constexpr const char* kDuckDnsBase = "https://www.duckdns.org";
inline constexpr std::size_t kMaxAnswer = 4096;  // a verbose answer is under 100 bytes
inline constexpr int kTimeoutMs = 15000;

// ---- what the person types -----------------------------------------------------------------

// The DuckDNS subdomain from what was typed: "home", "Home", "home.duckdns.org"
// and "home.duckdns.org." are all "home". One label of [a-z0-9-], 1 to 63
// long, not starting or ending with '-'. Anything with another dot
// ("photos.home.duckdns.org", "home.example.com", "home.duckdns.org.evil"),
// a scheme, a space inside or a character outside the set is no subdomain.
[[nodiscard]] std::optional<std::string> parse_subdomain(std::string_view typed);
// "home" -> "home.duckdns.org".
[[nodiscard]] std::string duckdns_name(std::string_view subdomain);
// DuckDNS's token: a UUID, 8-4-4-4-12 hex digits (case folded to lower),
// surrounding space trimmed. Anything else (a pasted URL, two tokens, a
// token with '&' after it) is no token, so nothing but hex and '-' ever goes
// into the request.
[[nodiscard]] std::optional<std::string> parse_token(std::string_view typed);

// The token in the secret store (kTokenSecret). save_token takes only what
// parse_token accepts.
[[nodiscard]] bool save_token(secrets::store& store, std::string_view token);
[[nodiscard]] std::optional<std::string> load_token(secrets::store& store);
bool forget_token(secrets::store& store);

// ---- one update ----------------------------------------------------------------------------

// What a GET answered: status 0 when nothing did (no network, a timeout, a
// refused certificate). Deliberately no error text (see the top).
struct fetch_answer {
  int status = 0;
  std::string body;
};
// GET `url` within `timeout_ms`; at most kMaxAnswer bytes of body. It must
// not throw (the keeper's thread does not catch).
using fetch_fn = std::function<fetch_answer(const std::string& url, int timeout_ms)>;

enum class outcome : std::uint8_t {
  none,        // not tried yet
  ok,          // the provider set the name
  refused,     // the provider said no (DuckDNS: KO — a wrong token or subdomain)
  no_answer,   // nothing answered in time
  bad_answer,  // an answer that was not the provider's (an HTTP error, a captive portal's page)
};
// Plain words for `host status` ("updated", "refused: check the token and the
// name", …). No address.
[[nodiscard]] const char* outcome_words(outcome o) noexcept;

struct request {
  std::string subdomain;              // parse_subdomain's
  std::string token;                  // parse_token's
  std::optional<net::address> ipv4;   // none: the address the provider sees
  std::optional<net::address> ipv6;   // none: no IPv6 record (cleared if one was set)
  // No IPv6 sent and one may still be set from before (the keeper's first
  // update, and until an update without one has succeeded): clear it even if
  // the answer does not show it.
  bool clear_ipv6 = false;
};

struct answer {
  outcome what = outcome::none;
  std::optional<net::address> ipv4;  // what the provider says the name now points at
  std::optional<net::address> ipv6;
  bool changed = false;              // DuckDNS: UPDATED (not NOCHANGE)
  bool ipv6_left = false;            // a clear was due and did not happen
};

class provider {
 public:
  virtual ~provider() = default;
  [[nodiscard]] virtual const char* name() const noexcept = 0;
  // Blocks while the provider answers (a worker's call).
  [[nodiscard]] virtual answer update(const request& r) = 0;
};

class duckdns final : public provider {
 public:
  // `base`: kDuckDnsBase; the tests' stand-in server is http://127.0.0.1:N
  // (the real fetch, cli::https_get, takes https only).
  explicit duckdns(fetch_fn fetch, std::string base = kDuckDnsBase, int timeout_ms = kTimeoutMs);
  [[nodiscard]] const char* name() const noexcept override { return "DuckDNS"; }
  // One GET, or, when the request has no IPv6 and DuckDNS may still hold
  // one (the answer shows it, or r.clear_ipv6; it keeps the last ipv6= until
  // told clear=true, which clears both), a clear and the update again, so a
  // phone never prefers a v6 address that cannot answer. All within one
  // timeout. A subdomain or token parse_subdomain / parse_token would not
  // give back unchanged is refused without a request.
  [[nodiscard]] answer update(const request& r) override;

 private:
  fetch_fn fetch_;
  std::string base_;
  int timeout_ms_;
};

// The pieces (the tests). The update URL (`clear`: DuckDNS's clear=true,
// which clears both records).
[[nodiscard]] std::string duckdns_url(std::string_view base, const request& r, bool clear = false);
// A verbose (or plain) DuckDNS body: "OK[\nV4\nV6\nUPDATED|NOCHANGE]" or "KO".
// Anything else is bad_answer. Trailing whitespace and CRLF are fine.
[[nodiscard]] answer parse_duckdns(std::string_view body);

// ---- the check -----------------------------------------------------------------------------

struct name_check {
  bool resolves = false;    // the name gave any address at all
  bool v4_matches = false;  // the reported IPv4 is among them
  bool v6_matches = false;  // the reported IPv6 is among them (false when none was reported)
  bool v4_expected = false; // the update reported an IPv4
  bool v6_expected = false; // the update reported an IPv6
  // Every address reported is among them (an IPv4 alone, or both).
  [[nodiscard]] bool ok() const noexcept;
};
// `found` (the name's addresses) against `reported` (what the update said).
[[nodiscard]] name_check compare_name(std::span<const net::address> found, const answer& reported);
using resolve_fn = std::function<std::vector<net::address>(const std::string& name)>;
// Resolves `name` (net::resolve unless `resolve` is given) and compares. A
// fresh name may take DuckDNS's TTL (60 s) and the resolver's cache to show:
// setup tries again rather than calls it broken. Blocks on DNS (a worker's call).
[[nodiscard]] name_check check_name(const std::string& name, const answer& reported, resolve_fn resolve = {});

// ---- when to update ------------------------------------------------------------------------

struct timing {
  std::chrono::milliseconds every{5 * 60 * 1000};     // while nothing changes
  std::chrono::milliseconds min_gap{60 * 1000};       // between any two updates
  std::chrono::milliseconds backoff_first{60 * 1000}; // after the first failure
  std::chrono::milliseconds backoff_max{30 * 60 * 1000};
};

// The keeper's clock, apart from its thread (the tests drive it with made-up
// times). Times are steady-clock milliseconds.
class schedule {
 public:
  explicit schedule(timing t = {}) : t_(t) {}
  // When the next update is due (<= now: now).
  [[nodiscard]] std::int64_t due() const noexcept { return due_; }
  // An update ran at `now` and succeeded (or not).
  void ran(std::int64_t now, bool ok) noexcept;
  // The address moved at `now`: due at once, but never sooner than min_gap
  // after the last update, and never later than it already was.
  void changed(std::int64_t now) noexcept;
  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  timing t_;
  std::int64_t due_ = 0;   // 0: at once
  std::int64_t last_ = -1; // the last update's start; -1: none yet
  int failures_ = 0;
};

// ---- the keeper ----------------------------------------------------------------------------

struct keeper_options {
  std::string subdomain;                // parse_subdomain's
  std::string token;                    // parse_token's
  std::unique_ptr<provider> dns;        // a duckdns
  // One line, plain ASCII, no address, subdomain or token. May be empty.
  // Called on the keeper's thread.
  std::function<void(std::string_view line)> log;
  timing when{};
};

// What `host status` shows. Times are Unix seconds (0: never).
struct keeper_status {
  std::string name;              // "home.duckdns.org"
  outcome last = outcome::none;  // the last update's
  std::int64_t last_try = 0;
  std::int64_t last_ok = 0;
  int failures = 0;              // in a row, since the last success
  std::int64_t next_try = 0;     // about when the next one runs
  bool ipv6_sent = false;        // the last update named an IPv6 address
  std::optional<net::address> ipv4;  // what the name points at, per the provider
  std::optional<net::address> ipv6;
};

class keeper {
 public:
  explicit keeper(keeper_options o);
  ~keeper();  // stop()
  keeper(const keeper&) = delete;
  keeper& operator=(const keeper&) = delete;

  // Starts the thread; the first update runs at once. Once only.
  void start();
  // The public IPv4 moved (the router keeper's outside_v4, an interface
  // change): an update at once (within min_gap). `v4`: the new address when
  // known and public (sent as ip=); none, or a non-public one (carrier NAT,
  // double NAT), lets the provider see the address instead, and replaces
  // an address given before. Until slice 4 passes one, DuckDNS sees the
  // address the request came from, which it can use only over IPv4.
  void address_changed(std::optional<net::address> v4 = std::nullopt);
  // The IPv6 address the service's port is open on, or none. Only a public
  // (net::is_public) IPv6 address counts; anything else is none. A change
  // updates at once (within min_gap).
  void set_ipv6(std::optional<net::address> v6);
  [[nodiscard]] keeper_status status() const;
  // Ends the thread; an update in flight finishes first (the provider's
  // timeout bounds the wait). Safe to call twice and from any thread.
  void stop();

 private:
  void run();
  void note(std::string_view line);

  keeper_options o_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  schedule sched_;             // under m_
  std::optional<net::address> v4_, v6_;  // under m_: what to send next
  keeper_status status_;       // under m_
  bool stop_ = false;          // under m_
  bool started_ = false;
  bool busy_ = false;          // under m_: an update in flight
  bool moved_ = false;         // under m_: a change came while busy_
  bool clear_v6_ = true;       // under m_: an IPv6 record may be left from before
  std::thread thread_;
};

}  // namespace mv::driftwire::ddns
