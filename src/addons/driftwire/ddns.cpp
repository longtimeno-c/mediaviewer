// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The dynamic-DNS keeper (ddns.h).
#include "addons/driftwire/ddns.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace mv::driftwire::ddns {

namespace {

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool is_hex(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::int64_t unix_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

answer fetch_one(const fetch_fn& fetch, const std::string& url, int timeout_ms) {
  answer a;
  if (!fetch) {
    a.what = outcome::no_answer;
    return a;
  }
  const fetch_answer f = fetch(url, timeout_ms);
  if (f.status == 0) {
    a.what = outcome::no_answer;
    return a;
  }
  if (f.status != 200 || f.body.size() > kMaxAnswer) {
    a.what = outcome::bad_answer;
    return a;
  }
  return parse_duckdns(f.body);
}

// "2 min", "45 s": how long until the next try, for a log line.
std::string after(std::int64_t ms) {
  char buf[32];
  if (ms < 60 * 1000) {
    std::snprintf(buf, sizeof buf, "%d s", static_cast<int>(std::max<std::int64_t>(ms, 0) / 1000));
  } else {
    std::snprintf(buf, sizeof buf, "%d min", static_cast<int>((ms + 30 * 1000) / (60 * 1000)));
  }
  return buf;
}

}  // namespace

// ---- what the person types -----------------------------------------------------------------

std::optional<std::string> parse_subdomain(std::string_view typed) {
  std::string s;
  for (const char c : trim(typed)) s.push_back(lower(c));
  if (!s.empty() && s.back() == '.') s.pop_back();
  const std::string zone = std::string(".") + kDuckDnsZone;
  if (s.size() > zone.size() && s.compare(s.size() - zone.size(), zone.size(), zone) == 0) {
    s.resize(s.size() - zone.size());
  }
  if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return std::nullopt;
  for (const char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
  }
  return s;
}

std::string duckdns_name(std::string_view subdomain) {
  std::string n(subdomain);
  n += '.';
  n += kDuckDnsZone;
  return n;
}

std::optional<std::string> parse_token(std::string_view typed) {
  const std::string_view t = trim(typed);
  if (t.size() != 36) return std::nullopt;
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = lower(t[i]);
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return std::nullopt;
    } else if (!is_hex(c)) {
      return std::nullopt;
    }
    out.push_back(c);
  }
  return out;
}

bool save_token(secrets::store& store, std::string_view token) {
  const auto t = parse_token(token);
  if (!t) return false;
  return store.put(kTokenSecret, std::span(reinterpret_cast<const std::uint8_t*>(t->data()), t->size()));
}

std::optional<std::string> load_token(secrets::store& store) {
  const auto v = store.get(kTokenSecret);
  if (!v) return std::nullopt;
  return parse_token(std::string_view(reinterpret_cast<const char*>(v->data()), v->size()));
}

bool forget_token(secrets::store& store) { return store.erase(kTokenSecret); }

// ---- one update ----------------------------------------------------------------------------

const char* outcome_words(outcome o) noexcept {
  switch (o) {
    case outcome::none: return "not tried yet";
    case outcome::ok: return "updated";
    case outcome::refused: return "refused: check the token and the name";
    case outcome::no_answer: return "no answer";
    case outcome::bad_answer: return "an answer that was not DuckDNS's";
  }
  return "unknown";
}

std::string duckdns_url(std::string_view base, const request& r, bool clear) {
  std::string u(base);
  u += "/update?domains=";
  u += r.subdomain;
  u += "&token=";
  u += r.token;
  if (clear) {
    u += "&clear=true";
  } else {
    u += "&ip=";
    if (r.ipv4 && !r.ipv4->v6) u += net::address_text(*r.ipv4);
    u += "&ipv6=";
    if (r.ipv6 && r.ipv6->v6) u += net::address_text(*r.ipv6);
  }
  u += "&verbose=true";
  return u;
}

answer parse_duckdns(std::string_view body) {
  answer a;
  a.what = outcome::bad_answer;
  std::vector<std::string_view> lines;
  body = trim(body);
  for (;;) {
    const std::size_t nl = body.find('\n');
    lines.push_back(trim(body.substr(0, nl)));
    if (lines.size() > 4) return a;
    if (nl == std::string_view::npos) break;
    body.remove_prefix(nl + 1);
  }
  if (lines[0] == "KO" && lines.size() == 1) {
    a.what = outcome::refused;
    return a;
  }
  if (lines[0] != "OK") return a;
  if (lines.size() > 1 && !lines[1].empty()) {
    const auto v4 = net::parse_address(lines[1]);
    if (!v4 || v4->v6) return a;
    a.ipv4 = v4;
  }
  if (lines.size() > 2 && !lines[2].empty()) {
    const auto v6 = net::parse_address(lines[2]);
    if (!v6 || !v6->v6) return a;
    a.ipv6 = v6;
  }
  if (lines.size() > 3) {
    if (lines[3] == "UPDATED") {
      a.changed = true;
    } else if (lines[3] != "NOCHANGE") {
      return a;
    }
  }
  a.what = outcome::ok;
  return a;
}

