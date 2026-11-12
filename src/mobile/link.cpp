// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <thread>

#include <sodium.h>

#include "wire/net.h"
#include "wire/serve_wire.h"
#include "wire/splice.h"

namespace mv::mobile {

namespace dw = driftwire;
namespace serve = driftwire::serve;
using clock = std::chrono::steady_clock;

namespace {

constexpr int kConnectEachMs = 2500;  // a dead address, at most
constexpr int kStallMs = 15000;       // a read that has stopped moving
// The relay starts this long after the direct routes (Driftwire's own race
// delay), so a working direct path always wins and costs the relay nothing.
constexpr int kRelayDelayMs = 1500;

serve::client::options options_for(const service_config& c) {
  serve::client::options o;
  o.access_key = c.access_key;
  o.device_key = c.device_key;
  o.service_key = c.identity;
  o.id = c.device_id;
  o.name = c.device_name;
  if (c.upgrade) o.ops |= serve::kUpgradeOp;  // granted only on a relayed link
  return o;
}

route_problem problem_of(serve::refusal why) {
  switch (why) {
    case serve::refusal::wrong_service: return route_problem::wrong_service;
    case serve::refusal::not_paired: return route_problem::not_paired;
    case serve::refusal::too_old: return route_problem::too_old;
    case serve::refusal::busy: return route_problem::busy;
    case serve::refusal::bad_code:
    case serve::refusal::too_many: return route_problem::bad_code;
    case serve::refusal::expired: return route_problem::expired;
    case serve::refusal::none: return route_problem::unreachable;
  }
  return route_problem::unreachable;
}

struct target {
  std::string text;
  std::string host;
  std::uint16_t port = 0;
  bool over_internet = false;
  bool relay = false;
  dw::relay::token token{};  // relay: this phone's waiter at it
};

// A link the race can cut while the relay's handshake owns it.
class shared_hop final : public dw::link {
 public:
  explicit shared_hop(std::shared_ptr<dw::link> inner) : inner_(std::move(inner)) {}
  expected write_all(std::span<const std::uint8_t> b) override { return inner_->write_all(b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  dw::transport via() const noexcept override { return inner_->via(); }

 private:
  std::shared_ptr<dw::link> inner_;
};

// `with_relay`: a paired phone's routes. Pairing never goes through the relay
// (the service keeps waiters only for devices it already knows).
std::vector<target> targets_of(const service_config& c, bool with_relay = true) {
  std::vector<target> out;
  const auto add = [&](const std::string& text, bool internet) {
    target t;
    t.text = text;
    t.over_internet = internet;
    if (!text.empty() && split_host_port(text, t.host, t.port)) out.push_back(std::move(t));
  };
  add(c.local, false);
  add(c.public_name, true);
  if (with_relay && !c.relay.empty() && !out.empty()) {
    target t;
    t.text = c.relay;
    t.over_internet = true;
    t.relay = true;
    if (split_host_port(c.relay, t.host, t.port)) {
      key32 device_pub{};
      crypto_scalarmult_base(device_pub.data(), c.device_key.data());
      const auto tok = serve::relay_token(c.access_key, device_pub);
      std::memcpy(t.token.data(), tok.data(), t.token.size());
      out.push_back(std::move(t));
    }
  }
  return out;
}

// One attempt on one route's addresses. A winner keeps its result in the race;
// everything else that is still in flight is cut.
template <typename T>
struct race_state {
  std::mutex m;
  std::condition_variable cv;
  std::optional<T> winner;
  std::string winner_address;
  bool winner_internet = false;
  bool winner_relayed = false;
  int running = 0;
  bool over = false;
  std::vector<std::shared_ptr<dw::link>> links;
  std::shared_ptr<dw::link> winner_link;
  std::vector<route_failure> failures;
};

// Every route at once; the first attempt `work` says succeeded wins. `work`
// runs on its own detached thread and may outlive this call by a connect's
// timeout, so it owns everything it touches (a value and a shared_ptr).
template <typename T, typename Work>
result<T> race(const std::vector<target>& targets, int timeout_ms, const cancel_flag* cancel,
               std::vector<route_failure>& failures, route_report& report, Work work) {
  auto st = std::make_shared<race_state<T>>();
  st->running = static_cast<int>(targets.size());
  const auto started = clock::now();
  for (const target& t : targets) {
    std::thread([st, t, work, started, timeout_ms]() mutable {
      if (t.relay) {
        // Last: only when the direct routes have not answered by now, or
        // at once when every one of them has already failed.
        std::unique_lock lock(st->m);
        st->cv.wait_until(lock, started + std::chrono::milliseconds(kRelayDelayMs),
                          [&] { return st->over || st->winner || st->running <= 1; });
        if (st->over || st->winner) {
          --st->running;
          st->cv.notify_all();
          return;
        }
      }
      // By name, always, so a NAT64 network can translate it (§7.3). Local
      // addresses are numeric and resolve to themselves.
      const auto addresses = dw::net::resolve(t.host);
      {
        std::lock_guard lock(st->m);
        st->running += static_cast<int>(addresses.size()) - 1;  // this route is one attempt per address
        if (addresses.empty()) {
          st->failures.push_back({t.text, route_problem::unreachable});
          st->cv.notify_all();
        }
      }
      for (const auto& a : addresses) {
        std::thread([st, t, a, work, started, timeout_ms]() mutable {
          const auto finish = [&](std::optional<route_problem> problem) {
            std::lock_guard lock(st->m);
            if (problem && !st->over) st->failures.push_back({t.text, *problem});
            --st->running;
            st->cv.notify_all();
          };
          auto linked = dw::net::connect({.address = dw::net::address_text(a), .port = t.port}, kConnectEachMs);
          if (!linked) {
            finish(route_problem::unreachable);
            return;
          }
          std::shared_ptr<dw::link> l = std::move(*linked);
          {
            std::lock_guard lock(st->m);
            if (st->over) {
              l->shutdown();
              --st->running;
              return;
            }
            st->links.push_back(l);
          }
          if (t.relay) {
            // Joined to this phone's waiter; the handshake past it is end to end.
            const int left = timeout_ms + kRelayDelayMs -
                             static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count());
            auto joined = dw::relay::join(std::make_unique<shared_hop>(l), t.token, std::max(500, left), [st] {
              std::lock_guard lock(st->m);
              return st->over;
            });
            if (!joined) {
              finish(route_problem::unreachable);
              return;
            }
            // The joined link carries the TCP one inside it: it takes its place
            // in the list, so a winner's own socket is never cut as a loser.
            std::shared_ptr<dw::link> j = std::move(*joined);
            std::lock_guard lock(st->m);
            std::replace(st->links.begin(), st->links.end(), l, j);
            l = std::move(j);
            if (st->over) {
              l->shutdown();
              --st->running;
              st->cv.notify_all();
              return;
            }
          }
          serve::refusal why = serve::refusal::none;
          auto done = work(l, why);
          if (!done) {
            finish(problem_of(why));
            return;
          }
          std::lock_guard lock(st->m);
          if (!st->over && !st->winner) {
            st->winner.emplace(std::move(*done));
            st->winner_link = l;
            st->winner_address = t.text;
            st->winner_internet = t.over_internet;
            st->winner_relayed = t.relay;
          }
          --st->running;
          st->cv.notify_all();
        }).detach();
      }
    }).detach();
  }
  // The relay starts late: it gets the whole time after its start, not what is left of it.
  const bool relayed = std::any_of(targets.begin(), targets.end(), [](const target& t) { return t.relay; });
  const auto until = clock::now() + std::chrono::milliseconds(timeout_ms + (relayed ? kRelayDelayMs : 0));
  std::unique_lock lock(st->m);
  while (!st->winner && st->running > 0 && clock::now() < until) {
    if (cancel && cancel->stopped()) break;
    st->cv.wait_for(lock, std::chrono::milliseconds(50));
  }
  st->over = true;
  for (auto& l : st->links) {
    if (l != st->winner_link) l->shutdown();
  }
  failures = st->failures;
  if (!st->winner) {
    lock.unlock();
    if (cancel && cancel->stopped()) return err(status::cancelled);
    return err(status::io);
  }
  T out = std::move(*st->winner);
  report.address = st->winner_address;
  report.over_internet = st->winner_internet;
  report.relayed = st->winner_relayed;
  return out;
}

status status_of(serve::end_code code) {
  switch (code) {
    case serve::end_code::ok: return status::ok;
    case serve::end_code::refused: return status::permission_denied;
    case serve::end_code::not_found: return status::not_found;
    case serve::end_code::unsupported: return status::unsupported_format;
    case serve::end_code::too_many: return status::busy;
    case serve::end_code::error:
    case serve::end_code::bad_request:
    default: return status::io;
  }
}

}  // namespace

bool split_host_port(std::string_view text, std::string& host, std::uint16_t& port) {
  if (text.empty() || text.size() > 512) return false;
  std::string_view h, p;
  if (text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string_view::npos || close + 2 >= text.size() || text[close + 1] != ':') return false;
    h = text.substr(1, close - 1);
    p = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || text.find(':') != colon) return false;  // a bare IPv6 needs its brackets
    h = text.substr(0, colon);
    p = text.substr(colon + 1);
  }
  if (h.empty() || h.size() > 255) return false;
  if (p.empty() || p.size() > 5) return false;
  unsigned v = 0;
  const auto [stop, why] = std::from_chars(p.data(), p.data() + p.size(), v);
  if (why != std::errc{} || stop != p.data() + p.size() || v == 0 || v > 65535) return false;
  host.assign(h);
  port = static_cast<std::uint16_t>(v);
  return true;
}

