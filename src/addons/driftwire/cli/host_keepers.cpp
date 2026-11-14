// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host's two keepers (docs/plans/self-hosted.md §4, §5; slice 5 wires
// slices 3 and 4 into slice 2's hooks): DuckDNS and the router.
#include "addons/driftwire/cli/host_keepers.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <optional>
#include <thread>

#include "addons/driftwire/cli/data_dir.h"
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
    thread_ = std::thread([this] { run(); });
  }
  void ports_changed(const keeper_context& ctx) noexcept final {
    {
      std::lock_guard lock(wm_);
      if (!started_ || stopping_) return;
      pending_ = ctx;
    }
    wcv_.notify_all();
  }
  void stop() noexcept final { stop_worker(); }

 protected:
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
    if (sub_.empty()) return "DuckDNS     none: phones reach this computer at home only (driftwire host name duckdns)";
    const std::string name = ddns::duckdns_name(sub_);
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
  std::string sub_;                  // under m_: what k_ was built from, and its token's hash
  std::array<std::uint8_t, 32> hash_{};
  bool have_token_ = false, built_ = false, idle_test_ = false, rebuilding_ = false;
};

// ---- the router ------------------------------------------------------------------------------

bool same_ports(const std::vector<host_ports::port_wanted>& a, const std::vector<host_ports::port_wanted>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].internal_port != b[i].internal_port || a[i].external_port != b[i].external_port ||
        a[i].fallback_external != b[i].fallback_external || a[i].v6 != b[i].v6 || a[i].what != b[i].what) {
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
      return what + " " + std::to_string(p.external_port) + " open (" + mapping::method_name(p.how) + ")";
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
      built_ = true;
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
  bool built_ = false, asking_ = false, idle_test_ = false, retry_ = false;
  int tries_ = 0;  // asks in a row that left a port closed
};

}  // namespace

std::string ago_words(std::int64_t then_unix, std::int64_t now_unix) {
  const std::int64_t d = now_unix - then_unix;
  if (d < 60) return "just now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  if (d < 2 * 86400) return std::to_string(d / 3600) + " h ago";
  return std::to_string(d / 86400) + " days ago";
}

std::vector<host_ports::port_wanted> wanted_ports(const keeper_context& ctx) {
  std::vector<host_ports::port_wanted> out;
  if (ctx.service_port != 0) out.push_back({.internal_port = ctx.service_port, .what = "service"});
  if (ctx.vpn && ctx.relay_port != 0) out.push_back({.internal_port = ctx.relay_port, .what = "relay"});
  if (ctx.vpn && ctx.relay_tls_port != 0) {
    out.push_back({.internal_port = ctx.relay_tls_port,
                   .external_port = 443,
                   .fallback_external = ctx.relay_tls_port,
                   .what = "web-site look"});
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
  return f;
}

}  // namespace mv::driftwire::cli