duckdns::duckdns(fetch_fn fetch, std::string base, int timeout_ms)
    : fetch_(std::move(fetch)), base_(std::move(base)), timeout_ms_(timeout_ms) {}

answer duckdns::update(const request& r) {
  // Never a URL built from anything parse_subdomain / parse_token would refuse.
  if (parse_subdomain(r.subdomain) != r.subdomain || parse_token(r.token) != r.token) {
    return answer{.what = outcome::refused};
  }
  // One deadline for the whole update, however many requests it takes.
  const std::int64_t deadline = steady_ms() + timeout_ms_;
  const auto left = [&] { return static_cast<int>(std::max<std::int64_t>(deadline - steady_ms(), 0)); };
  answer a = fetch_one(fetch_, duckdns_url(base_, r), left());
  // DuckDNS keeps the last ipv6= it was given when the next update leaves it
  // empty; only clear=true (both records) removes it. Clear when the answer
  // shows one, or when the caller says one may be left (r.clear_ipv6: the
  // keeper's first update, and the first after it stopped sending one), so
  // this does not hang on how DuckDNS's verbose answer reports a record it
  // kept. Then set the IPv4 again: the name is without an address for one
  // round trip, and with none until the next try if that update fails.
  const bool sending_v6 = r.ipv6 && r.ipv6->v6;
  if (a.what == outcome::ok && !sending_v6 && (a.ipv6 || r.clear_ipv6) && left() > 0) {
    const answer cleared = fetch_one(fetch_, duckdns_url(base_, r, true), left());
    if (cleared.what != outcome::ok || left() <= 0) {
      // As before (the record kept): the keeper asks again next time.
      a.ipv6_left = true;
      return a;
    }
    answer again = fetch_one(fetch_, duckdns_url(base_, r), left());
    again.changed = again.changed || again.what == outcome::ok;
    return again;
  }
  return a;
}

// ---- the check -----------------------------------------------------------------------------

bool name_check::ok() const noexcept {
  return resolves && (v4_expected || v6_expected) && (!v4_expected || v4_matches) && (!v6_expected || v6_matches);
}

name_check compare_name(std::span<const net::address> found, const answer& reported) {
  name_check c;
  c.resolves = !found.empty();
  c.v4_expected = reported.ipv4.has_value();
  c.v6_expected = reported.ipv6.has_value();
  for (const net::address& a : found) {
    if (reported.ipv4 && a == *reported.ipv4) c.v4_matches = true;
    if (reported.ipv6 && a == *reported.ipv6) c.v6_matches = true;
  }
  return c;
}

name_check check_name(const std::string& name, const answer& reported, resolve_fn resolve) {
  const std::vector<net::address> found = resolve ? resolve(name) : net::resolve(name);
  return compare_name(found, reported);
}

// ---- when to update ------------------------------------------------------------------------

void schedule::ran(std::int64_t now, bool ok) noexcept {
  last_ = now;
  if (ok) {
    failures_ = 0;
    due_ = now + t_.every.count();
    return;
  }
  ++failures_;
  std::int64_t wait = t_.backoff_first.count();
  for (int i = 1; i < failures_ && wait < t_.backoff_max.count(); ++i) wait *= 2;
  wait = std::min<std::int64_t>(wait, t_.backoff_max.count());
  due_ = now + std::max<std::int64_t>(wait, t_.min_gap.count());
}

void schedule::changed(std::int64_t now) noexcept {
  const std::int64_t earliest = last_ < 0 ? now : std::max(now, last_ + t_.min_gap.count());
  due_ = std::min(due_, earliest);
}

// ---- the keeper ----------------------------------------------------------------------------

keeper::keeper(keeper_options o) : o_(std::move(o)), sched_(o_.when) {
  status_.name = duckdns_name(o_.subdomain);
}

keeper::~keeper() { stop(); }

void keeper::start() {
  std::lock_guard lock(m_);
  if (started_ || stop_) return;
  started_ = true;
  thread_ = std::thread([this] { run(); });
}