expected pair_device(const service_config& c, std::string_view code, route_report& report, const cancel_flag* cancel,
                     int timeout_ms) {
  const auto targets = targets_of(c, false);
  if (targets.empty()) return err(status::invalid_arg);
  const auto opts = options_for(c);
  const std::string code_copy(code), name = c.device_name;
  // Paired by the winning route, in one connection, as the desktop's service expects.
  // A loser that reached the service with a spent code loses honestly.
  auto opts_with_code = opts;
  opts_with_code.pair_code = code_copy;
  opts_with_code.device_name = name;
  auto done = race<std::shared_ptr<serve::client>>(
      targets, timeout_ms, cancel, report.failures, report,
      [opts_with_code](std::shared_ptr<dw::link> l, serve::refusal& why) -> result<std::shared_ptr<serve::client>> {
        auto opened = serve::client::open(std::move(l), opts_with_code, &why);
        if (!opened) return err(opened.error());
        return std::shared_ptr<serve::client>(std::move(*opened));
      });
  if (!done) {
    for (const auto& f : report.failures) {
      if (f.problem == route_problem::wrong_service || f.problem == route_problem::not_paired ||
          f.problem == route_problem::bad_code || f.problem == route_problem::expired) {
        return err(status::permission_denied);
      }
      if (f.problem == route_problem::too_old) return err(status::unsupported_format);
    }
    return err(done.error());
  }
  (*done)->close();
  return {};
}

