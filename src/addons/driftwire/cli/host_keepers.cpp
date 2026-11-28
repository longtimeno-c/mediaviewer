// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host's two keepers (docs/plans/self-hosted.md §4, §5; slice 5 wires
// slices 3 and 4 into slice 2's hooks): DuckDNS and the router.
#include "addons/driftwire/cli/host_keepers.h"
#include "addons/driftwire/cli/host_copy.h"
#include "addons/driftwire/cli/host_drives.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/update.h"

namespace mv::driftwire::cli {

namespace {

std::int64_t now_s() { return static_cast<std::int64_t>(std::time(nullptr)); }

// A keeper whose work runs on a thread of its own: start() and
// ports_changed() hand it the newest context and return; the thread rebuilds
// (rebuild() may block on the network), and stop() ends it and tears down.
class worker_keeper : public host_keeper {
 public:
  ~worker_keeper() override { stop_worker(); }

  void start(const keeper_context& ctx) final {
    {
      std::lock_guard lock(wm_);
      if (started_) return;
      started_ = true;
      pending_ = ctx;
    }
    wanted(ctx);
    thread_ = std::thread([this] { run(); });
  }
  void ports_changed(const keeper_context& ctx) noexcept final {
    {
      std::lock_guard lock(wm_);
      if (!started_ || stopping_) return;
      pending_ = ctx;
    }
    wanted(ctx);
    wcv_.notify_all();
  }
  void stop() noexcept final { stop_worker(); }

 protected:
  // On the caller, before the worker hears of it: what the context asks for, so
  // status_line never speaks of the one before. Never blocks.
  virtual void wanted(const keeper_context& ctx) noexcept { (void)ctx; }
  // On the worker: the context changed (or the first one). May block.
  virtual void rebuild(const keeper_context& ctx) = 0;
  // On the worker, once, at the end: undo what it holds.
  virtual void teardown() noexcept = 0;
  // Asked from within rebuild (a long ask gives up).
  [[nodiscard]] bool stopping() const noexcept { return stop_flag_.load(); }
  // After a rebuild: when to try again with the same context (< 0: not until it changes).
  [[nodiscard]] virtual int retry_ms() const { return -1; }
  // That try, on the worker.
  virtual void retry_now(const keeper_context& ctx) { (void)ctx; }

  // A subclass's destructor calls this first: the worker never runs into a
  // half-destroyed object.
  void stop_worker() noexcept {
    {
      std::lock_guard lock(wm_);
      stopping_ = true;
    }
    stop_flag_.store(true);
    wcv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void run() {
    std::unique_lock lock(wm_);
    std::optional<keeper_context> last;
    const auto woken = [this] { return stopping_ || pending_.has_value(); };
    for (;;) {
      const int again = last ? retry_ms() : -1;
      if (again < 0) {
        wcv_.wait(lock, woken);
      } else if (!wcv_.wait_for(lock, std::chrono::milliseconds(again), woken)) {
        lock.unlock();
        retry_now(*last);
        lock.lock();
        continue;
      }
      if (stopping_) break;
      keeper_context ctx = std::move(*pending_);
      pending_.reset();
      last = ctx;
      lock.unlock();
      rebuild(ctx);
      lock.lock();
    }
    lock.unlock();
    teardown();
  }

  std::mutex wm_;
  std::condition_variable wcv_;
  std::optional<keeper_context> pending_;  // under wm_
  bool started_ = false, stopping_ = false;  // under wm_
  std::atomic<bool> stop_flag_{false};
  std::thread thread_;
};

// ---- DuckDNS ---------------------------------------------------------------------------------

class ddns_keeper final : public worker_keeper {
 public:
  explicit ddns_keeper(ddns_keeper_config c) : c_(std::move(c)) {
    if (!c_.now_unix) c_.now_unix = now_s;
    if (!c_.fetch) {
      c_.fetch = [](const std::string& url, int ms) {
        auto a = https_get(url, ddns::kMaxAnswer, ms);
        return ddns::fetch_answer{a.status, std::move(a.body)};  // never a.error: it can hold the URL, and so the token
      };
    }
  }
  ~ddns_keeper() override { stop_worker(); }

