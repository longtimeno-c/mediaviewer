// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// self_host.h: this computer as the phones' server, through the installed
// driftwire program.
#include "addons/phone/self_host.h"

#include "addons/phone/bundled.h"
#include "addons/phone/service.h"

#include "addons/phone/host_tray.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "io/child_process.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve_wire.h"
#include "wire/share_users.h"

namespace mv::phone::self_host {

namespace fs = std::filesystem;
namespace net = mv::driftwire::net;
namespace serve = mv::driftwire::serve;
namespace secrets = mv::driftwire::secrets;

namespace {

fs::path fs_path(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string getenv_utf8(const char* name) {
  // The app reads these at a click, on the link thread; nothing sets them while it runs.
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  return v ? std::string(v) : std::string();
}

// Small files only: serve.json, host.json, state.json. nullopt: not read.
std::optional<std::string> read_small(const std::string& file) {
  std::error_code ec;
  const auto size = fs::file_size(fs_path(file), ec);
  if (ec || size > 256 * 1024) return std::nullopt;
  std::ifstream in(fs_path(file), std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(static_cast<std::size_t>(size), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (in.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
  return text;
}

bool is_file(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(fs_path(p), ec);
}

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return std::string(s);
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool absolute_path(os o, const std::string& p) {
  if (o == os::windows) {
    // C:\…, C:/…, or \\server\share
    return (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
            (p[2] == '\\' || p[2] == '/')) ||
           p.rfind("\\\\", 0) == 0;
  }
  return !p.empty() && p.front() == '/';
}

std::string join(const std::string& a, const char* b, os o) {
  if (a.empty()) return {};
  return a + (o == os::windows ? "\\" : "/") + b;
}

}  // namespace

os this_os() noexcept {
#if defined(_WIN32)
  return os::windows;
#elif defined(__APPLE__)
  return os::mac;
#else
  return os::linux_os;
#endif
}

env read_env() {
  env e;
#if defined(_WIN32)
  e.home = getenv_utf8("USERPROFILE");
  e.local_app_data = getenv_utf8("LOCALAPPDATA");
  e.path = getenv_utf8("Path");
  if (e.path.empty()) e.path = getenv_utf8("PATH");
#else
  e.home = getenv_utf8("HOME");
  e.xdg_state_home = getenv_utf8("XDG_STATE_HOME");
  e.xdg_config_home = getenv_utf8("XDG_CONFIG_HOME");
  e.path = getenv_utf8("PATH");
#endif
  while (e.home.size() > 1 && (e.home.back() == '/' || e.home.back() == '\\')) e.home.pop_back();
  return e;
}

std::string data_dir_for(os o, const env& e) {
  switch (o) {
    case os::mac:
      return e.home.empty() ? std::string() : e.home + "/Library/Application Support/Driftwire";
    case os::windows:
      return e.local_app_data.empty() ? std::string() : e.local_app_data + "\\Driftwire";
    case os::linux_os:
      if (!e.xdg_state_home.empty() && e.xdg_state_home.front() == '/') return e.xdg_state_home + "/driftwire";
      return e.home.empty() ? std::string() : e.home + "/.local/state/driftwire";
  }
  return {};
}

std::vector<std::string> program_candidates(os o, const env& e) {
  std::vector<std::string> out;
  const char sep = o == os::windows ? ';' : ':';
  const char* name = o == os::windows ? "driftwire.exe" : "driftwire";
  switch (o) {
    case os::windows:
      // install.ps1's folder (docs/design/28 "Installing").
      if (!e.local_app_data.empty()) out.push_back(e.local_app_data + "\\Programs\\Driftwire\\driftwire.exe");
      break;
    case os::mac:
      // Homebrew's (Apple silicon, then Intel), then install.sh's without it.
      out.emplace_back("/opt/homebrew/bin/driftwire");
      out.emplace_back("/usr/local/bin/driftwire");
      if (!e.home.empty()) out.push_back(e.home + "/.local/bin/driftwire");
      break;
    case os::linux_os:
      out.emplace_back("/usr/bin/driftwire");  // the apt, dnf and zypper packages
      if (!e.home.empty()) out.push_back(e.home + "/.local/bin/driftwire");
      out.emplace_back("/usr/local/bin/driftwire");
      break;
  }
  std::size_t at = 0;
  while (at <= e.path.size()) {
    const auto end = e.path.find(sep, at);
    std::string dir = e.path.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (o == os::windows && dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    // Only an absolute folder: a relative PATH entry means "wherever the app was started".
    if (absolute_path(o, dir)) {
      std::string p = join(dir, name, o);
      bool seen = false;
      for (const auto& c : out) seen = seen || c == p;
      if (!seen) out.push_back(std::move(p));
    }
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return out;
}

std::string login_entry_for(os o, const env& e, const std::string& host_folder) {
  switch (o) {
    case os::mac:
      return e.home.empty() ? std::string()
                            : e.home + "/Library/LaunchAgents/io.github.longtimeno-c.driftwire.host.plist";
    case os::windows:
      return host_folder.empty() ? std::string() : host_folder + "\\autostart-task.xml";
    case os::linux_os: {
      const std::string base = !e.xdg_config_home.empty() && e.xdg_config_home.front() == '/'
                                   ? e.xdg_config_home
                                   : (e.home.empty() ? std::string() : e.home + "/.config");
      return base.empty() ? std::string() : base + "/systemd/user/driftwire-host.service";
    }
  }
  return {};
}

namespace {
std::mutex& bundled_m() {
  static std::mutex* m = new std::mutex();
  return *m;
}
std::string& bundled_program() {
  static std::string* p = new std::string();
  return *p;
}
}  // namespace

void set_bundled(std::string program) {
  std::lock_guard lock(bundled_m());
  bundled_program() = std::move(program);
}

std::string bundled() {
  std::lock_guard lock(bundled_m());
  return bundled_program();
}

namespace {
std::string& clip_helper_program() {
  static std::string p;
  return p;
}
}  // namespace

void set_clip_helper(std::string program) {
  std::lock_guard lock(bundled_m());
  clip_helper_program() = std::move(program);
}

std::string clip_helper() {
  std::lock_guard lock(bundled_m());
  return clip_helper_program();
}

namespace {

// The task's XML is UTF-16LE with a BOM (cli/autostart_win.cpp); the plist and the unit are UTF-8.
std::string as_utf8(const std::string& raw) {
  if (raw.size() < 2 || static_cast<unsigned char>(raw[0]) != 0xFF || static_cast<unsigned char>(raw[1]) != 0xFE) return raw;
  std::string out;
  for (std::size_t i = 2; i + 1 < raw.size(); i += 2) {
    std::uint32_t c = static_cast<unsigned char>(raw[i]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8);
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
      const std::uint32_t lo = static_cast<unsigned char>(raw[i + 2]) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i + 3])) << 8);
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::string xml_escaped(const std::string& t) {
  std::string out;
  for (char c : t) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

bool login_entry_runs(const where& w, const std::string& program) {
  if (w.login_entry.empty() || program.empty()) return false;
  const auto raw = read_small(w.login_entry);
  if (!raw) return false;
  const std::string text = as_utf8(*raw);
  return text.find(program) != std::string::npos || text.find(xml_escaped(program)) != std::string::npos;
}

where find() {
  const os o = this_os();
  const env e = read_env();
  where w;
  // The Phone access add-on's own copy first (bundled.h): the card runs it, and
  // the login entry it writes points at it. A separately installed driftwire
  // keeps its own place and its own updates; it is used only without one.
  if (const std::string b = bundled(); !b.empty() && is_file(b)) w.program = b;
  // Where the add-on installs it, also for a caller in the base app (the
  // server's menu, host_tray), which the add-on's set_bundled never reaches.
  if (w.program.empty()) {
    if (auto dir = feature_dir()) {
      const std::string b = bundled::program_in(bundled::bin_dir(*dir));
      if (is_file(b)) w.program = b;
    }
  }
  for (const auto& c : w.program.empty() ? program_candidates(o, e) : std::vector<std::string>{}) {
    if (is_file(c)) {
      w.program = c;
      break;
    }
  }
  w.data_dir = data_dir_for(o, e);
  w.login_entry = login_entry_for(o, e, join(w.data_dir, "host", o));
  return w;
}

std::optional<running_state> parse_state(std::string_view text) {
  const auto doc = json::parse(text, 6);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  running_state s;
  s.pid = doc->integer("pid").value_or(0);
  const auto port = [&](const char* k) -> std::uint16_t {
    const auto p = doc->integer(k).value_or(0);
    return p > 0 && p <= 65535 ? static_cast<std::uint16_t>(p) : std::uint16_t{0};
  };
  s.service_port = port("service_port");
  s.relay_port = port("relay_port");
  s.service_outside_port = port("service_outside_port");
  s.vpn = doc->boolean("vpn").value_or(false);
  s.awake = doc->boolean("awake").value_or(false);
  s.folders = static_cast<std::size_t>(std::max<std::int64_t>(0, doc->integer("folders").value_or(0)));
  s.offline = static_cast<std::size_t>(std::max<std::int64_t>(0, doc->integer("offline").value_or(0)));
  if (const json::value* k = doc->find("keepers"); k && k->k == json::kind::array) {
    for (const auto& line : k->a) {
      if (line.k == json::kind::string && !line.s.empty()) s.keepers.push_back(tidy_keeper_line(line.s));
    }
  }
  s.paused = doc->boolean("paused").value_or(false);
  s.tls_failed = doc->boolean("tls_failed").value_or(false);
  const auto count = [](const json::value& v, const char* k) {
    const auto n = v.integer(k).value_or(0);
    return n > 0 ? static_cast<std::uint64_t>(n) : std::uint64_t{0};
  };
  if (const json::value* c = doc->find("clients"); c && c->k == json::kind::array) {
    for (const auto& v : c->a) {
      if (v.k != json::kind::object || s.clients.size() >= 64) continue;
      const std::string* name = v.str("name");
      if (!name || name->empty()) continue;
      running_state::client cl;
      cl.name = *name;
      const std::string* via = v.str("via");
      cl.via = via && (*via == "home" || *via == "relay") ? *via : std::string("internet");
      cl.since = v.integer("since").value_or(0);
      cl.read = count(v, "read");
      cl.written = count(v, "written");
      cl.read_rate = count(v, "read_rate");
      cl.write_rate = count(v, "write_rate");
      s.clients.push_back(std::move(cl));
    }
  }
  if (const json::value* u = doc->find("usage"); u && u->k == json::kind::object) {
    if (const std::string* day = u->str("day")) s.day = *day;
    s.read_today = count(*u, "read");
    s.written_today = count(*u, "written");
    s.read_rate = count(*u, "read_rate");
    s.write_rate = count(*u, "write_rate");
    if (const json::value* d = u->find("devices"); d && d->k == json::kind::array) {
      for (const auto& v : d->a) {
        const std::string* name = v.k == json::kind::object ? v.str("name") : nullptr;
        if (name && !name->empty() && s.devices_today.size() < 16) {
          s.devices_today.push_back({*name, count(v, "read"), count(v, "written")});
        }
      }
    }
  }
  if (doc->find("drives_sets")) s.drives_health = static_cast<int>(doc->integer("drives_health").value_or(-1));
  if (const json::value* c = doc->find("convert"); c && c->k == json::kind::object) {
    s.convert_on = c->boolean("on").value_or(false);
    s.convert_available = c->boolean("available").value_or(false);
    s.convert_busy = c->boolean("busy").value_or(false);
    if (const std::string* e = c->str("encoder"); e && e->size() <= 64) s.convert_encoder = *e;
  }
  if (const json::value* k = doc->find("keepers"); k && k->k == json::kind::array) {
    for (const auto& line : k->a) {
      if (line.k != json::kind::string || !line.s.starts_with("drives ")) continue;
      // The keeper's column, then a set per line.
      std::string_view rest(line.s);
      std::size_t at = 0;
      while (at < rest.size() && s.drive_lines.size() < 8) {
        const auto end = rest.find('\n', at);
        std::string part = trim(rest.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (at == 0) part = trim(part.substr(std::string_view("drives").size()));
        if (const auto semi = part.find(';'); semi != std::string::npos) part.resize(semi);
        if (!part.empty()) s.drive_lines.push_back(part);
        if (end == std::string_view::npos) break;
        at = end + 1;
      }
    }
  }
  return s;
}

std::string tidy_keeper_line(std::string_view line) {
  std::string out;
  bool first = true;
  std::size_t at = 0;
  while (at <= line.size()) {
    const auto end = line.find('\n', at);
    std::string part = trim(line.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
    if (first) {
      // The terminal's column: a label, then two or more spaces.
      if (const auto gap = part.find("  "); gap != std::string::npos && gap > 0) {
        const auto rest = part.find_first_not_of(' ', gap);
        part = part.substr(0, gap) + ": " + (rest == std::string::npos ? std::string() : part.substr(rest));
      }
    }
    if (!part.empty()) {
      if (!out.empty()) out += '\n';
      out += part;
    }
    first = false;
    if (end == std::string_view::npos) break;
    at = end + 1;
  }
  return out;
}

std::string parse_duckdns(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return {};
  const std::string* v = doc->str("ddns");
  if (!v) return {};
  const auto sub = parse_subdomain(*v);
  return sub ? *sub : std::string();
}

host_status read_status(const where& w) {
  host_status s;
  s.installed = !w.program.empty();
  if (w.data_dir.empty()) return s;
  const std::string host = w.data_dir + "/host";  // '/' works on Windows too, as the program's own layout does
  if (const auto text = read_small(host + "/serve/serve.json")) {
    if (auto c = serve::parse_config(*text, true, true)) {
      s.set_up = !c->roots.empty();
      s.folders = c->roots.size();
      s.configured_port = c->port;
      s.name = c->name;
    }
  }
  if (const auto text = read_small(host + "/host.json")) {
    s.duckdns = parse_duckdns(*text);
    if (const auto doc = json::parse(*text, 4); doc && doc->k == json::kind::object) {
      s.convert_on = doc->boolean("convert").value_or(true);
      if (const std::string* h = doc->str("convert_helper")) s.convert_helper = *h;
    }
  }
  s.at_login = is_file(w.login_entry);
  std::error_code ec;
  if (!fs::is_directory(fs_path(host), ec)) return s;
  // Held by a running host: taken here for a moment only, as `host status`
  // does (a host starting just then tries again for a while).
  s.running = net::try_lock_instance(host + "/host.lock") == nullptr;
  if (!s.running) return s;
  if (const auto text = read_small(host + "/state.json")) {
    if (const auto st = parse_state(*text)) {
      s.service_port = st->service_port;
      s.relay_port = st->relay_port;
      s.outside_port = st->service_outside_port;
      if (st->folders) s.folders = st->folders;
      s.offline = st->offline;
      s.keepers = st->keepers;
      s.paused = st->paused;
      s.stats = host_tray::usage_lines(*st);
      s.convert_available = st->convert_available;
      s.convert_busy = st->convert_busy;
      s.convert_encoder = st->convert_encoder;
    }
  }
  return s;
}

result<ran> run_host(const where& w, std::vector<std::string> args, std::span<const std::string> input, int timeout_ms) {
  if (w.program.empty()) return err(status::not_found);
  args.insert(args.begin(), "host");
  args.insert(args.end(), w.data_args.begin(), w.data_args.end());
  io::child_process child;
  MV_TRY_VOID(child.start(w.program, args, true));  // its "why" is on stderr
  // The program's own answer comes within seconds; one that hangs (a
  // launchctl or schtasks that never returns) is ended, not waited on forever.
  std::mutex m;
  std::condition_variable cv;
  bool done = false, killed = false;
  std::thread watchdog([&] {
    std::unique_lock lock(m);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return done; })) {
      killed = true;
      child.kill();  // under the lock: the reader cannot have reached wait() yet
    }
  });
  for (const auto& line : input) {
    if (!child.write_line(line)) break;
  }
  child.close_stdin();
  ran r;
  std::string line;
  while (child.read_line(line)) {
    // `host drives list --json` names every disk and volume: room for a big machine's, not unbounded.
    if (r.output.size() < 1024 * 1024) {
      r.output += line;
      r.output += '\n';
    }
  }
  // Its output has ended: the watchdog stands down before wait() reaps the
  // child, so kill() never runs alongside wait() (nor at a reused pid).
  bool timed_out = false;
  {
    std::lock_guard lock(m);
    done = true;
    timed_out = killed;
  }
  cv.notify_all();
  watchdog.join();
  r.code = child.wait();
  if (timed_out) return err(status::timeout);
  return r;
}

std::vector<shared_folder> shared_folders(const where& w) {
  std::vector<shared_folder> out;
  if (w.data_dir.empty()) return out;
  if (const auto text = read_small(w.data_dir + "/host/serve/serve.json")) {
    if (auto c = serve::parse_config(*text, true, true)) {
      for (const auto& r : c->roots) out.push_back({r.path, r.label, r.backup});
    }
  }
  return out;
}

std::vector<std::string> share_addresses(const where& w) {
  std::vector<std::string> out;
  if (w.data_dir.empty()) return out;
  auto store = driftwire::secrets::file_store(w.data_dir + "/host/serve/secrets");
  if (!store || !store->exists("share_state")) return out;
  const auto bytes = store->get("share_state");
  if (!bytes) return out;
  const auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), 3);
  const std::string* computer = doc ? doc->str("computer") : nullptr;
  const std::string* mdns = doc ? doc->str("mdns_host") : nullptr;
  const json::value* shares = doc ? doc->find("shares") : nullptr;
  if (!computer || !mdns || !shares || shares->k != json::kind::array) return out;
  namespace su = driftwire::serve::share_users;
  for (const auto& s : shares->a) {
    if (s.k != json::kind::string) continue;
    if (auto unc = su::unc_address(*computer, s.s); !unc.empty()) out.push_back(std::move(unc));
    if (auto url = su::smb_url(*mdns, s.s); !url.empty()) out.push_back(std::move(url));
  }
  return out;
}

std::vector<std::string> apply_lines(std::span<const user_password> p) {
  std::vector<std::string> out;
  out.reserve(p.size());
  for (const auto& x : p) {
    if (x.name.empty() || x.password.empty()) continue;
    out.push_back(x.name + "\t" + x.password);
  }
  return out;
}

std::string same_path_key(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs_path(path), ec);
  if (ec) p = fs_path(path).lexically_normal();
  const std::u8string u = p.u8string();
  std::string out(reinterpret_cast<const char*>(u.data()), u.size());
  while (out.size() > 1 && (out.back() == '/' || out.back() == '\\') && !(out.size() == 3 && out[1] == ':')) out.pop_back();
#if defined(_WIN32) || defined(__APPLE__)
  // Case-insensitive file systems by default (NTFS, APFS): "Photos" is "photos".
  for (char& c : out) c = lower(c);
#endif
  return out;
}

std::string folder_label(const std::string& path) {
  std::string p = path;
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  if (p.size() == 2 && p[1] == ':') return std::string(1, p[0] >= 'a' && p[0] <= 'z' ? static_cast<char>(p[0] - 32) : p[0]);
  const auto at = p.find_last_of("/\\");
  return at == std::string::npos ? p : p.substr(at + 1);
}

std::vector<std::string> unique_labels(std::span<const folder_choice> f, std::span<const std::string> taken) {
  std::vector<std::string> used(taken.begin(), taken.end());
  const auto is_used = [&](const std::string& l) {
    for (const auto& u : used) {
      if (u.size() == l.size() && std::equal(u.begin(), u.end(), l.begin(), [](char a, char b) { return lower(a) == lower(b); })) return true;
    }
    return false;
  };
  std::vector<std::string> out;
  for (const auto& c : f) {
    const std::string base = c.label.empty() ? folder_label(c.path) : c.label;
    std::string l = base;
    for (int n = 2; is_used(l) && n < 1000; ++n) l = base + " " + std::to_string(n);
    used.push_back(l);
    out.push_back(l);
  }
  return out;
}

std::vector<folder_choice> share_plan(std::span<const folder_choice> chosen, std::span<const shared_folder> shared) {
  std::vector<folder_choice> fresh, again;
  for (const auto& f : chosen) {
    const std::string key = same_path_key(f.path);
    const auto s = std::find_if(shared.begin(), shared.end(), [&](const shared_folder& x) { return same_path_key(x.path) == key; });
    if (s == shared.end()) {
      fresh.push_back(f);
    } else if (f.backup && !s->backup) {
      again.push_back({s->path, s->label, true});  // as serve.json has it: matched even with its drive away
    }
  }
  std::vector<std::string> taken;
  for (const auto& s : shared) taken.push_back(s.label);
  const auto labels = unique_labels(fresh, taken);  // the program refuses a label in use
  for (std::size_t i = 0; i < fresh.size(); ++i) fresh[i].label = labels[i];
  fresh.insert(fresh.end(), again.begin(), again.end());
  return fresh;
}

bool answers(std::uint16_t port, int timeout_ms) {
  if (port == 0) return false;
  for (const char* loopback : {"127.0.0.1", "::1"}) {
    if (auto c = net::connect(net::endpoint{loopback, port}, timeout_ms)) return true;
  }
  return false;
}

namespace {

void write_value(json::writer& w, const json::value& v) {
  switch (v.k) {
    case json::kind::null: w.null(); break;
    case json::kind::boolean: w.boolean(v.b); break;
    case json::kind::number:
      if (v.is_integer) w.integer(v.i); else w.number(v.d);
      break;
    case json::kind::string: w.string(v.s); break;
    case json::kind::array:
      w.begin_array();
      for (const auto& e : v.a) write_value(w, e);
      w.end_array();
      break;
    case json::kind::object:
      w.begin_object();
      for (const auto& [k, e] : v.o) {
        w.key(k);
        write_value(w, e);
      }
      w.end_object();
      break;
  }
}

// Whole, then renamed over the old one: the running host never reads half a file.
bool write_replacing(const std::string& file, const std::string& text) {
  const std::string tmp = file + ".app-tmp";
  {
    std::ofstream out(fs_path(tmp), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) return false;
  }
  std::error_code ec;
  fs::rename(fs_path(tmp), fs_path(file), ec);
  if (ec) fs::remove(fs_path(tmp), ec);
  return !ec;
}

}  // namespace

std::string with_duckdns(std::string_view host_json, const std::string& subdomain, std::int64_t stamp) {
  json::writer w;
  w.begin_object();
  if (const auto doc = json::parse(host_json, 8); doc && doc->k == json::kind::object) {
    for (const auto& [k, v] : doc->o) {
      if (k == "ddns" || k == "ddns_stamp") continue;
      w.key(k);
      write_value(w, v);
    }
  }
  w.key("ddns").string(subdomain).key("ddns_stamp").integer(stamp);
  w.end_object();
  return w.take();
}

expected set_duckdns(const where& w, const std::string& subdomain, const std::string& token) {
  const auto sub = parse_subdomain(subdomain);
  const auto tok = parse_token(token);
  if (!sub || !tok || w.data_dir.empty()) return err(status::invalid_arg);
  const std::string host = w.data_dir + "/host";
  // The host's own store, as the host's keeper reads it: a folder this user's alone.
  if (!secrets::owner_only_folder(host) || !secrets::owner_only_folder(host + "/secrets")) return err(status::permission_denied);
  auto store = secrets::file_store(host + "/secrets");
  if (!store || !store->put(kTokenSecret, std::span(reinterpret_cast<const std::uint8_t*>(tok->data()), tok->size()))) {
    return err(status::io);
  }
  std::string text = "{}";
  std::error_code ec;
  if (fs::exists(fs_path(host + "/host.json"), ec)) {
    const auto there = read_small(host + "/host.json");
    // One that is there and not read, or not an object, is left for the program to say so.
    if (!there || !json::parse(*there, 8) || json::parse(*there, 8)->k != json::kind::object) return err(status::io);
    text = *there;
  }
  const auto stamp = static_cast<std::int64_t>(std::time(nullptr));
  if (!write_replacing(host + "/host.json", with_duckdns(text, *sub, stamp))) return err(status::io);
  return {};
}

std::string lan_address() {
  if (const auto gw = net::default_gateway()) {
    if (const auto a = net::local_address_toward(*gw)) return net::address_text(*a);
  }
  for (const auto& a : net::local_addresses()) {
    if (a.v6) continue;
    const auto b = a.bytes;  // IPv4 in the first four
    const bool private_range = b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168);
    if (private_range) return net::address_text(a);
  }
  return {};
}

std::vector<std::string> share_args(const std::string& path, const std::string& label, bool backup) {
  std::vector<std::string> a{"share", path};
  if (!label.empty()) a.insert(a.end(), {"--label", label});
  if (backup) a.emplace_back("--backup");
  return a;
}

std::string check_folders(std::span<const folder_choice> f) {
  if (f.empty()) return "no_folders";
  if (!library_folder(f)) return "no_library";
  const os o = this_os();
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (!absolute_path(o, f[i].path)) return "bad_path";
    for (std::size_t j = 0; j < i; ++j) {
      if (f[j].path == f[i].path) return "twice";
    }
  }
  return {};
}

const folder_choice* library_folder(std::span<const folder_choice> f) {
  for (const auto& c : f) {
    if (!c.backup) return &c;
  }
  return nullptr;
}

// The same rules as ddns::parse_subdomain and ddns::parse_token (the engine's,
// which the app does not link): the tests check both give the same answers.
std::optional<std::string> parse_subdomain(std::string_view typed) {
  std::string s;
  for (const char c : trim(typed)) s.push_back(lower(c));
  // Pasted from the browser: "https://home.duckdns.org/" is "home".
  for (const std::string_view scheme : {"https://", "http://"}) {
    if (s.starts_with(scheme)) {
      s.erase(0, scheme.size());
      if (!s.empty() && s.back() == '/') s.pop_back();
      break;
    }
  }
  if (!s.empty() && s.back() == '.') s.pop_back();
  static constexpr std::string_view kZone = ".duckdns.org";
  if (s.size() > kZone.size() && s.compare(s.size() - kZone.size(), kZone.size(), kZone) == 0) {
    s.resize(s.size() - kZone.size());
  }
  if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return std::nullopt;
  for (const char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
  }
  return s;
}

std::optional<std::string> parse_token(std::string_view typed) {
  const std::string t = trim(typed);
  if (t.size() != 36) return std::nullopt;
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = lower(t[i]);
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return std::nullopt;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return std::nullopt;
    }
    out.push_back(c);
  }
  return out;
}

std::string public_route(const std::string& subdomain, std::uint16_t port) {
  return subdomain + ".duckdns.org:" + std::to_string(port);
}

std::string host_port(const std::string& address, std::uint16_t port) {
  if (address.find(':') != std::string::npos) return "[" + address + "]:" + std::to_string(port);
  return address + ":" + std::to_string(port);
}

}  // namespace mv::phone::self_host