// ---- link_session ------------------------------------------------------------------------------

link_session::link_session(service_config c) : config_(std::move(c)) {}

link_session::~link_session() {
  stop_upgrades(false);
  disconnect();
  stop_upgrades(true);
}

expected link_session::connect(route_report& report, const cancel_flag* cancel, int timeout_ms) {
  std::lock_guard single(connect_m_);
  {
    std::lock_guard lock(m_);
    if (shut_) return err(status::cancelled);
    // A live link is kept: foregrounding never cuts a copy, a clip or a read.
    if (live_ && !live_->closed()) {
      report = last_report_;
      return {};
    }
  }
  return race_locked(report, cancel, timeout_ms);
}

expected link_session::reconnect(route_report& report, const cancel_flag* cancel, int timeout_ms) {
  std::lock_guard single(connect_m_);
  {
    std::lock_guard lock(m_);
    if (shut_) return err(status::cancelled);
  }
  return race_locked(report, cancel, timeout_ms);
}

expected link_session::race_locked(route_report& report, const cancel_flag* cancel, int timeout_ms) {
  const auto targets = targets_of(config_);
  if (targets.empty()) return err(status::invalid_arg);
  const auto opts = options_for(config_);
  auto c = race<std::shared_ptr<serve::client>>(
      targets, timeout_ms, cancel, report.failures, report,
      [opts](std::shared_ptr<dw::link> l, serve::refusal& why) -> result<std::shared_ptr<serve::client>> {
        auto opened = serve::client::open(std::move(l), opts, &why);
        if (!opened) return err(opened.error());
        return std::shared_ptr<serve::client>(std::move(*opened));
      });
  if (!c) {
    for (const auto& f : report.failures) {
      if (f.problem == route_problem::wrong_service || f.problem == route_problem::not_paired ||
          f.problem == route_problem::expired) {
        return err(status::permission_denied);
      }
      if (f.problem == route_problem::too_old) return err(status::unsupported_format);
      if (f.problem == route_problem::busy) return err(status::busy);
    }
    return err(c.error());
  }
  std::shared_ptr<serve::client> previous;
  {
    std::lock_guard lock(m_);
    if (shut_) {
      (*c)->close();
      return err(status::cancelled);
    }
    // Swapped, never closed here: reads already running on the old client hold
    // it and finish on it; it closes when the last of them lets go.
    previous = std::move(live_);
    if (previous) {
      std::erase_if(retired_, [](const auto& w) { return w.expired(); });
      retired_.push_back(previous);  // closed by shut_down if a read still holds it then
    }
    live_ = std::move(*c);
    last_report_ = report;
  }
  if (report.relayed && config_.upgrade && (live_caps_ops() & serve::kUpgradeOp) != 0) start_upgrades();
  return {};
}