  void address_may_have_moved() noexcept override {
    std::lock_guard lock(m_);
    if (k_) k_->address_changed(std::nullopt);
  }

  [[nodiscard]] std::string status_line() const override {
    std::lock_guard lock(m_);
    if (idle_test_) return "DuckDNS     not asked (the test build never reaches duckdns.org)";
    if (want_sub_.empty()) return "DuckDNS     none: phones reach this computer at home only (driftwire host name duckdns)";
    const std::string name = ddns::duckdns_name(want_sub_);
    if (!built_ || want_sub_ != sub_) return "DuckDNS     " + name + ", updating";  // the worker has not taken it yet
    if (!k_) {
      return "DuckDNS     " + name + (rebuilding_ ? ", updating" : ": no token kept (driftwire host name duckdns)");
    }
    const ddns::keeper_status s = k_->status();
    const std::int64_t now = c_.now_unix();
    if (s.last == ddns::outcome::none) return "DuckDNS     " + name + ", updating";
    if (s.last == ddns::outcome::ok) return "DuckDNS     " + name + ", updated " + ago_words(s.last_ok, now);
    return "DuckDNS     " + name + ": " + ddns::outcome_words(s.last) +
           (s.last_ok != 0 ? " (last updated " + ago_words(s.last_ok, now) + ")" : std::string(" (never updated)"));
  }

 protected:
  void wanted(const keeper_context& ctx) noexcept override {
    std::lock_guard lock(m_);
    want_sub_ = ctx.ddns_subdomain;
  }
  void rebuild(const keeper_context& ctx) override {
    if (ctx.loopback_only && !c_.on_loopback) {
      std::lock_guard lock(m_);
      idle_test_ = true;
      return;
    }
    std::string sub = ctx.ddns_subdomain;
    std::optional<std::string> token;
    if (!sub.empty() && ctx.secrets) token = ddns::load_token(*ctx.secrets);
    // What k_ was built from is kept as a hash: the token itself lives in the keeper alone.
    std::array<std::uint8_t, 32> hash{};
    if (token) {
      crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(token->data()), token->size(),
                         nullptr, 0);
    }
    std::unique_ptr<ddns::keeper> old;
    {
      std::lock_guard lock(m_);
      if (built_ && sub == sub_ && token.has_value() == have_token_ && sodium_memcmp(hash.data(), hash_.data(), 32) == 0) {
        if (token) sodium_memzero(token->data(), token->size());
        return;
      }
      built_ = true;
      sub_ = sub;
      hash_ = hash;
      have_token_ = token.has_value();
      old = std::move(k_);
      rebuilding_ = true;
    }
    old.reset();  // its update in flight ends first (the provider's timeout)
    if (sub.empty() || !token) {
      std::lock_guard lock(m_);
      rebuilding_ = false;
      return;
    }
    ddns::keeper_options o;
    o.subdomain = sub;
    o.token = *token;
    sodium_memzero(token->data(), token->size());
    o.dns = std::make_unique<ddns::duckdns>(c_.fetch, c_.base);
    o.log = [](std::string_view line) {
      if (!line.empty()) print("driftwire host: " + std::string(line) + "\n");
    };
    o.when = c_.when;
    auto k = std::make_unique<ddns::keeper>(std::move(o));
    k->start();
    std::lock_guard lock(m_);
    k_ = std::move(k);
    rebuilding_ = false;
  }
  void teardown() noexcept override {
    std::unique_ptr<ddns::keeper> k;
    {
      std::lock_guard lock(m_);
      k = std::move(k_);
    }
    k.reset();
  }

 private:
  ddns_keeper_config c_;
  mutable std::mutex m_;
  std::unique_ptr<ddns::keeper> k_;  // under m_
  std::string want_sub_;             // under m_: the newest context's name (wanted)
  std::string sub_;                  // under m_: what k_ was built from, and its token's hash
  std::array<std::uint8_t, 32> hash_{};
  bool have_token_ = false, built_ = false, idle_test_ = false, rebuilding_ = false;
};

// ---- the router ------------------------------------------------------------------------------

bool same_ports(const std::vector<host_ports::port_wanted>& a, const std::vector<host_ports::port_wanted>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].internal_port != b[i].internal_port || a[i].external_port != b[i].external_port ||
        a[i].fallback_external != b[i].fallback_external || a[i].v6 != b[i].v6 || a[i].udp != b[i].udp ||
        a[i].what != b[i].what) {
      return false;
    }
  }
  return true;
}