void keeper::address_changed(std::optional<net::address> v4) {
  {
    std::lock_guard lock(m_);
    // None, an IPv6 or a non-public IPv4 (carrier NAT's 100.64/10, a
    // double NAT's 192.168/16): let the provider see the address instead.
    v4_ = (v4 && !v4->v6 && net::is_public(*v4)) ? v4 : std::nullopt;
    if (busy_) {
      moved_ = true;
    } else {
      sched_.changed(steady_ms());
    }
  }
  cv_.notify_all();
}

void keeper::set_ipv6(std::optional<net::address> v6) {
  if (v6 && !(v6->v6 && net::is_public(*v6))) v6.reset();
  {
    std::lock_guard lock(m_);
    if (v6 == v6_) return;
    v6_ = v6;
    if (busy_) {
      moved_ = true;
    } else {
      sched_.changed(steady_ms());
    }
  }
  cv_.notify_all();
}

keeper_status keeper::status() const {
  std::lock_guard lock(m_);
  keeper_status s = status_;
  s.failures = sched_.failures();
  s.next_try = unix_s() + std::max<std::int64_t>(sched_.due() - steady_ms(), 0) / 1000;
  return s;
}

void keeper::stop() {
  std::thread t;
  {
    std::lock_guard lock(m_);
    stop_ = true;
    t = std::move(thread_);  // one caller joins; a later one finds nothing
  }
  cv_.notify_all();
  if (t.joinable()) {
    if (t.get_id() == std::this_thread::get_id()) {
      t.detach();  // stop() from the log callback: the loop ends on its own
    } else {
      t.join();
    }
  }
}

void keeper::note(std::string_view line) {
  if (o_.log) o_.log(line);
}

void keeper::run() {
  std::unique_lock lock(m_);
  while (!stop_) {
    const std::int64_t now = steady_ms();
    if (now < sched_.due()) {
      cv_.wait_for(lock, std::chrono::milliseconds(sched_.due() - now));
      continue;
    }
    request r{.subdomain = o_.subdomain, .token = o_.token, .ipv4 = v4_, .ipv6 = v6_, .clear_ipv6 = clear_v6_};
    busy_ = true;
    moved_ = false;
    lock.unlock();
    const answer a = o_.dns ? o_.dns->update(r) : answer{.what = outcome::no_answer};
    lock.lock();
    busy_ = false;
    const std::int64_t done = steady_ms();
    sched_.ran(now, a.what == outcome::ok);
    if (moved_) sched_.changed(done);
    moved_ = false;
    const outcome before = status_.last;
    const bool first_ok = a.what == outcome::ok && status_.last_ok == 0;
    const bool moved_name = a.what == outcome::ok && (a.ipv4 != status_.ipv4 || a.ipv6 != status_.ipv6);
    status_.last = a.what;
    status_.last_try = unix_s();
    status_.ipv6_sent = r.ipv6.has_value();
    // A record possibly left is cleared until an update without IPv6 succeeds
    // without one left; it may be left again whenever one is sent.
    if (r.ipv6) {
      clear_v6_ = true;
    } else if (a.what == outcome::ok && !a.ipv6_left) {
      clear_v6_ = false;
    }
    if (a.what == outcome::ok) {
      status_.last_ok = status_.last_try;
      status_.ipv4 = a.ipv4;
      status_.ipv6 = a.ipv6;
    }
    // A line when something happened: the first success, a change, a
    // failure, the first success after failures; not every quiet 5 minutes.
    const bool say = a.what != outcome::ok || first_ok || moved_name || a.changed || before != outcome::ok;
    const int failures = sched_.failures();
    const std::int64_t next = sched_.due() - done;
    const char* const who = o_.dns ? o_.dns->name() : "DDNS";
    lock.unlock();

    if (!say) {
      lock.lock();
      continue;
    }
    std::string line = who;
    switch (a.what) {
      case outcome::ok:
        line += (a.changed || moved_name) ? ": the name now points at this home" : ": the name is up to date";
        break;
      case outcome::refused:
        line += " refused the update: check the token and the name";
        break;
      case outcome::no_answer:
        line += " did not answer";
        break;
      case outcome::bad_answer:
      case outcome::none:
        line += " gave an answer that was not its own";
        break;
    }
    if (a.what != outcome::ok) {
      char buf[48];
      std::snprintf(buf, sizeof buf, " (%d in a row; next try in ", failures);
      line += buf;
      line += after(next);
      line += ')';
    }
    note(line);
    lock.lock();
  }
}

}  // namespace mv::driftwire::ddns