std::uint32_t link_session::live_caps_ops() const {
  std::lock_guard lock(m_);
  return live_ ? live_->caps().ops : 0;
}

// ---- a direct path after the relay (docs/plans/nas-relay.md §9) --------------------------------

void link_session::set_upgrade_policy(bool on_cellular, bool allowed_on_cellular) noexcept {
  on_cellular_.store(on_cellular);
  cellular_ok_.store(allowed_on_cellular);
  {
    std::lock_guard lock(up_m_);
    if (upgrade_allowed()) up_now_ = true;  // just allowed again (Wi-Fi, or switched on): try now
  }
  up_cv_.notify_all();
}

bool link_session::upgrade_allowed() const noexcept { return !on_cellular_.load() || cellular_ok_.load(); }

void link_session::start_upgrades() {
  std::lock_guard lock(up_m_);
  up_now_ = true;
  if (up_stop_ || upgrader_.joinable()) {
    up_cv_.notify_all();
    return;
  }
  try {
    upgrader_ = std::thread([this] { upgrade_loop(); });
  } catch (...) {
    // No thread: the relay stays, as when a try fails.
  }
}

void link_session::stop_upgrades(bool join) noexcept {
  std::thread t;
  {
    std::lock_guard lock(up_m_);
    up_stop_ = true;
    if (up_rz_) up_rz_->shutdown();  // the punch and the ask end now
    if (join) t = std::move(upgrader_);
  }
  up_cv_.notify_all();
  if (t.joinable()) t.join();  // with the port shut and the links closed, a try ends at once
}

void link_session::upgrade_loop() {
  static constexpr int kAfterMinutes[] = {1, 5, 15, 60};
  std::size_t tried = 0;
  auto next = clock::now();  // at once
  std::unique_lock lock(up_m_);
  while (!up_stop_) {
    up_cv_.wait_until(lock, next, [&] { return up_stop_ || up_now_; });
    if (up_stop_) return;
    if (!up_now_ && clock::now() < next) continue;
    up_now_ = false;
    lock.unlock();
    bool relayed = false;
    {
      std::lock_guard l(m_);
      relayed = live_ && !live_->closed() && last_report_.relayed && !shut_;
    }
    bool moved = false;
    if (relayed && upgrade_allowed()) moved = try_upgrade();
    lock.lock();
    if (moved || !relayed) return;  // direct now, or nothing to move (a new relayed link starts it again)
    next = clock::now() + std::chrono::minutes(kAfterMinutes[std::min(tried, std::size(kAfterMinutes) - 1)]);
    ++tried;
  }
}