std::string port_words(const host_ports::port_report& p) {
  const std::string what = p.wanted.what.empty() ? std::string("a port") : p.wanted.what;
  const std::uint16_t asked = p.wanted.external_port ? p.wanted.external_port : p.wanted.internal_port;
  switch (p.v4) {
    case host_ports::port_state::mapped:
      return what + " " + std::to_string(p.external_port) + " open (" + mapping::method_name(p.how) +
             (p.udp == host_ports::port_state::mapped ? ", UDP too"
              : p.udp                                 ? ", not UDP"
                                                      : "") +
             ")";
    case host_ports::port_state::refused:
      return what + " " + std::to_string(asked) + " refused";
    case host_ports::port_state::not_offered:
      break;
  }
  return what + " " + std::to_string(asked) + " not open";
}

class router_keeper final : public worker_keeper {
 public:
  explicit router_keeper(router_keeper_config c) : c_(std::move(c)) {}
  ~router_keeper() override { stop_worker(); }

  [[nodiscard]] std::string status_line() const override {
    std::lock_guard lock(m_);
    if (idle_test_) return "router      not asked (the test build never asks a router)";
    if (!seen_) return "router      not asked yet";  // the worker has not read the ports
    if (want_.empty()) return "router      nothing to open (no folder shared, VPN off)";
    if (!k_) return asking_ ? "router      asking it to open the ports" : "router      not asked yet";
    const host_ports::report r = k_->status();
    std::string line = "router      ";
    for (std::size_t i = 0; i < r.ports.size(); ++i) line += (i ? ", " : "") + port_words(r.ports[i]);
    if (r.kind != host_ports::outside_kind::unknown && r.kind != host_ports::outside_kind::public_address) {
      line += std::string("; outside: ") + host_ports::outside_kind_name(r.kind);
    }
    for (const auto& a : host_ports::advice(r)) line += "\n            " + a;
    return line;
  }

  [[nodiscard]] std::vector<std::pair<std::string, std::int64_t>> state_numbers() const override {
    std::lock_guard lock(m_);
    std::vector<std::pair<std::string, std::int64_t>> out;
    if (!k_) return out;
    const host_ports::report r = k_->status();
    for (const auto& p : r.ports) {
      if (p.v4 != host_ports::port_state::mapped) continue;
      const std::string key = p.wanted.what == "service" ? "service_outside_port"
                              : p.wanted.what == "relay" ? "relay_outside_port"
                                                         : "tls_outside_port";
      out.emplace_back(key, p.external_port);
    }
    out.emplace_back("outside_public", r.kind == host_ports::outside_kind::public_address ? 1 : 0);
    return out;
  }

 protected:
  void rebuild(const keeper_context& ctx) override {
    if (ctx.loopback_only && !c_.on_loopback) {
      std::lock_guard lock(m_);
      idle_test_ = true;
      return;
    }
    auto want = wanted_ports(ctx);
    std::unique_ptr<host_ports::keeper> old;
    {
      std::lock_guard lock(m_);
      if (built_ && same_ports(want, want_)) return;
      if (built_) tries_ = 0;  // the ports changed: a fresh start (a retry clears built_ itself)
      built_ = seen_ = true;
      want_ = want;
      asking_ = !want.empty();
      retry_ = false;
      old = std::move(k_);
    }
    old.reset();  // its forwards removed before the new ones are asked
    if (want.empty() || stopping()) {
      std::lock_guard lock(m_);
      asking_ = false;
      return;
    }
    host_ports::options o = c_.ports;
    o.ledger = ctx.folder + "/router.ledger";
    o.router.cancelled = [this] { return stopping(); };  // this outlives the keeper (teardown)
    o.on_outside_changed = [moved = ctx.address_moved](const net::address&) {
      if (moved) moved();
    };
    auto k = std::make_unique<host_ports::keeper>(std::move(want), std::move(o));
    const host_ports::report r = k->status();
    bool all = r.router_found && !r.cancelled;
    bool any = false, unanswered = !r.router_found;
    for (const auto& p : r.ports) {
      all = all && p.v4 == host_ports::port_state::mapped;
      any = any || p.v4 == host_ports::port_state::mapped;
      unanswered = unanswered || p.v4 == host_ports::port_state::not_offered;
    }
    if (!all && tries_ == 0) {
      for (const auto& a : host_ports::advice(r)) print("driftwire host: " + a + "\n");
    }
    std::lock_guard lock(m_);
    asking_ = false;
    k_ = std::move(k);
    // Nothing open yet (no router: a start at login before the network is up; no answer): asked
    // again on a backoff while the ports wanted stay the same. Never once a port is open (a retry
    // rebuilds, and would close it a moment), and a plain refusal only three times.
    tries_ = all ? 0 : tries_ + 1;
    retry_ = !all && !any && !r.cancelled && (unanswered || tries_ < 3);
  }

