// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/phone/host_tray.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "wire/net.h"

namespace mv::phone::host_tray {

namespace fs = std::filesystem;

namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// At most `max` bytes, cut where a character starts.
std::string clip(const std::string& s, std::size_t max = 60) {
  if (s.size() <= max) return s;
  std::size_t at = max;
  while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
  return s.substr(0, at) + "…";
}

std::string count_of(std::size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

std::string bytes_words(std::uint64_t n) {
  // Decimal units, as the OS's file sizes; one decimal below 10.
  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int u = 0;
  while (v >= 1000.0 && u < 4) {
    v /= 1000.0;
    ++u;
  }
  char buf[32];
  if (u == 0 || v >= 10.0) {
    (void)std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
  } else {
    (void)std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
  }
  return buf;
}

std::string local_clock(std::int64_t unix_s) {
  const std::time_t tt = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#if defined(_WIN32)
  (void)localtime_s(&tm, &tt);
#else
  (void)localtime_r(&tt, &tm);
#endif
  char buf[32];  // room for any int (GCC's -Wformat-truncation), though a time needs 5
  (void)std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

std::string via_words(const std::string& via) {
  return via == "home" ? "home network" : via == "relay" ? "through your relay" : "over the internet";
}

std::vector<std::string> usage_lines(const self_host::running_state& s,
                                     const std::function<std::string(std::int64_t)>& clock_in) {
  const auto clock = clock_in ? clock_in : local_clock;
  std::vector<std::string> out;
  if (!s.day.empty() && (s.read_today > 0 || s.written_today > 0)) {
    std::string u = "Today: " + bytes_words(s.read_today) + " read, " + bytes_words(s.written_today) + " written";
    if (s.read_rate > 0 && s.write_rate > 0) {
      u += " \u00b7 now " + bytes_words(s.read_rate) + "/s and " + bytes_words(s.write_rate) + "/s";
    } else if (s.read_rate > 0) {
      u += " \u00b7 reading " + bytes_words(s.read_rate) + "/s";
    } else if (s.write_rate > 0) {
      u += " \u00b7 writing " + bytes_words(s.write_rate) + "/s";
    }
    out.push_back(std::move(u));
  }
  if (s.paused) return out;
  const std::size_t n = s.clients.size();
  if (n == 0) out.emplace_back("No phones connected");
  for (std::size_t i = 0; i < n && i < kMaxNames; ++i) {
    const auto& c = s.clients[i];
    std::string l = clip(c.name, 40) + " \u00b7 " + via_words(c.via);
    if (c.since > 0) l += " since " + clock(c.since);
    if (c.read > 0 || c.written > 0) {
      l += " \u00b7 " + bytes_words(c.read) + " read";
      if (c.written > 0) l += ", " + bytes_words(c.written) + " written";
    }
    if (c.read_rate > 0 && c.write_rate > 0) l += " \u00b7 reading and writing";
    else if (c.read_rate > 0) l += " \u00b7 reading";
    else if (c.write_rate > 0) l += " \u00b7 writing";
    out.push_back(std::move(l));
  }
  if (n > kMaxNames) out.push_back("and " + count_of(n - kMaxNames, "more phone", "more phones"));
  return out;
}

model build(const input& in) {
  model m;
  m.login = in.at_login;
  if (!in.running || !in.state) return m;
  const self_host::running_state& s = *in.state;
  m.shown = true;
  m.label = in.duckdns.empty() ? std::string("Hosting from this computer")
                               : "Hosting as " + in.duckdns + ".duckdns.org";

  std::vector<row> info;
  const auto text = [&](std::string t) { info.push_back({"text", {}, std::move(t), false}); };
  // On: serving. Off: `host pause` (nothing served, no relay, the router's
  // mapping and the stay-awake request let go); the host and start at login stay.
  info.push_back({"toggle", "hosting", "Hosting", !s.paused});
  bool trouble = false;
  if (s.paused) {
    m.what = status::paused;
    text("Off: phones can\u2019t reach this computer");
  } else if (s.folders == 0) {
    m.what = status::nothing_shared;
    text("Nothing shared yet");
  } else {
    text(s.service_port ? "Serving phones" : "Not serving: its port is taken");
    trouble = s.service_port == 0;
    if (s.relay_port == 0) {
      text("The relay could not start: its port is taken");
      trouble = true;
    }
    if (s.tls_failed) {
      text("The web-site look could not start");
      trouble = true;
    }
  }
  // Drive health: the shared folders whose drive is gone, then combined
  // drives once they exist (issue #686 §3 writes state.json's "drives").
  if (s.folders > 0) {
    if (s.offline > 0) {
      text(std::to_string(s.offline) + " of " + count_of(s.folders, "folder", "folders") + " offline");
      trouble = true;
    } else {
      text(s.folders == 1 ? std::string("Its folder is online") : "All " + std::to_string(s.folders) + " folders online");
    }
  }
  // Combined drives (PR #692's keeper): a line per set, two at most; degraded,
  // offline or failing is a problem.
  for (std::size_t i = 0; i < s.drive_lines.size() && i < 2; ++i) text(clip(s.drive_lines[i], 80));
  if (s.drives_health >= 2) trouble = true;
  // Usage today and now, then each phone connected (the card and `host status` say the same).
  for (auto& l : usage_lines(s, in.clock)) text(std::move(l));
  if (!s.paused && s.folders > 0) m.what = trouble ? status::problem : status::serving;

  const std::vector<row> buttons{
      {"separator", {}, {}, false},
      {"button", "quit", "Quit hosting", false},
  };
  // The registry takes a label of 120 bytes at most (keep_menu's rule).
  for (auto& r : info) r.label = clip(r.label, 117);
  // The buttons always fit: what is said is cut first.
  if (info.size() + buttons.size() > kMaxRows) info.resize(kMaxRows - buttons.size());
  m.rows = std::move(info);
  m.rows.insert(m.rows.end(), buttons.begin(), buttons.end());
  return m;
}

std::string menu_json(const model& m) {
  json::writer w;
  w.begin_object().key("items").begin_array();
  for (const row& r : m.rows) {
    w.begin_object().key("kind").string(r.kind);
    if (!r.id.empty()) w.key("id").string(r.id);
    if (!r.label.empty()) w.key("label").string(r.label);
    if (r.kind == "toggle") w.key("value").boolean(r.value);
    w.end_object();
  }
  w.end_array().end_object();
  return w.take();
}

// ---- the watcher --------------------------------------------------------------------------

namespace {

std::optional<std::string> read_small(const std::string& file, std::size_t max = 256 * 1024) {
  std::ifstream in(fs_path(file), std::ios::binary);
  if (!in) return std::nullopt;
  std::string out;
  char buf[4096];
  while (in.read(buf, sizeof buf) || in.gcount() > 0) {
    out.append(buf, static_cast<std::size_t>(in.gcount()));
    if (out.size() > max) return std::nullopt;
  }
  return out;
}

// What a stat of state.json and the lock said: a change means read again.
struct look {
  bool exists = false, running = false, at_login = false;
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
  bool operator==(const look&) const = default;
};

}  // namespace

struct watcher::state {
  options o;
  mutable std::mutex m;
  std::condition_variable cv;
  bool stopping = false, look_asked = false;  // under m
  std::deque<std::string> picks;              // under m
  model cur;                                  // under m
  std::thread t;

  void run() {
    self_host::where w = o.where ? o.where() : self_host::find();
    const std::string host = w.data_dir.empty() ? std::string() : w.data_dir + "/host";
    look last;
    bool first = true, at_login = false;
    auto next_login = std::chrono::steady_clock::now();
    int quick = 0;              // looks left at 1 s after a verb (the host takes it within 2 s)
    bool quit_ran = false;      // Quit hosting from here: gone at once, no grace
    std::chrono::steady_clock::time_point quit_at{};
    std::optional<std::chrono::steady_clock::time_point> gone_since;  // the host stopped holding its lock
    bool host_folder = true;
    for (;;) {
      std::deque<std::string> todo;
      {
        std::unique_lock lock(m);
        if (!first) {
          // No host folder at all (nothing set up): a slow look; the card's Start asks for one now.
          const int wait = quick > 0 ? 1000 : host_folder ? o.poll_ms : std::max(o.poll_ms, o.idle_poll_ms);
          if (quick > 0) --quick;
          cv.wait_for(lock, std::chrono::milliseconds(wait),
                      [this] { return stopping || look_asked || !picks.empty(); });
        }
        if (stopping) return;
        look_asked = false;
        todo.swap(picks);
      }
      bool force = first;
      for (const std::string& verb : todo) {
        if (o.run) {
          (void)o.run(w, verb);
        } else {
          if (w.program.empty()) w.program = self_host::find().program;  // installed since
          (void)self_host::run_host(w, {verb}, {}, 30000);
        }
        force = true;
        quick = 3;
        if (verb == "quit") {
          quit_ran = true;
          quit_at = std::chrono::steady_clock::now();
        }
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_login) {
        std::error_code ec;
        at_login = !w.login_entry.empty() && fs::is_regular_file(fs_path(w.login_entry), ec);
        next_login = now + std::chrono::milliseconds(o.login_every_ms);
      }
      look l;
      l.at_login = at_login;
      if (!host.empty()) {
        std::error_code fec;
        host_folder = fs::is_directory(fs_path(host), fec);
        std::error_code ec, ec2;
        const fs::path state_file = fs_path(host + "/state.json");
        l.size = fs::file_size(state_file, ec);
        l.mtime = fs::last_write_time(state_file, ec2);
        l.exists = !ec && !ec2;
        // A running host holds host.lock; a state.json left by a crash says nothing.
        // Taken for a moment only, as `host status` does (a host starting retries).
        l.running = l.exists && mv::driftwire::net::try_lock_instance(host + "/host.lock") == nullptr;
      }
      // A host that stopped holding its lock for a moment (an update's restart,
      // a crash the supervisor restarts in 30 s) keeps its entry for a grace,
      // so the app hidden behind it neither quits nor shows its window.
      bool grace_over = false;
      if (l.running) {
        gone_since.reset();
        // A slow quit (the router's mapping let go first) still holds the lock: the
        // quit stands until the lock goes, or a minute has passed and it runs on.
        if (quit_ran && std::chrono::steady_clock::now() - quit_at > std::chrono::minutes(1)) quit_ran = false;
      } else if (cur.shown && !quit_ran) {
        const auto gone_at = std::chrono::steady_clock::now();
        if (!gone_since) gone_since = gone_at;
        grace_over = gone_at - *gone_since >= std::chrono::milliseconds(o.gone_grace_ms);
      }
      const bool in_grace = !l.running && gone_since && !grace_over && !quit_ran;
      if (!force && l == last && !grace_over) continue;
      last = l;
      input in;
      in.running = l.running;
      in.at_login = l.at_login;
      if (l.running) {
        if (const auto text = read_small(host + "/state.json")) in.state = self_host::parse_state(*text);
        if (const auto text = read_small(host + "/host.json", 16 * 1024)) in.duckdns = self_host::parse_duckdns(*text);
      }
      model next = build(in);
      if (in_grace) {
        model held;
        {
          std::lock_guard lock(m);
          held = cur;
        }
        held.login = next.login;
        held.rows = {{"text", {}, "Not running just now: starting again\u2026", false}};
        next = std::move(held);
      }
      if (!l.running) gone_since = in_grace ? gone_since : std::nullopt;
      bool tell = first;
      {
        std::lock_guard lock(m);
        if (!(next == cur)) {
          cur = next;
          tell = true;
        }
      }
      first = false;
      {
        std::lock_guard lock(m);
        if (stopping) return;  // stopped meanwhile: the registry's entry is gone
      }
      if (tell && o.changed) o.changed(next);
    }
  }
};

std::unique_ptr<watcher> watcher::start(options o) {
  std::unique_ptr<watcher> w(new watcher());
  w->st_ = std::make_unique<state>();
  w->st_->o = std::move(o);
  if (w->st_->o.poll_ms <= 0) w->st_->o.poll_ms = kPollMs;
  w->st_->t = std::thread([s = w->st_.get()] { s->run(); });
  return w;
}

watcher::~watcher() { stop(); }

void watcher::pick(const std::string& id, const std::string& value_json) {
  std::string verb = id;
  // The Hosting toggle: its new value.
  if (id == "hosting") verb = value_json == "true" ? "resume" : value_json == "false" ? "pause" : std::string();
  if (verb != "pause" && verb != "resume" && verb != "quit") return;
  {
    std::lock_guard lock(st_->m);
    if (st_->stopping) return;
    st_->picks.push_back(verb);
  }
  st_->cv.notify_all();
}

void watcher::look_now() {
  {
    std::lock_guard lock(st_->m);
    st_->look_asked = true;
  }
  st_->cv.notify_all();
}

model watcher::current() const {
  std::lock_guard lock(st_->m);
  return st_->cur;
}

void watcher::stop(bool wait) noexcept {
  if (!st_) return;
  {
    std::lock_guard lock(st_->m);
    st_->stopping = true;
    st_->picks.clear();
  }
  st_->cv.notify_all();
  if (!st_->t.joinable()) return;
  if (wait) {
    st_->t.join();
  } else {
    st_->t.detach();  // the caller never destroys this watcher
  }
}

}  // namespace mv::phone::host_tray