bool link_session::try_upgrade() {
  std::shared_ptr<serve::client> c;
  std::string relay_text;
  {
    std::lock_guard lock(m_);
    if (!live_ || shut_ || !last_report_.relayed) return false;
    c = live_;
    relay_text = last_report_.address;
  }
  auto opened = dw::net::open_rendezvous(config_.loopback_only);
  if (!opened) return false;
  std::shared_ptr<dw::net::tcp_rendezvous> rz(std::move(*opened));
  {
    std::lock_guard lock(up_m_);
    if (up_stop_) return false;
    up_rz_ = rz;
  }
  struct closer {
    link_session& s;
    std::shared_ptr<dw::net::tcp_rendezvous>& r;
    ~closer() {
      r->shutdown();
      std::lock_guard lock(s.up_m_);
      s.up_rz_.reset();
    }
  } close_it{*this, rz};
  // This side's candidates at the port: its own network, and its public side as the relay sees it.
  std::vector<serve::candidate> mine;
  if (config_.loopback_only) {
    mine.push_back({"127.0.0.1", rz->port()});
  } else {
    for (const auto& a : dw::net::local_addresses()) {
      if (mine.size() >= serve::kMaxCandidates - 1) break;
      mine.push_back({dw::net::address_text(a), rz->port()});
    }
  }
  std::string host;
  std::uint16_t port = 0;
  if (split_host_port(relay_text, host, port)) {
    std::vector<dw::net::endpoint> relay;
    if (const auto a = dw::net::parse_address(host)) {
      relay.push_back({dw::net::address_text(*a), port});
    } else if (const auto found = dw::net::resolve(host); !found.empty()) {
      relay.push_back({dw::net::address_text(found.front()), port});
    }
    if (!relay.empty()) {
      if (auto l = rz->next(relay, 3000)) {
        if (auto seen = dw::relay::ask_seen(**l, 3000)) {
          const bool known = std::any_of(mine.begin(), mine.end(), [&](const serve::candidate& m) {
            return m.address == seen->address && m.port == seen->port;
          });
          if (!known && mine.size() < serve::kMaxCandidates) mine.push_back({seen->address, seen->port});
        }
        (*l)->shutdown();
      }
    }
  }
  // Asked inside the sealed session: the relay sees neither side's list.
  auto answer = c->admin(serve::candidates_json(mine, "upgrade"));
  if (!answer) return false;
  std::vector<dw::net::endpoint> peers;
  for (const auto& t : serve::parse_candidates(*answer)) peers.push_back({t.address, t.port});
  if (peers.empty()) return false;
  auto punched = rz->next(peers, serve::kPunchMs);
  if (!punched) return false;
  // The whole handshake again on the new path: only this phone completes it.
  serve::refusal why = serve::refusal::none;
  auto direct = serve::client::open(std::shared_ptr<dw::link>(std::move(*punched)), options_for(config_), &why);
  if (!direct) return false;
  std::shared_ptr<serve::client> fresh(std::move(*direct));
  std::lock_guard single(connect_m_);
  std::lock_guard lock(m_);
  if (shut_ || live_ != c) {
    fresh->close();  // reconnected meanwhile: that one stands
    return false;
  }
  // As a reconnect swaps: reads running on the relayed client finish there.
  std::erase_if(retired_, [](const auto& w) { return w.expired(); });
  retired_.push_back(live_);
  live_ = std::move(fresh);
  last_report_.relayed = false;
  last_report_.upgraded = true;
  last_report_.address = "direct";
  return true;
}

bool link_session::upgrade_now_for_test() {
  {
    std::lock_guard lock(m_);
    if (!live_ || !last_report_.relayed) return false;
  }
  return upgrade_allowed() && try_upgrade();
}

void link_session::shut_down() noexcept {
  stop_upgrades(false);
  std::vector<std::shared_ptr<serve::client>> still_reading;
  {
    std::lock_guard lock(m_);
    shut_ = true;
    for (auto& w : retired_) {
      if (auto c = w.lock()) still_reading.push_back(std::move(c));
    }
    retired_.clear();
  }
  for (auto& c : still_reading) c->close();
  disconnect();
  streams_cv_.notify_all();
  stop_upgrades(true);
}

void link_session::disconnect() noexcept {
  std::shared_ptr<serve::client> gone;
  {
    std::lock_guard lock(m_);
    gone = std::move(live_);
    live_.reset();
  }
  if (gone) gone->close();
}