  [[nodiscard]] int retry_ms() const override {
    std::lock_guard lock(m_);
    if (!retry_ || want_.empty() || c_.retry_ms.empty()) return -1;
    return c_.retry_ms[std::min<std::size_t>(static_cast<std::size_t>(tries_ - 1), c_.retry_ms.size() - 1)];
  }
  void retry_now(const keeper_context& ctx) override {
    {
      std::lock_guard lock(m_);
      built_ = false;  // the same ports, asked again
    }
    rebuild(ctx);
  }
  void teardown() noexcept override {
    std::unique_ptr<host_ports::keeper> k;
    {
      std::lock_guard lock(m_);
      k = std::move(k_);
    }
    k.reset();  // the forwards removed
  }

 private:
  router_keeper_config c_;
  mutable std::mutex m_;
  std::unique_ptr<host_ports::keeper> k_;      // under m_
  std::vector<host_ports::port_wanted> want_;  // under m_
  bool built_ = false, seen_ = false, asking_ = false, idle_test_ = false, retry_ = false;
  int tries_ = 0;  // asks in a row that left a port closed
};

// ---- the certificate (docs/plans/vpn-stealth.md §1) -----------------------------------------

std::string tls_dir(const std::string& folder) { return folder + "/tls"; }

std::filesystem::path to_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string day_words(std::int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char b[16];
  std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
  return b;
}

class cert_keeper final : public worker_keeper {
 public:
  explicit cert_keeper(cert_keeper_config c) : c_(std::move(c)) {
    if (!c_.now_unix) c_.now_unix = now_s;
    if (!c_.http) {
      c_.http = [](const acme::http_request& r, int ms) {
        auto a = https_request(r.method, r.url, "application/jose+json", r.body, 1 << 20, ms);
        // Never a.error: it can hold the URL.
        return acme::http_response{a.status, std::move(a.body), std::move(a.location), std::move(a.nonce),
                                   a.retry_after};
      };
    }
    if (!c_.fetch) {
      c_.fetch = [](const std::string& url, int ms) {
        auto a = https_get(url, ddns::kMaxAnswer, ms);
        return ddns::fetch_answer{a.status, std::move(a.body)};  // never a.error: it can hold the token
      };
    }
  }
  ~cert_keeper() override { stop_worker(); }

  [[nodiscard]] std::string status_line() const override {
    std::lock_guard lock(m_);
    return "certificate " + words_;
  }

 protected:
  void rebuild(const keeper_context& ctx) override { check(ctx); }
  void retry_now(const keeper_context& ctx) override { check(ctx); }
  [[nodiscard]] int retry_ms() const override {
    std::lock_guard lock(m_);
    return next_ms_;
  }
  void teardown() noexcept override {}

 private:
  void say(std::string words, int next_ms) {
    std::lock_guard lock(m_);
    words_ = std::move(words);
    next_ms_ = next_ms;
  }

