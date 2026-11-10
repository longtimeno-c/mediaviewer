// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/link.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <thread>

#include "addons/driftwire/net.h"

namespace mv::mobile {

namespace dw = driftwire;
namespace serve = driftwire::serve;
using clock = std::chrono::steady_clock;

namespace {

constexpr int kConnectEachMs = 2500;  // a dead address, at most
constexpr int kStallMs = 15000;       // a read that has stopped moving

serve::client::options options_for(const service_config& c) {
  serve::client::options o;
  o.access_key = c.access_key;
  o.device_key = c.device_key;
  o.service_key = c.identity;
  o.id = c.device_id;
  o.name = c.device_name;
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
    case serve::refusal::none: return route_problem::unreachable;
  }
  return route_problem::unreachable;
}

struct target {
  std::string text;
  std::string host;
  std::uint16_t port = 0;
  bool over_internet = false;
};

std::vector<target> targets_of(const service_config& c) {
  std::vector<target> out;
  const auto add = [&](const std::string& text, bool internet) {
    target t;
    t.text = text;
    t.over_internet = internet;
    if (!text.empty() && split_host_port(text, t.host, t.port)) out.push_back(std::move(t));
  };
  add(c.local, false);
  add(c.public_name, true);
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
               std::vector<route_failure>& failures, std::string& address, bool& over_internet, Work work) {
  auto st = std::make_shared<race_state<T>>();
  st->running = static_cast<int>(targets.size());
  for (const target& t : targets) {
    std::thread([st, t, work]() mutable {
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
        std::thread([st, t, a, work]() mutable {
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
          }
          --st->running;
          st->cv.notify_all();
        }).detach();
      }
    }).detach();
  }
  const auto until = clock::now() + std::chrono::milliseconds(timeout_ms);
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
  address = st->winner_address;
  over_internet = st->winner_internet;
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
  const auto targets = targets_of(c);
  if (targets.empty()) return err(status::invalid_arg);
  const auto opts = options_for(c);
  const std::string code_copy(code), name = c.device_name;
  // Paired by the winning route, in one connection, as the desktop's service expects.
  // A loser that reached the service with a spent code loses honestly.
  auto opts_with_code = opts;
  opts_with_code.pair_code = code_copy;
  opts_with_code.device_name = name;
  auto done = race<std::shared_ptr<serve::client>>(
      targets, timeout_ms, cancel, report.failures, report.address, report.over_internet,
      [opts_with_code](std::shared_ptr<dw::link> l, serve::refusal& why) -> result<std::shared_ptr<serve::client>> {
        auto opened = serve::client::open(std::move(l), opts_with_code, &why);
        if (!opened) return err(opened.error());
        return std::shared_ptr<serve::client>(std::move(*opened));
      });
  if (!done) {
    for (const auto& f : report.failures) {
      if (f.problem == route_problem::wrong_service || f.problem == route_problem::not_paired ||
          f.problem == route_problem::bad_code) {
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

link_session::~link_session() { disconnect(); }

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
      targets, timeout_ms, cancel, report.failures, report.address, report.over_internet,
      [opts](std::shared_ptr<dw::link> l, serve::refusal& why) -> result<std::shared_ptr<serve::client>> {
        auto opened = serve::client::open(std::move(l), opts, &why);
        if (!opened) return err(opened.error());
        return std::shared_ptr<serve::client>(std::move(*opened));
      });
  if (!c) {
    for (const auto& f : report.failures) {
      if (f.problem == route_problem::wrong_service || f.problem == route_problem::not_paired) {
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
    live_ = std::move(*c);
    last_report_ = report;
  }
  return {};
}

void link_session::shut_down() noexcept {
  {
    std::lock_guard lock(m_);
    shut_ = true;
  }
  disconnect();
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

bool link_session::connected() const {
  std::lock_guard lock(m_);
  return live_ && !live_->closed();
}

void link_session::dropped(const std::shared_ptr<serve::client>& c) noexcept {
  std::lock_guard lock(m_);
  if (live_ == c) live_.reset();
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

class link_session::stream_slot {
 public:
  explicit stream_slot(link_session& s) : s_(s) {
    std::unique_lock lock(s_.streams_m_);
    s_.streams_cv_.wait(lock, [this] { return s_.streams_ < serve::kMaxStreams; });
    ++s_.streams_;
  }
  ~stream_slot() {
    {
      std::lock_guard lock(s_.streams_m_);
      --s_.streams_;
    }
    s_.streams_cv_.notify_one();
  }
  stream_slot(const stream_slot&) = delete;
  stream_slot& operator=(const stream_slot&) = delete;

 private:
  link_session& s_;
};

result<std::vector<serve::root_info>> link_session::roots() {
  stream_slot slot(*this);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->roots();
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

result<std::vector<serve::list_entry>> link_session::list(const std::string& root, const std::string& rel) {
  stream_slot slot(*this);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->list(root, rel);
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

result<serve::stat_info> link_session::stat(const std::string& root, const std::string& rel) {
  stream_slot slot(*this);
  auto c = client(nullptr);
  if (!c) return err(c.error());
  auto r = (*c)->stat(root, rel);
  if (!r && r.error() == status::io) dropped(*c);
  return r;
}

expected link_session::read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                  std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel, bool bulk) {
  stream_slot slot(*this);
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