int link_session::grant(std::int64_t& expires_unix, bool& scoped) const {
  std::lock_guard lock(m_);
  if (!live_ || live_->closed()) return -1;
  if (!live_->caps().has_grant) return 0;
  expires_unix = live_->caps().grant_expires;
  scoped = live_->caps().grant_scoped;
  return 1;
}

bool link_session::connected() const {
  std::lock_guard lock(m_);
  return live_ && !live_->closed();
}

void link_session::dropped(const std::shared_ptr<serve::client>& c) noexcept {
  std::lock_guard lock(m_);
  if (live_ != c) return;
  // Other reads may still be on it: Forget must be able to cut them, as for a swapped-out client.
  std::erase_if(retired_, [](const auto& w) { return w.expired(); });
  retired_.push_back(live_);
  live_.reset();
}

result<std::shared_ptr<serve::client>> link_session::client(const cancel_flag* cancel) {
  {
    std::lock_guard lock(m_);
    if (live_ && !live_->closed()) return live_;
  }
  route_report report;
  if (auto c = connect(report, cancel); !c) return err(c.error());  // single-flight: re-checks under connect_m_
  std::lock_guard lock(m_);
  if (live_) return live_;
  return err(status::io);
}

// One below the service's limit: it frees a slot only after the end of a stream has gone out.
constexpr std::size_t kClientStreams = serve::kMaxStreams - 1;

class link_session::stream_slot {
 public:
  stream_slot(link_session& s, const cancel_flag* cancel) : s_(s) {
    std::unique_lock lock(s_.streams_m_);
    while (s_.streams_ >= kClientStreams) {
      if ((cancel && cancel->stopped()) || s_.is_shut()) return;
      s_.streams_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    ++s_.streams_;
    held_ = true;
  }
  ~stream_slot() {
    if (!held_) return;
    {
      std::lock_guard lock(s_.streams_m_);
      --s_.streams_;
    }
    s_.streams_cv_.notify_one();
  }
  [[nodiscard]] bool held() const noexcept { return held_; }
  stream_slot(const stream_slot&) = delete;
  stream_slot& operator=(const stream_slot&) = delete;

 private:
  link_session& s_;
  bool held_ = false;
};

bool link_session::is_shut() const {
  std::lock_guard lock(m_);
  return shut_;
}

result<std::vector<serve::root_info>> link_session::roots() {
  stream_slot slot(*this, nullptr);
  if (!slot.held()) return err(status::cancelled);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->roots();
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

result<std::vector<serve::list_entry>> link_session::list(const std::string& root, const std::string& rel) {
  stream_slot slot(*this, nullptr);
  if (!slot.held()) return err(status::cancelled);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->list(root, rel);
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

result<serve::stat_info> link_session::stat(const std::string& root, const std::string& rel) {
  stream_slot slot(*this, nullptr);
  if (!slot.held()) return err(status::cancelled);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->stat(root, rel);
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

expected link_session::read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                  std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel, bool bulk) {
  stream_slot slot(*this, cancel);
  if (!slot.held()) return err(status::cancelled);
  auto c = client(cancel);
  if (!c) return err(c.error());
  if (cancel && cancel->stopped()) return err(status::cancelled);
  auto stream = (*c)->read_range(root, rel, at, length, serve::kMaxStreamCredit / 8, bulk);
  if (!stream) {
    dropped(*c);
    return err(stream.error());
  }
  auto last_data = clock::now();
  for (;;) {
    if (cancel && cancel->stopped()) {
      (*stream)->cancel();
      return err(status::cancelled);
    }
    auto chunk = (*stream)->next(250);
    if (!chunk) {
      if (chunk.error() == status::timeout) {
        if (clock::now() - last_data > std::chrono::milliseconds(kStallMs)) {
          (*stream)->cancel();
          dropped(*c);
          return err(status::io);
        }
        continue;
      }
      dropped(*c);
      return err(status::io);
    }
    if (!*chunk) {
      const auto code = (*stream)->code();
      if (code == serve::end_code::ok) return {};
      return err(status_of(code));
    }
    last_data = clock::now();
    if (!sink((*chunk)->offset, (*chunk)->bytes)) {
      (*stream)->cancel();
      return err(status::cancelled);
    }
  }
}

}  // namespace mv::mobile