  void check(const keeper_context& ctx) {
    constexpr int kDayMs = 86'400'000;
    if (ctx.loopback_only && !c_.on_loopback) return say("self-signed (the test build never asks Let's Encrypt)", -1);
    if (!ctx.vpn || ctx.relay_tls_port == 0) return say("none: the web-site look is off", -1);
    if (ctx.cert_name.empty() || ctx.ddns_subdomain.empty()) {
      return say("self-signed: Let's Encrypt needs a DuckDNS name (driftwire host name duckdns)", -1);
    }
    if (!acme::available()) return say("self-signed: this build cannot get one", -1);
    const std::int64_t now = c_.now_unix();
    if (const auto have = usable_certificate(ctx.folder, ctx.cert_name, now)) {
      if (have->not_after - now > kRenewBeforeS) {
        failures_ = 0;
        const std::int64_t renew_in = have->not_after - kRenewBeforeS - now;
        return say("Let's Encrypt for " + ctx.cert_name + ", until " + day_words(have->not_after),
                   static_cast<int>(std::min<std::int64_t>(kDayMs, renew_in * 1000)));
      }
    }
    // An order is due: unless backing off, or the day's orders are spent.
    if (now < not_before_) {
      return say(last_words_ + "; trying again " + (not_before_ - now < 3600 ? "within the hour" : "in " +
                 std::to_string((not_before_ - now + 3599) / 3600) + " h"),
                 static_cast<int>(std::min<std::int64_t>(kDayMs, (not_before_ - now) * 1000)));
    }
    std::erase_if(orders_, [&](std::int64_t t) { return now - t >= 86400; });
    if (static_cast<int>(orders_.size()) >= kOrdersPerDay) {
      const std::int64_t wait = orders_.front() + 86400 - now;
      return say(last_words_ + "; trying again tomorrow", static_cast<int>(std::min<std::int64_t>(kDayMs, wait * 1000)));
    }
    std::optional<std::string> token = ctx.secrets ? ddns::load_token(*ctx.secrets) : std::nullopt;
    if (!token) return say("self-signed: no DuckDNS token kept (driftwire host name duckdns)", -1);
    orders_.push_back(now);
    say("asking Let's Encrypt for " + ctx.cert_name, -1);

    const std::string dir = tls_dir(ctx.folder);
    std::error_code ec;
    std::filesystem::create_directories(to_path(dir), ec);
    acme::order_options o;
    o.directory = c_.directory;
    o.name = ctx.cert_name;
    if (const auto acct = host_files::read(dir + "/account.pem", 16 * 1024)) o.account_key_pem = *acct;
    o.http = c_.http;
    const std::string sub = ctx.ddns_subdomain;
    o.txt = [this, sub, &token](const std::string& value) {
      return ddns::set_duckdns_txt(c_.fetch, c_.ddns_base, sub, *token, value);
    };
    o.wait = [this](int ms) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (std::chrono::steady_clock::now() < until) {
        if (stopping()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, 100)));
      }
      return !stopping();
    };
    o.propagation_ms = c_.propagation_ms;
    o.poll_ms = c_.poll_ms;
    acme::outcome got = acme::order(o);
    sodium_memzero(token->data(), token->size());
    if (!got.account_key_pem.empty() && got.account_key_pem != o.account_key_pem) {
      (void)host_files::write(dir + "/account.pem", got.account_key_pem);
    }
    sodium_memzero(o.account_key_pem.data(), o.account_key_pem.size());
    sodium_memzero(got.account_key_pem.data(), got.account_key_pem.size());
    if (got.why == acme::failure::stopped) return say("stopped", -1);
    if (got.why == acme::failure::none && host_files::write(dir + "/key.pem", got.key_pem) &&
        host_files::write(dir + "/cert.pem", got.chain_pem)) {
      sodium_memzero(got.key_pem.data(), got.key_pem.size());
      failures_ = 0;
      not_before_ = 0;
      print("driftwire host: a Let's Encrypt certificate for the web-site look, until " + day_words(got.not_after) +
            "\n");
      if (ctx.cert_changed) ctx.cert_changed();
      const std::int64_t renew_in = got.not_after - kRenewBeforeS - c_.now_unix();
      return say("Let's Encrypt for " + ctx.cert_name + ", until " + day_words(got.not_after),
                 static_cast<int>(std::clamp<std::int64_t>(renew_in * 1000, 60'000, kDayMs)));
    }
    sodium_memzero(got.key_pem.data(), got.key_pem.size());
    ++failures_;
    std::int64_t backoff = std::min<std::int64_t>(86400, 3600LL << std::min(failures_ - 1, 5));
    if (got.why == acme::failure::rate_limited) backoff = std::max(backoff, got.retry_after);
    not_before_ = c_.now_unix() + backoff;
    const bool kept = usable_certificate(ctx.folder, ctx.cert_name, c_.now_unix()).has_value();
    last_words_ = std::string(kept ? "Let's Encrypt's, renewal failed: " : "self-signed: ") +
                  acme::failure_words(got.why);
    print("driftwire host: no Let's Encrypt certificate this time: " + std::string(acme::failure_words(got.why)) +
          "\n", true);
    say(last_words_, static_cast<int>(std::min<std::int64_t>(kDayMs, backoff * 1000)));
  }

  cert_keeper_config c_;
  mutable std::mutex m_;
  std::string words_ = "not checked yet";  // under m_
  int next_ms_ = -1;                       // under m_
  // The worker's own:
  std::string last_words_;
  std::vector<std::int64_t> orders_;  // unix s of each order in the last day
  std::int64_t not_before_ = 0;
  int failures_ = 0;
};

}  // namespace

std::optional<cert_files> usable_certificate(const std::string& folder, const std::string& name, std::int64_t now) {
  if (name.empty()) return std::nullopt;
  const std::string dir = tls_dir(folder);
  const auto pem = host_files::read(dir + "/cert.pem", 64 * 1024);
  if (!pem) return std::nullopt;
  const auto info = acme::inspect(*pem, name);
  if (!info || !info->for_name || info->self_signed || info->not_after <= now) return std::nullopt;
  // A key that is not this certificate's (a write cut between the two files) is no certificate.
  auto key = host_files::read(dir + "/key.pem", 16 * 1024);
  if (!key) return std::nullopt;
  const bool matches = acme::key_matches(*pem, *key);
  sodium_memzero(key->data(), key->size());
  if (!matches) return std::nullopt;
  return cert_files{dir + "/cert.pem", dir + "/key.pem", info->not_after};
}

std::unique_ptr<host_keeper> make_cert_keeper(cert_keeper_config c) { return std::make_unique<cert_keeper>(std::move(c)); }

std::string ago_words(std::int64_t then_unix, std::int64_t now_unix) {
  const std::int64_t d = now_unix - then_unix;
  if (d < 60) return "just now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  if (d < 2 * 86400) return std::to_string(d / 3600) + " h ago";
  return std::to_string(d / 86400) + " days ago";
}

std::vector<host_ports::port_wanted> wanted_ports(const keeper_context& ctx) {
  std::vector<host_ports::port_wanted> out;
  // The service's sessions over UDP take the same port number (serve::server::options::udp).
  if (ctx.service_port != 0) out.push_back({.internal_port = ctx.service_port, .udp = true, .what = "service"});
  // With the web-site look, only it is open outside: the plain relay's
  // cleartext "MVRL" head never reaches the internet (docs/plans/vpn-stealth.md).
  // Outside 443, else 8443 (still a port HTTPS uses) when 443 is someone else's.
  if (ctx.vpn && ctx.relay_tls_port != 0) {
    out.push_back({.internal_port = ctx.relay_tls_port,
                   .external_port = 443,
                   .fallback_external = kLookFallbackPort,
                   .what = "web-site look"});
  } else if (ctx.vpn && ctx.relay_port != 0) {
    out.push_back({.internal_port = ctx.relay_port, .what = "relay"});
  }
  return out;
}

std::unique_ptr<host_keeper> make_ddns_keeper(ddns_keeper_config c) { return std::make_unique<ddns_keeper>(std::move(c)); }

std::unique_ptr<host_keeper> make_router_keeper(router_keeper_config c) {
  return std::make_unique<router_keeper>(std::move(c));
}

keeper_factories host_keepers() {
  keeper_factories f;
  f.ddns = [] { return make_ddns_keeper(); };
  f.router = [] { return make_router_keeper(); };
  f.drives = [] { return make_drives_keeper(); };
  f.cert = [] { return make_cert_keeper(); };
  f.copy = [] { return make_copy_keeper(); };
  return f;
}

}  // namespace mv::driftwire::cli
