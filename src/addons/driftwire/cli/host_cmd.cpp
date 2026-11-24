// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/convert_run.h"
#include "addons/driftwire/cli/host_drives.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "addons/driftwire/cli/autostart.h"
#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host_control.h"
#include "addons/driftwire/cli/host_usage.h"
#include "addons/driftwire/cli/host_setup.h"
#include "addons/driftwire/cli/host_users.h"
#include "addons/driftwire/cli/nas_update.h"
#include "addons/driftwire/cli/serve_tty.h"
#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/update.h"
#include "addons/driftwire/power.h"
#include "core/json.h"
#include "wire/grants.h"
#include "wire/known.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "wire/splice.h"

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

static_assert(host_settings{}.relay_port == relay::kDefaultPort);

namespace {

std::atomic<bool> g_stop{false};
extern "C" void on_host_signal(int) { g_stop.store(true); }

fs::path to_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

int fail(const std::string& why, int code) {
  print("driftwire host: " + why + "\n", true);
  return code;
}

std::optional<std::string> read_small(const std::string& path, std::size_t max = 256 * 1024) {
  std::ifstream f(to_path(path), std::ios::binary);
  if (!f) return std::nullopt;
  std::string text;
  char buf[4096];
  while (f.read(buf, sizeof buf) || f.gcount() > 0) {
    text.append(buf, static_cast<std::size_t>(f.gcount()));
    if (text.size() > max) return std::nullopt;
  }
  return text;
}

// Whole and renamed in, owner-only.
bool write_whole(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".part";
  {
    std::ofstream f(to_path(tmp), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.flush()) return false;
  }
#if !defined(_WIN32)
  if (::chmod(tmp.c_str(), 0600) != 0) return false;
#endif
  // Windows refuses to replace a file another process has open without
  // share-delete (the host reading serve.json every 2 s): tried for a while.
  std::error_code ec;
  for (int i = 0; i < 40; ++i) {
    fs::rename(to_path(tmp), to_path(path), ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  fs::remove(to_path(tmp), ec);
  return false;
}

// serve.json is written by `host share` and by the service (the owner's edits):
// each takes this lock around reading and writing it. A try-lock, tried for a while.
std::unique_ptr<net::instance_lock> config_lock(const host_layout& l, int wait_ms = 3000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    if (auto lock = net::try_lock_instance(l.serve + "/serve-config.lock")) return lock;
    if (std::chrono::steady_clock::now() >= until) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

serve::config new_service_config() {
  serve::config c;
  c.port = kHostServicePort;
  c.name = net::machine_name();
  return c;
}

std::string random_id() {
  std::uint8_t b[8];
  randombytes_buf(b, sizeof b);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 8; ++i) {
    out[2 * i] = kHex[b[i] >> 4];
    out[2 * i + 1] = kHex[b[i] & 15];
  }
  return out;
}

bool same_label(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

fs::path canonical_or_normal(const std::string& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(to_path(p), ec);
  if (ec) c = to_path(p).lexically_normal();
  return c;
}

// `inner` is `outer` or below it.
bool within(const fs::path& inner, const fs::path& outer) {
  auto i = inner.begin();
  for (auto o = outer.begin(); o != outer.end(); ++o, ++i) {
    if (o->empty()) continue;  // a trailing separator
    if (i == inner.end() || *i != *o) return false;
  }
  return true;
}

// A folder that is the host's own folder, holds it, or is inside it: never shared
// (its keys would be readable by phones).
bool holds_host_folder(const std::string& path, const std::string& host_folder) {
  const fs::path mine = canonical_or_normal(path);
  const fs::path keep = canonical_or_normal(host_folder);
  return within(keep, mine) || within(mine, keep);
}

std::unique_ptr<secrets::store> owner_store(const std::string& folder) {
  if (!secrets::owner_only_folder(folder)) return nullptr;
  return secrets::file_store(folder);
}

// The relay's key: made once, kept in its own store, never printed here.
std::optional<std::string> relay_key(secrets::store& store) {
  if (store.exists(serve::kRelayKeyName)) {
    auto v = store.get(serve::kRelayKeyName);
    if (!v || v->size() < relay::kMinKeyChars) return std::nullopt;  // there and unreadable: never replaced
    std::string key(v->begin(), v->end());
    sodium_memzero(v->data(), v->size());
    return key;
  }
  std::string key = relay::random_key();
  if (!store.put(serve::kRelayKeyName, std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()))) {
    return std::nullopt;
  }
  return key;
}

std::string state_json(const host_snapshot& s, const std::vector<std::string>& keeper_lines,
                       const std::vector<std::pair<std::string, std::int64_t>>& numbers) {
  json::writer w;
  w.begin_object();
#if defined(_WIN32)
  w.key("pid").integer(static_cast<std::int64_t>(::_getpid()));
#else
  w.key("pid").integer(static_cast<std::int64_t>(::getpid()));
#endif
  w.key("service_port").integer(s.service_port).key("relay_port").integer(s.relay_port);
  w.key("relay_tls_port").integer(s.relay_tls_port);
  if (s.tls_failed) w.key("tls_failed").boolean(true);
  for (const auto& [k, v] : numbers) w.key(k).integer(v);
  w.key("vpn").boolean(s.vpn).key("awake").boolean(s.awake);
  w.key("folders").integer(static_cast<std::int64_t>(s.folders)).key("offline").integer(static_cast<std::int64_t>(s.offline));
  if (s.paused) w.key("paused").boolean(true);
  w.key("convert").begin_object().key("on").boolean(s.convert_on).key("available").boolean(s.convert_available);
  if (!s.convert_encoder.empty()) w.key("encoder").string(s.convert_encoder);
  if (s.convert_busy) w.key("busy").boolean(true);
  w.end_object();
  if (!s.clients.empty()) {
    w.key("clients").begin_array();
    for (const auto& c : s.clients) {
      w.begin_object().key("name").string(c.name).key("via").string(c.via).key("since").integer(c.since);
      w.key("read").integer(static_cast<std::int64_t>(c.read)).key("written").integer(static_cast<std::int64_t>(c.written));
      w.key("read_rate").integer(static_cast<std::int64_t>(c.read_rate));
      w.key("write_rate").integer(static_cast<std::int64_t>(c.write_rate));
      w.end_object();
    }
    w.end_array();
  }
  if (!s.day.empty()) {
    w.key("usage").begin_object().key("day").string(s.day);
    w.key("read").integer(static_cast<std::int64_t>(s.read_today)).key("written").integer(static_cast<std::int64_t>(s.written_today));
    w.key("read_rate").integer(static_cast<std::int64_t>(s.read_rate));
    w.key("write_rate").integer(static_cast<std::int64_t>(s.write_rate));
    w.key("devices").begin_array();
    for (const auto& d : s.devices_today) {
      w.begin_object().key("name").string(d.name);
      w.key("read").integer(static_cast<std::int64_t>(d.read)).key("written").integer(static_cast<std::int64_t>(d.written));
      w.end_object();
    }
    w.end_array().end_object();
  }
  w.key("keepers").begin_array();
  for (const auto& line : keeper_lines) w.string(line);
  w.end_array();
  w.end_object();
  return w.take();
}

}  // namespace

// ---- host.json ------------------------------------------------------------------------------

std::optional<host_settings> parse_host_settings(std::string_view text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  host_settings s;
  if (doc->find("relay_port")) {
    const auto p = doc->integer("relay_port");
    if (!p || *p < 1 || *p > 65535) return std::nullopt;
    s.relay_port = static_cast<std::uint16_t>(*p);
  }
  if (doc->find("vpn")) {
    const auto v = doc->boolean("vpn");
    if (!v) return std::nullopt;
    s.vpn = *v;
  }
  if (doc->find("stay_awake")) {
    const auto v = doc->boolean("stay_awake");
    if (!v) return std::nullopt;
    s.stay_awake = *v;
  }
  if (doc->find("ddns")) {
    const std::string* v = doc->str("ddns");
    if (!v) return std::nullopt;
    if (!v->empty()) {
      const auto sub = ddns::parse_subdomain(*v);
      if (!sub || *sub != *v) return std::nullopt;  // kept parsed: anything else was not written here
    }
    s.ddns = *v;
  }
  if (doc->find("ddns_stamp")) {
    const auto v = doc->integer("ddns_stamp");
    if (!v) return std::nullopt;
    s.ddns_stamp = *v;
  }
  if (doc->find("tls_port")) {
    const auto p = doc->integer("tls_port");
    if (!p || *p < 0 || *p > 65535) return std::nullopt;
    s.tls_port = static_cast<std::uint16_t>(*p);
  }
  if (doc->find("domain")) {
    const std::string* v = doc->str("domain");
    if (!v) return std::nullopt;
    if (!v->empty()) {
      const auto d = parse_domain(*v);
      if (!d || *d != *v) return std::nullopt;
    }
    s.domain = *v;
  }
  if (doc->find("look")) {
    const auto v = doc->boolean("look");
    if (!v) return std::nullopt;
    s.look = *v;
  }
  if (doc->find("library_id")) {
    const std::string* v = doc->str("library_id");
    if (!v || v->size() > 64 ||
        v->find_first_not_of("0123456789abcdef") != std::string::npos) {
      return std::nullopt;
    }
    s.library_id = *v;
  }
  if (doc->find("paused")) {
    const auto v = doc->boolean("paused");
    if (!v) return std::nullopt;
    s.paused = *v;
  }
  if (doc->find("quit_stamp")) {
    const auto v = doc->integer("quit_stamp");
    if (!v || *v < 0) return std::nullopt;
    s.quit_stamp = *v;
  }
  if (doc->find("convert")) {
    const auto v = doc->boolean("convert");
    if (!v) return std::nullopt;
    s.convert = *v;
  }
  if (doc->find("convert_helper")) {
    const std::string* v = doc->str("convert_helper");
    if (!v || v->size() > 4096) return std::nullopt;
    s.convert_helper = *v;
  }
  return s;
}

std::uint16_t look_port(const host_settings& s) noexcept {
  if (!s.vpn || !s.look) return 0;
  if (s.tls_port != 0) return s.tls_port;
  return static_cast<std::uint16_t>(s.relay_port < 65535 ? s.relay_port + 1 : s.relay_port - 1);
}

std::string vpn_name(const host_settings& s) {
  if (!s.domain.empty()) return s.domain;
  return s.ddns.empty() ? std::string() : ddns::duckdns_name(s.ddns);
}

std::optional<std::string> parse_domain(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.remove_suffix(1);
  for (std::string_view scheme : {"https://", "http://"}) {
    if (t.substr(0, scheme.size()) == scheme) t.remove_prefix(scheme.size());
  }
  if (!t.empty() && t.back() == '/') t.remove_suffix(1);
  if (!t.empty() && t.back() == '.') t.remove_suffix(1);
  if (t.empty() || t.size() > 253) return std::nullopt;
  std::string out;
  int labels = 0;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dot = t.find('.', start);
    const std::string_view label = t.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return std::nullopt;
    for (const char ch : label) {
      const char c = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
      out += c;
    }
    ++labels;
    if (dot == std::string_view::npos) break;
    out += '.';
    start = dot + 1;
  }
  if (labels < 2) return std::nullopt;
  return out;
}

std::string host_settings_json(const host_settings& s) {
  json::writer w;
  w.begin_object().key("relay_port").integer(s.relay_port).key("vpn").boolean(s.vpn).key("stay_awake").boolean(s.stay_awake);
  if (!s.ddns.empty()) w.key("ddns").string(s.ddns);
  if (s.ddns_stamp != 0) w.key("ddns_stamp").integer(s.ddns_stamp);
  if (s.tls_port != 0) w.key("tls_port").integer(s.tls_port);
  if (!s.look) w.key("look").boolean(false);
  if (!s.domain.empty()) w.key("domain").string(s.domain);
  if (!s.library_id.empty()) w.key("library_id").string(s.library_id);
  if (s.paused) w.key("paused").boolean(true);
  if (s.quit_stamp != 0) w.key("quit_stamp").integer(s.quit_stamp);
  if (!s.convert) w.key("convert").boolean(false);
  if (!s.convert_helper.empty()) w.key("convert_helper").string(s.convert_helper);
  w.end_object();
  return w.take();
}

host_layout host_layout_of(const std::string& data_dir) {
  host_layout l;
  l.folder = data_dir + "/host";
  l.serve = l.folder + "/serve";
  l.serve_config = l.serve + "/serve.json";
  l.relay = l.folder + "/relay";
  l.settings = l.folder + "/host.json";
  l.state = l.folder + "/state.json";
  l.lock = l.folder + "/host.lock";
  return l;
}

std::string host_catalogue_of(const host_layout& l) {
  const auto slash = l.folder.find_last_of("/\\");
  const std::string data = slash == std::string::npos ? std::string(".") : l.folder.substr(0, slash);
  return data + "/library/" + serve::kCatalogueFolder;
}

bool make_host_folders(const host_layout& l) {
  return secrets::owner_only_folder(l.folder) && secrets::owner_only_folder(l.serve) &&
         secrets::owner_only_folder(l.relay);
}

// ---- what is shared -------------------------------------------------------------------------

std::string default_label(const std::string& abs_path) {
  std::string p = abs_path;
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  if (p.size() == 2 && p[1] == ':') return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(p[0]))));
  const auto slash = p.find_last_of("/\\");
  std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
  if (name.empty()) name = "Computer";
  if (name.size() > serve::kMaxLabelBytes) name.resize(serve::kMaxLabelBytes);  // may cut a character: shown, never parsed
  return name;
}

std::string share_folder(serve::config& c, const std::string& abs_path, const std::string& label, bool backup,
                         const volume_facts& facts, const std::string& id, const std::string& host_folder) {
  if (label.empty() || label.size() > serve::kMaxLabelBytes) return "a label is 1 to 64 bytes (--label).";
  for (const unsigned char ch : label) {
    if (ch < 0x20 || ch == 0x7f) return "a label has no control characters.";
  }
  if (c.roots.size() >= serve::kMaxRoots) return "32 folders are shared already; unshare one first.";
  const fs::path mine = canonical_or_normal(abs_path);
  if (holds_host_folder(abs_path, host_folder)) {
    return "that folder holds the host's own keys; share a folder that does not hold " +
           to_utf8(canonical_or_normal(host_folder)) + ".";
  }
  for (const auto& r : c.roots) {
    if (same_label(r.label, label)) return "a folder is shared as \"" + r.label + "\" already (give another --label).";
    if (canonical_or_normal(r.path) == mine) return "that folder is shared already, as \"" + r.label + "\".";
  }
  serve::root_config rc;
  rc.id = id;
  rc.label = label;
  rc.path = abs_path;
  rc.mount = facts.mount;
  rc.volume = facts.volume;
  if (backup) {
    rc.backup = true;
    rc.limit = static_cast<std::uint64_t>(INT64_MAX);  // no limit of its own: the reserve keeps the drive from filling
    rc.reserve = serve::kDefaultReserve;
  }
  c.roots.push_back(std::move(rc));
  return {};
}

std::string unshare_folder(serve::config& c, std::string_view label, std::string_view host_catalogue) {
  const auto before = c.roots.size();
  if (same_label(label, serve::kCatalogueLabel)) return "the Catalogue goes with the last folder shared.";
  std::erase_if(c.roots, [&](const serve::root_config& r) { return same_label(r.label, label); });
  if (c.roots.size() == before) return "no folder is shared as \"" + std::string(label) + "\".";
  // The host library's Catalogue names no folder of the person's: it goes with the last one
  // (a desktop's claimed Catalogue stays: that library is the desktop's).
  if (!host_catalogue.empty() &&
      std::all_of(c.roots.begin(), c.roots.end(),
                  [](const serve::root_config& r) { return r.label == serve::kCatalogueLabel; })) {
    std::erase_if(c.roots, [&](const serve::root_config& r) { return r.path == host_catalogue; });
  }
  return {};
}

after_child decide_after_child(const child_process::ended& e, bool stopping) {
  if (stopping) return after_child::stop;
  if (!e.crashed && e.code == kHostChildCannotRun) return after_child::fail;
  if (!e.crashed && e.code == kHostChildQuit) return after_child::stop;  // `host quit`
  return after_child::restart;  // a crash, a failure, or an end nobody asked for
}

// ---- the service process --------------------------------------------------------------------

struct host_service::state {
  host_service_options o;
  std::unique_ptr<secrets::store> serve_store, relay_store, keeper_store;
  std::unique_ptr<net::instance_lock> serve_lock;
  std::string relay_secret;

  // serve.json's text: as the loop last took it, and as the service last wrote
  // it (the owner's edits). Held only to read or set these, never around a call
  // into the service: a session writing an edit waits on nothing the loop holds.
  std::mutex text_m;
  std::string cfg_text;      // under text_m
  std::string service_text;  // under text_m

  std::atomic<bool> address_moved{false};
  std::atomic<bool> cert_changed{false};  // the certificate keeper's news

  mutable std::mutex m;  // everything below; the loop's, and snapshot()'s
  host_settings settings;
  std::string settings_text;
  serve::config cfg;
  std::unique_ptr<serve::server> srv;
  std::unique_ptr<relay::server> rel;
  std::unique_ptr<power::stay_awake> awake;
  // Converting (docs/plans/phone-clip-convert.md): the service, and what was checked last.
  std::shared_ptr<serve::convert::service> conv;
  std::string conv_helper;  // the helper `conv` (or the failed check) was made for
  bool conv_checked = false;
  convert_check conv_found;
  std::chrono::steady_clock::time_point serve_retry{}, relay_retry{}, next_roots{};
  std::size_t offline = 0;
  bool tls_failed = false;
  bool said_bad_config = false, said_serve_failed = false, said_relay_failed = false;
  std::vector<std::unique_ptr<host_keeper>> keepers;
  // The drives keeper's word for the phones (serve's options::storage), apart
  // from `m`: serve asks it on a session's thread, the keeper sets it on its own.
  mutable std::mutex storage_m;
  std::string storage;
  std::string state_written;
  bool started = false;  // state.json is this process's to remove
  // The tray (docs/plans/self-hosted.md "Tray"): when this service started
  // (a `host quit` stamped later stops it), and the connected phones' names.
  std::int64_t started_ms = 0;
  std::atomic<bool> quit{false};
  std::vector<host_snapshot::client> clients;
  usage_book usage;           // today's bytes (host_usage.h), kept in usage.json
  std::string usage_written;  // usage.json's text as last written
  std::map<std::array<std::uint8_t, 32>, std::string> phone_names;  // the paired list, as last read
  std::chrono::steady_clock::time_point names_read{};

  std::mutex wake_m;
  std::condition_variable wake;
  bool stopping = false;  // under wake_m
  std::thread loop;

  [[nodiscard]] keeper_context keeper_ctx_locked() {
    keeper_context k;
    k.folder = o.where.folder;
    k.secrets = keeper_store.get();
    k.service_port = srv ? srv->port() : 0;
    k.relay_port = rel ? rel->port() : 0;
    k.relay_tls_port = rel ? rel->tls_port() : 0;
    k.vpn = settings.vpn;
    k.ddns_subdomain = settings.ddns;
    k.cert_name = vpn_name(settings);
    k.loopback_only = o.loopback_only;
    k.cert_changed = [this] {
      cert_changed.store(true);
      {
        std::lock_guard lock(wake_m);
      }
      wake.notify_all();
    };
    k.address_moved = [this] {
      // Any thread, even inside a keeper's own call: the loop passes it on.
      address_moved.store(true);
      {
        std::lock_guard lock(wake_m);
      }
      wake.notify_all();
    };
    k.storage_changed = [this](std::string word) {
      std::lock_guard g(storage_m);
      storage = std::move(word);
    };
    return k;
  }

  void ports_changed_locked() {
    const keeper_context k = keeper_ctx_locked();
    for (auto& kp : keepers) kp->ports_changed(k);
  }

  // serve's save_config (a session's thread): the owner's edits, written over
  // the file only if nobody else changed it since. The loop takes the new text
  // as any other change of the file.
  bool save_from_service(const serve::config& next) {
    // The owner's add_root never shares the host's own keys either.
    for (const auto& r : next.roots) {
      if (holds_host_folder(r.path, o.where.folder)) return false;
    }
    const auto lock = config_lock(o.where, 1000);
    if (!lock) return false;
    const std::string disk = read_small(o.where.serve_config).value_or(std::string());
    std::lock_guard g(text_m);
    if (disk != cfg_text && disk != service_text) return false;  // a `host share` got there first: the desktop tries again
    const std::string text = serve::config_json(next);
    if (!write_whole(o.where.serve_config, text)) return false;
    service_text = text;
    return true;
  }

  [[nodiscard]] std::string loaded_text() {
    std::lock_guard g(text_m);
    return cfg_text;
  }
  void set_loaded_text(std::string t) {
    std::lock_guard g(text_m);
    cfg_text = std::move(t);
  }

  void start_serve_locked() {
    serve::server::options so;
    so.cfg = cfg;
    so.store = serve_store.get();
    so.loopback_only = o.loopback_only;
    so.claimable = true;
    so.devices_lock = o.where.serve + "/serve-devices.lock";
    so.save_config = [this](const serve::config& c) { return save_from_service(c); };
    so.root_present = o.present;
    so.storage = [this] {
      std::lock_guard g(storage_m);
      return storage;
    };
    so.converter = conv;
    auto s = serve::server::start(so);
    if (!s) {
      serve_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.retry_ms);
      if (!said_serve_failed) {
        print(std::string("driftwire host: the service could not listen on port ") + std::to_string(cfg.port) +
                  "; trying again every 30 s\n",
              true);
        said_serve_failed = true;
      }
      return;
    }
    said_serve_failed = false;
    srv = std::move(*s);
    print("driftwire host: serving " + std::to_string(cfg.roots.size()) + " folders on port " +
          std::to_string(srv->port()) + "\n");
  }

  void stop_serve_locked() {
    if (!srv) return;
    fold_usage_locked();
    srv->stop();
    srv.reset();
    print("driftwire host: no folder shared; the service stopped\n");
  }

  void start_relay_locked() {
    relay::server::options ro;
    ro.key = relay_secret;
    ro.port = settings.relay_port;
    ro.loopback_only = o.loopback_only;
    if (!settings.vpn) {
      ro.forward_ports.clear();  // forwarding (the Tunnel/VPN exit) only with `host vpn on`
      ro.forward_udp_ports.clear();
    } else {
      relay::vpn_host_defaults(ro);  // all traffic, never into the home's networks
      // Nor to the home's outside address (the router's, which hairpins to its forwarded ports).
      // Both names the home goes by: the DuckDNS one and the own domain (vpn-stealth), either may be set.
      std::vector<std::function<bool(const net::address&)>> names;
      if (!settings.ddns.empty()) names.push_back(relay::refuse_name_addresses(ddns::duckdns_name(settings.ddns)));
      if (!settings.domain.empty()) names.push_back(relay::refuse_name_addresses(settings.domain));
      if (names.size() == 1) {
        ro.forward_refused = std::move(names.front());
      } else if (!names.empty()) {
        ro.forward_refused = [names = std::move(names)](const net::address& a) {
          for (const auto& f : names) {
            if (f(a)) return true;
          }
          return false;
        };
      }
    }
    if (look_port(settings) != 0) {
      // The web-site look (on by default with the VPN; docs/plans/vpn-stealth.md): Let's
      // Encrypt's certificate for the name when the certificate keeper has one, else self-signed.
      ro.tls_port = look_port(settings);
      const std::string name = vpn_name(settings);
      ro.tls_name = name.empty() ? std::string("localhost") : name;
      if (const auto c = usable_certificate(o.where.folder, name, static_cast<std::int64_t>(std::time(nullptr)))) {
        ro.tls_cert = c->cert;
        ro.tls_key = c->key;
      }
    }
    // Never a private, loopback or link-local address (call 5: no VPN into the LAN).
    if (o.relay_tweak) o.relay_tweak(ro);
    const bool with_tls = ro.tls_port != 0;
    auto r = relay::server::start(ro);
    if (!r && !ro.tls_cert.empty()) {
      // Let's Encrypt's files would not load: the look self-signed rather than no look.
      ro.tls_cert.clear();
      ro.tls_key.clear();
      r = relay::server::start(ro);
    }
    tls_failed = false;
    if (!r && with_tls) {
      // The look's port taken or not allowed (443 on Linux without privilege): the relay and the
      // VPN run without it rather than not at all.
      ro.tls_port = 0;
      r = relay::server::start(std::move(ro));
      if (r) {
        tls_failed = true;
        print("driftwire host: the web-site look could not listen on port " + std::to_string(look_port(settings)) +
                  "; the relay runs without it\n",
              true);
      }
    }
    if (!r) {
      relay_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.retry_ms);
      if (!said_relay_failed) {
        print("driftwire host: the relay could not listen on port " + std::to_string(settings.relay_port) +
                  "; trying again every 30 s\n",
              true);
        said_relay_failed = true;
      }
      return;
    }
    said_relay_failed = false;
    rel = std::move(*r);
    print("driftwire host: relay on port " + std::to_string(rel->port()) + (settings.vpn ? ", VPN on" : ", VPN off") +
          (rel->tls_port() ? ", web-site look on port " + std::to_string(rel->tls_port()) + "\n" : std::string("\n")));
  }

  void set_awake_locked() {
    const bool want = o.power && settings.stay_awake && !settings.paused;
    if (want && !awake) {
      awake = power::hold_for(kHostReason);
    } else if (!want) {
      awake.reset();
    }
  }

  // serve.json again: the folders now, as the service takes them.
  void reload_config_locked() {
    const auto text = read_small(o.where.serve_config);
    if (!text) {
      if (!loaded_text().empty() || srv) {
        set_loaded_text({});
        cfg = serve::config{};
        stop_serve_locked();
        ports_changed_locked();
      }
      return;
    }
    if (*text == loaded_text()) return;
    auto parsed = serve::parse_config(*text, o.any_port, true);
    if (!parsed) {
      if (!said_bad_config) print("driftwire host: serve.json is not valid; the folders stay as they were\n", true);
      said_bad_config = true;
      return;
    }
    said_bad_config = false;
    const bool port_changed = parsed->port != cfg.port;
    cfg = std::move(*parsed);
    bool from_service = false;
    {
      std::lock_guard g(text_m);
      cfg_text = *text;
      from_service = *text == service_text;
    }
    // The service's own write: it has that config already, and may have a
    // newer one by now that taking this one would undo.
    if (from_service && srv && !port_changed) return;
    if (cfg.roots.empty()) {
      stop_serve_locked();
    } else if (srv && !port_changed) {
      (void)srv->replace_config(cfg);
    } else if (!settings.paused) {
      if (srv) {
        fold_usage_locked();
        srv->stop();
        srv.reset();
      }
      start_serve_locked();
    }
    ports_changed_locked();
  }

  void reload_settings_locked() {
    const auto text = read_small(o.where.settings, 16 * 1024);
    const std::string t = text.value_or(std::string());
    if (t == settings_text) return;
    settings_text = t;
    const host_settings next = text ? parse_host_settings(*text).value_or(settings) : host_settings{};
    const bool relay_changed = next.vpn != settings.vpn || next.relay_port != settings.relay_port ||
                               look_port(next) != look_port(settings) ||
                               (next.vpn && (next.ddns != settings.ddns || next.domain != settings.domain));
    const bool name_changed = next.ddns != settings.ddns || next.ddns_stamp != settings.ddns_stamp ||
                              next.domain != settings.domain;
    const bool pause_changed = next.paused != settings.paused;
    if (next.quit_stamp != 0 && next.quit_stamp >= started_ms && !quit.exchange(true)) {
      print("driftwire host: asked to quit\n");
    }
    settings = next;
    apply_convert_locked();
    if (pause_changed) {
      // Paused: nothing for phones or the laptop, and the computer may sleep;
      // the keepers hear the ports are gone (the router's mapping goes).
      if (settings.paused) {
        if (srv) {
          fold_usage_locked();
          srv->stop();
          srv.reset();
        }
        rel.reset();
        print("driftwire host: paused\n");
      } else {
        start_relay_locked();
        if (!cfg.roots.empty()) start_serve_locked();
        print("driftwire host: resumed\n");
      }
      ports_changed_locked();
    } else if (relay_changed && rel) {
      rel.reset();
      start_relay_locked();
      ports_changed_locked();
    } else if (name_changed) {
      ports_changed_locked();  // the DuckDNS keeper takes the new name (and token)
    }
    set_awake_locked();
  }

  // On with a helper: checked once per helper, then given to the service. Off: gone, and the
  // cache emptied (what was converted is not kept for a feature turned off).
  void apply_convert_locked() {
    const bool want = settings.convert && !settings.convert_helper.empty();
    if (!want) {
      if (conv) {
        if (srv) srv->set_converter(nullptr);
        conv->purge();
        conv.reset();
      }
      conv_checked = false;
      conv_helper.clear();
      return;
    }
    if (conv_checked && conv_helper == settings.convert_helper) return;
    if (srv) srv->set_converter(nullptr);
    conv.reset();
    conv_helper = settings.convert_helper;
    conv_checked = true;
    conv = start_converter(conv_helper, o.where.serve + "/convert-cache", &conv_found);
    if (srv) srv->set_converter(conv);
    print(conv ? "driftwire host: converting clips for phones with " + conv_found.family + "\n"
               : std::string("driftwire host: no hardware video encoder found; phones keep the download\n"));
  }

  // Each folder's drive: offline counted, a new letter taken (and written).
  void recheck_roots_locked() {
    std::size_t off = 0;
    bool moved = false;
    serve::config next = cfg;
    for (auto& r : next.roots) {
      const auto at = o.locate(r);
      if (!at) {
        ++off;
        continue;
      }
      std::error_code dec;
      if (*at != r.path && fs::is_directory(to_path(*at), dec)) {
        r.path = *at;
        moved = true;
      }
      std::error_code ec;
      if (!fs::is_directory(to_path(r.path), ec) || !o.present(r)) ++off;
    }
    offline = off;
    if (!moved) return;
    // Written under the file's lock (one try: the loop holds `m`, which a
    // waiting writer never needs), and only over what this process read.
    const auto lock = config_lock(o.where, 0);
    if (!lock) return;  // next minute
    if (read_small(o.where.serve_config).value_or(std::string()) != loaded_text()) return;  // reloaded first, next pass
    const std::string text = serve::config_json(next);
    if (!write_whole(o.where.serve_config, text)) return;
    cfg = std::move(next);
    set_loaded_text(text);
    if (srv) (void)srv->replace_config(cfg);
    print("driftwire host: a shared folder's drive is back under another letter\n");
  }

  void retry_locked() {
    if (settings.paused) return;
    const auto now = std::chrono::steady_clock::now();
    if (!srv && !cfg.roots.empty() && now >= serve_retry) {
      start_serve_locked();
      if (srv) ports_changed_locked();
    }
    if (!rel && now >= relay_retry) {
      start_relay_locked();
      if (rel) ports_changed_locked();
    }
  }

  host_snapshot snapshot_locked() const {
    host_snapshot s;
    s.service_port = srv ? srv->port() : 0;
    s.relay_port = rel ? rel->port() : 0;
    s.relay_tls_port = rel ? rel->tls_port() : 0;
    s.tls_failed = rel && tls_failed;
    s.vpn = settings.vpn;
    s.awake = awake != nullptr;
    s.folders = cfg.roots.size();
    s.offline = offline;
    s.paused = settings.paused;
    s.convert_on = settings.convert;
    s.convert_available = conv != nullptr;
    s.convert_encoder = conv ? conv_found.family : std::string();
    s.convert_busy = conv && conv->now().busy;
    s.clients = clients;
    s.day = usage.day;
    s.read_today = usage.total.read;
    s.written_today = usage.total.written;
    s.read_rate = usage.total_rate.read;
    s.write_rate = usage.total_rate.written;
    for (const auto& [k, b] : usage.today) {
      if (s.devices_today.size() >= 16) break;
      const auto it = phone_names.find(k);
      s.devices_today.push_back({it != phone_names.end() && !it->second.empty() ? it->second : std::string("A phone"),
                                 b.read, b.written});
    }
    return s;
  }

  // "Tray": before the service stops, what it counted goes into today's book.
  void fold_usage_locked() {
    if (srv) usage.service_gone(srv->usage(), local_day(std::chrono::system_clock::now()), std::chrono::steady_clock::now());
  }

  // "Tray": the service's per-device bytes into today's book, the phones up now
  // by the paired list's names. The list is read again only when a device not
  // on it shows (at most every 10 s); usage.json only when today's totals moved.
  void refresh_usage_locked() {
    const auto now = srv ? srv->usage() : std::vector<serve::server::device_usage>{};
    usage.take(now, local_day(std::chrono::system_clock::now()), std::chrono::steady_clock::now());
    const bool unknown = std::any_of(now.begin(), now.end(), [&](const auto& u) { return !phone_names.contains(u.device); });
    const auto t = std::chrono::steady_clock::now();
    // Unknown: at most every 10 s; a rename in the app: within a minute while any is there.
    const bool due = names_read == std::chrono::steady_clock::time_point{} ||
                     t - names_read >= (unknown ? std::chrono::seconds(10) : std::chrono::seconds(60));
    if (!now.empty() && serve_store && due && (unknown || t - names_read >= std::chrono::seconds(60))) {
      names_read = t;
      phone_names.clear();
      for (const auto& d : known::load(*serve_store)) phone_names[d.key] = d.name;
      std::vector<serve::grants::grant> limited;
      (void)serve::grants::load(*serve_store, limited);
      for (const auto& g : limited) phone_names[g.key] = g.name;
    }
    std::vector<host_snapshot::client> up;
    for (const auto& u : now) {
      if (!u.connected) continue;
      host_snapshot::client c;
      const auto it = phone_names.find(u.device);
      c.name = it != phone_names.end() && !it->second.empty() ? it->second : std::string("A phone");
      c.via = u.via == serve::server::usage_via::home ? "home" : u.via == serve::server::usage_via::relay ? "relay" : "internet";
      c.since = u.since_unix;
      if (auto d = usage.today.find(u.device); d != usage.today.end()) {
        c.read = d->second.read;
        c.written = d->second.written;
      }
      if (auto r = usage.rate.find(u.device); r != usage.rate.end()) {
        c.read_rate = r->second.read;
        c.write_rate = r->second.written;
      }
      up.push_back(std::move(c));
    }
    std::sort(up.begin(), up.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    clients = std::move(up);
    const std::string text = usage.to_json();
    if (text != usage_written && write_whole(o.where.folder + "/usage.json", text)) usage_written = text;
  }

  void write_state_locked() {
    refresh_usage_locked();
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, std::int64_t>> numbers;
    for (const auto& kp : keepers) {
      if (std::string line = kp->status_line(); !line.empty()) lines.push_back(std::move(line));
      for (auto& n : kp->state_numbers()) numbers.push_back(std::move(n));
    }
    const std::string text = state_json(snapshot_locked(), lines, numbers);
    if (text == state_written) return;
    if (write_whole(o.where.state, text)) state_written = text;
  }

  void pass(bool roots_now) {
    std::lock_guard lock(m);
    if (address_moved.exchange(false)) {
      for (auto& kp : keepers) kp->address_may_have_moved();
    }
    if (cert_changed.exchange(false) && rel && rel->tls_port() != 0) {
      // A renewal: new connections take it, a laptop's open forwards keep theirs.
      if (const auto c = usable_certificate(o.where.folder, vpn_name(settings),
                                            static_cast<std::int64_t>(std::time(nullptr)))) {
        if (rel->set_tls_certificate(c->cert, c->key)) print("driftwire host: the web-site look took the new certificate\n");
      }
    }
    reload_settings_locked();
    reload_config_locked();
    retry_locked();
    const auto now = std::chrono::steady_clock::now();
    if (roots_now || now >= next_roots) {
      recheck_roots_locked();
      next_roots = now + std::chrono::milliseconds(o.roots_every_ms);
    }
    write_state_locked();
  }

  void run_loop() {
    std::unique_lock lock(wake_m);
    while (!stopping) {
      wake.wait_for(lock, std::chrono::milliseconds(o.config_every_ms),
                    [this] { return stopping || address_moved.load() || cert_changed.load(); });
      if (stopping) break;
      lock.unlock();
      pass(false);
      lock.lock();
    }
  }
};

std::unique_ptr<host_service> host_service::start(host_service_options o, std::string& why) {
  if (sodium_init() < 0) {
    why = "could not start its cryptography.";
    return nullptr;
  }
  if (!make_host_folders(o.where)) {
    why = "its folder must be this user's alone (mode 700): " + o.where.folder;
    return nullptr;
  }
  if (!o.present) o.present = [](const serve::root_config& r) { return volume_present(r); };
  if (!o.locate) o.locate = [](const serve::root_config& r) { return volume_locate(r); };
  std::unique_ptr<host_service> hs(new host_service());
  hs->st_ = std::make_unique<state>();
  state& st = *hs->st_;
  st.o = std::move(o);
  st.serve_store = owner_store(st.o.where.serve + "/secrets");
  st.relay_store = owner_store(st.o.where.relay + "/secrets");
  st.keeper_store = owner_store(st.o.where.folder + "/secrets");
  if (!st.serve_store || !st.relay_store || !st.keeper_store) {
    why = "its secrets folders must be this user's alone (mode 700).";
    return nullptr;
  }
  // One service per data folder: a `driftwire serve --data <host>/serve` waits its turn.
  st.serve_lock = net::try_lock_instance(st.o.where.serve + "/serve.lock");
  if (!st.serve_lock) {
    why = "a service is already running in its folder.";
    return nullptr;
  }
  std::array<std::uint8_t, 32> pub{}, key{};
  bool made = false;
  if (!serve::set_up(*st.serve_store, pub, key, made)) {
    why = "the service's keys cannot be read or made.";
    return nullptr;
  }
  sodium_memzero(key.data(), key.size());  // shown by `host setup` (slice 5), never logged
  const auto rk = relay_key(*st.relay_store);
  if (!rk) {
    why = "the relay's key cannot be read or made.";
    return nullptr;
  }
  st.relay_secret = *rk;
  if (const auto text = read_small(st.o.where.folder + "/usage.json", 64 * 1024)) {
    if (auto b = usage_book::from_json(*text)) {
      st.usage = std::move(*b);
      st.usage_written = *text;
    }
  }
  st.started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
  if (st.o.quit_since_ms > 0 && st.o.quit_since_ms < st.started_ms) st.started_ms = st.o.quit_since_ms;
  {
    std::lock_guard lock(st.m);
    st.settings_text = "\x01";  // never a file's text: read the first time
    st.reload_settings_locked();
    if (!st.rel && !st.settings.paused) st.start_relay_locked();
    st.reload_config_locked();
    st.recheck_roots_locked();
    st.next_roots = std::chrono::steady_clock::now() + std::chrono::milliseconds(st.o.roots_every_ms);
    st.set_awake_locked();
    // The keepers last: they need the ports.
    for (const auto& make : {st.o.keepers.ddns, st.o.keepers.router, st.o.keepers.drives, st.o.keepers.cert,
                             st.o.keepers.copy}) {
      if (!make) continue;
      if (auto kp = make()) st.keepers.push_back(std::move(kp));
    }
    const keeper_context k = st.keeper_ctx_locked();
    for (auto& kp : st.keepers) kp->start(k);
    st.write_state_locked();
  }
  st.started = true;
  st.loop = std::thread([&st] { st.run_loop(); });
  return hs;
}

host_service::~host_service() { stop(); }

host_snapshot host_service::snapshot() const {
  std::lock_guard lock(st_->m);
  return st_->snapshot_locked();
}

void host_service::recheck_now() { st_->pass(true); }

bool host_service::quit_asked() const noexcept { return st_ && st_->quit.load(); }

void host_service::stop() noexcept {
  if (!st_) return;
  {
    std::lock_guard lock(st_->wake_m);
    st_->stopping = true;
  }
  st_->wake.notify_all();
  if (st_->loop.joinable()) st_->loop.join();
  // The keepers first (a router mapping goes before the port it opened), then the services.
  std::vector<std::unique_ptr<host_keeper>> keepers;
  {
    std::lock_guard lock(st_->m);
    keepers.swap(st_->keepers);
  }
  for (auto& kp : keepers) kp->stop();
  keepers.clear();
  std::lock_guard lock(st_->m);
  if (st_->srv) {
    // "Tray": its last bytes kept for today (usage.json) before it goes.
    st_->fold_usage_locked();
    if (const std::string text = st_->usage.to_json(); text != st_->usage_written) {
      (void)write_whole(st_->o.where.folder + "/usage.json", text);
    }
    st_->srv->stop();
    st_->srv.reset();
  }
  st_->rel.reset();
  st_->awake.reset();
  std::error_code ec;
  if (st_->started) fs::remove(to_path(st_->o.where.state), ec);  // never a running host's
  st_->started = false;
  if (!st_->relay_secret.empty()) sodium_memzero(st_->relay_secret.data(), st_->relay_secret.size());
  st_->relay_secret.clear();
}

// ---- the commands ---------------------------------------------------------------------------

namespace {

std::string data_folder_of(const args& a) {
  if (!a.data.empty()) return a.data;
  if (!a.test_data.empty()) return a.test_data;
  return data_dir();
}

std::string absolute_of(const std::string& arg) {
  std::string s = arg;
  const std::string home = home_dir();
  if (!home.empty() && (s == "~" || s.rfind("~/", 0) == 0 || s.rfind("~\\", 0) == 0)) s = home + s.substr(1);
  std::error_code ec;
  fs::path p = fs::absolute(to_path(s), ec);
  if (ec) p = to_path(s);
  std::string out = to_utf8(p.lexically_normal());
  while (out.size() > 1 && (out.back() == '/' || out.back() == '\\') && !(out.size() == 3 && out[1] == ':')) out.pop_back();
  return out;
}

// serve.json as it is, or a new one with no folders; nullopt when it is there and not valid.
std::optional<serve::config> load_or_new(const host_layout& l, bool any_port) {
  const auto text = read_small(l.serve_config);
  if (!text) return new_service_config();
  auto c = serve::parse_config(*text, any_port, true);
  if (!c) return std::nullopt;
  return std::move(*c);
}

std::string share_path(const host_layout& l, const std::string& typed, const std::string& label_arg, bool backup,
                       std::uint64_t limit, bool test_hooks, std::string& label) {
  const std::string path = absolute_of(typed);
  std::error_code ec;
  if (!fs::is_directory(to_path(path), ec)) return "not a folder: " + typed;
  if (!make_host_folders(l)) return "its folder must be this user's alone (mode 700).";
  const auto lock = config_lock(l);
  if (!lock) return "serve.json is being changed by something else; try again.";
  auto cfg = load_or_new(l, test_hooks);
  if (!cfg) return "serve.json is not valid; not changing it.";
  label = label_arg.empty() ? default_label(path) : label_arg;
  std::string why = share_folder(*cfg, path, label, backup, volume_of(path), random_id(), l.folder);
  if (!why.empty()) return why;
  if (backup && limit != 0) cfg->roots.back().limit = limit;
  if (!write_whole(l.serve_config, serve::config_json(*cfg))) return "could not write serve.json.";
  return {};
}

int run_share(const args& a, const host_layout& l, bool test_hooks) {
  std::string label;
  const std::string why = share_path(l, a.host_path, a.label, a.backup, 0, test_hooks, label);
  if (!why.empty()) return fail(why, 1);
  print("Shared \"" + label + "\"" + (a.backup ? ", phones may back up into it" : std::string()) +
        ". A running host serves it within seconds.\n");
  return 0;
}

int run_unshare(const args& a, const host_layout& l, bool test_hooks) {
  const auto lock = config_lock(l);
  if (!lock) return fail("serve.json is being changed by something else; try again.", 1);
  auto cfg = load_or_new(l, test_hooks);
  if (!cfg) return fail("serve.json is not valid; not changing it.", 1);
  const std::string why = unshare_folder(*cfg, a.label, host_catalogue_of(l));
  if (!why.empty()) return fail(why, 1);
  if (!write_whole(l.serve_config, serve::config_json(*cfg))) return fail("could not write serve.json.", 1);
  print("\"" + a.label + "\" is no longer shared. Its files stay where they are.\n");
  return 0;
}

host_settings load_settings(const host_layout& l) {
  const auto text = read_small(l.settings, 16 * 1024);
  return text ? parse_host_settings(*text).value_or(host_settings{}) : host_settings{};
}

int run_host_convert(const args& a, const host_layout& l) {
  if (!make_host_folders(l)) return fail("its folder must be this user's alone (mode 700).", 1);
  host_settings s = load_settings(l);
  s.convert = a.on;
  if (a.on && !a.convert_helper.empty()) {
    std::error_code ec;
    if (!to_path(a.convert_helper).is_absolute() || !fs::is_regular_file(to_path(a.convert_helper), ec)) {
      return fail("--helper must name the app's clip helper by its full path.", 1);
    }
    s.convert_helper = a.convert_helper;
  }
  if (!write_whole(l.settings, host_settings_json(s))) return fail("could not write host.json.", 1);
  if (!a.on) {
    print("Phones get the download for clips they can't play; nothing is converted.\n");
  } else if (s.convert_helper.empty()) {
    print("On, but no clip helper is known yet: MediaViewer's Phone access card names it.\n");
  } else {
    print("Clips a phone can't play are converted on this computer's graphics hardware as it watches.\n");
  }
  return 0;
}

int run_stay_awake(const args& a, const host_layout& l) {
  if (!make_host_folders(l)) return fail("its folder must be this user's alone (mode 700).", 1);
  host_settings s = load_settings(l);
  s.stay_awake = a.on;
  if (!write_whole(l.settings, host_settings_json(s))) return fail("could not write host.json.", 1);
  print(a.on ? "While it hosts, this computer does not go to sleep by itself (the display still may).\n"
             : "While it hosts, this computer may go to sleep; phones cannot reach it then.\n");
  return 0;
}

autostart_place autostart_where(const host_layout& l) {
  autostart_place p;
#if defined(_WIN32)
  p.home = l.folder;
#else
  (void)l;
  p.home = home_dir();
#endif
  return p;
}

int run_start_at_login(const args& a, const host_layout& l, bool test_hooks) {
  if (test_hooks) return fail("the test build never changes what starts at login.", 1);
  if (!make_host_folders(l)) return fail("its folder must be this user's alone (mode 700).", 1);
  autostart_command c;
  c.program = stable_program(self_path(), [](const std::string& p) {
    std::error_code ec;
    return fs::exists(to_path(p), ec);
  });
  if (c.program.empty()) return fail("cannot tell where this program is.", 1);
  c.args = {"host"};
  if (!a.data.empty()) c.args.insert(c.args.end(), {"--data", absolute_of(a.data)});
  const autostart_result r = set_autostart(a.on, c, autostart_where(l));
  print(r.note + (r.note.empty() || r.note.back() == '\n' ? "" : "\n"), !r.ok);
  return r.ok ? 0 : 1;
}

// "Tray": decimal units, as the OS's file sizes (phone/host_tray.h's bytes_words).
std::string bytes_words(std::uint64_t n) {
  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int u = 0;
  while (v >= 1000.0 && u < 4) {
    v /= 1000.0;
    ++u;
  }
  char buf[32];
  (void)std::snprintf(buf, sizeof buf, u == 0 || v >= 10.0 ? "%.0f %s" : "%.1f %s", v, units[u]);
  return buf;
}

std::string clock_of(std::int64_t unix_s) {
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

// The usage and the phones connected, from state.json ("Tray"): the same words
// the menu and the card say. Names only, never an address.
std::string usage_lines(const json::value& state) {
  std::string out;
  const auto n = [](const json::value& v, const char* k) {
    const auto x = v.integer(k).value_or(0);
    return x > 0 ? static_cast<std::uint64_t>(x) : std::uint64_t{0};
  };
  out += std::string("hosting     ") + (state.boolean("paused").value_or(false) ? "off (driftwire host resume)" : "on") + "\n";
  if (const json::value* u = state.find("usage"); u && u->k == json::kind::object) {
    std::string line = "today       " + bytes_words(n(*u, "read")) + " read, " + bytes_words(n(*u, "written")) + " written";
    const auto rr = n(*u, "read_rate"), wr = n(*u, "write_rate");
    if (rr || wr) line += "; now " + bytes_words(rr) + "/s read, " + bytes_words(wr) + "/s written";
    out += line + "\n";
    if (const json::value* d = u->find("devices"); d && d->k == json::kind::array) {
      for (const auto& v : d->a) {
        const std::string* name = v.k == json::kind::object ? v.str("name") : nullptr;
        if (!name) continue;
        out += "  " + *name + "  " + bytes_words(n(v, "read")) + " read, " + bytes_words(n(v, "written")) + " written\n";
      }
    }
  }
  const json::value* c = state.find("clients");
  const bool any = c && c->k == json::kind::array && !c->a.empty();
  out += std::string("connected   ") + (any ? std::to_string(c->a.size()) : std::string("none")) + "\n";
  if (any) {
    for (const auto& v : c->a) {
      const std::string* name = v.k == json::kind::object ? v.str("name") : nullptr;
      if (!name) continue;
      const std::string* via = v.str("via");
      const std::string how = via && *via == "home" ? "home network" : via && *via == "relay" ? "through your relay" : "over the internet";
      std::string line = "  " + *name + "  " + how;
      if (const auto since = v.integer("since").value_or(0); since > 0) line += " since " + clock_of(since);
      line += "  " + bytes_words(n(v, "read")) + " read, " + bytes_words(n(v, "written")) + " written";
      const auto rr = n(v, "read_rate"), wr = n(v, "write_rate");
      if (rr || wr) line += "  now " + bytes_words(rr) + "/s read, " + bytes_words(wr) + "/s written";
      out += line + "\n";
    }
  }
  return out;
}

std::string status_text(const host_layout& l, bool test_hooks) {
  std::string out;
  std::error_code fec;
  const bool folder = fs::is_directory(to_path(l.folder), fec);
  // Held by a running host. Taken here for a moment only: a host starting
  // just then tries again for a while (run_supervisor).
  const bool running = folder && net::try_lock_instance(l.lock) == nullptr;
  const auto state_text = running ? read_small(l.state) : std::nullopt;
  const auto state = state_text ? json::parse(*state_text, 6) : std::nullopt;
  const auto cfg = load_or_new(l, test_hooks);
  const host_settings s = load_settings(l);
  out += std::string("host        ") + (running ? "running" : "not running (driftwire host starts it)") + "\n";
  if (cfg) out += "name        " + cfg->name + "\n";
  const auto port_of = [&](const char* k) -> std::int64_t { return state ? state->integer(k).value_or(0) : 0; };
  if (!cfg || cfg->roots.empty()) {
    out += "service     no folder shared yet (driftwire host share PATH)\n";
  } else if (running && state && state->boolean("paused").value_or(false)) {
    out += "service     off while hosting is off\n";
  } else if (running) {
    const auto p = port_of("service_port");
    out += p ? "service     port " + std::to_string(p) + "\n"
             : "service     not listening (port " + std::to_string(cfg->port) + " taken?)\n";
  } else {
    out += "service     port " + std::to_string(cfg->port) + " when running\n";
  }
  if (running && state && state->boolean("paused").value_or(false)) {
    out += "relay       off while hosting is off\n";
  } else if (running) {
    const auto p = port_of("relay_port");
    out += p ? "relay       port " + std::to_string(p) + "\n" : "relay       not listening (port taken?)\n";
  } else {
    out += "relay       port " + std::to_string(s.relay_port) + " when running\n";
  }
  const bool tls_down = state && state->boolean("tls_failed").value_or(false);
  out += std::string("VPN         ") +
         (s.vpn ? (look_port(s) ? (tls_down ? "on; the web-site look could not listen on port "
                                            : "on, web-site look on port ") + std::to_string(look_port(s))
                                : std::string("on, without the web-site look"))
                : "off") +
         (s.vpn ? "  (driftwire host vpn on shows the relay line again)\n" : "\n");
  if (cfg) {
    for (const auto& r : cfg->roots) {
      std::error_code ec;
      const auto at = volume_locate(r);
      const bool there = at && fs::is_directory(to_path(*at), ec);
      out += "folder      " + r.label + (r.backup ? " (backup)" : "") + (there ? "" : "  offline") + "\n";
    }
  }
  if (auto store = owner_store(l.serve + "/secrets")) {
    std::vector<serve::grants::grant> limited;
    (void)serve::grants::load(*store, limited);
    out += "phones      " + std::to_string(known::load(*store).size() + limited.size()) + " paired\n";
    // The LAN share and its users: addresses, names and roles only (self-host-users-smb.md §7).
    for (const auto& line : users_status_lines(*store)) out += line + "\n";
  }
  const bool held = state && state->boolean("awake").value_or(false);
  out += std::string("stay awake  ") + (s.stay_awake ? (running ? (held ? "on" : "on (the OS refused)") : "on") : "off") + "\n";
  out += std::string("at login    ") + (autostart_is_on(autostart_where(l)) ? "on" : "off") + "\n";
  if (state) {
    if (const json::value* k = state->find("keepers"); k && k->k == json::kind::array) {
      for (const auto& line : k->a) {
        if (line.k == json::kind::string) out += line.s + "\n";
      }
    }
    out += usage_lines(*state);
  } else if (!test_hooks) {
    // Not running: no drives keeper, so the sets' health is asked here (no volume opened).
    const auto sets = drives::os_port()->sets_only().sets;
    if (const std::string line = drives_status_line(sets); !line.empty()) out += line + "\n";
  }
  return out;
}

int run_status(const host_layout& l, bool test_hooks) {
  print(status_text(l, test_hooks));
  return 0;
}

// `host status --watch`: again whenever it changed, read at the host's own
// cadence (every 2 s), until Ctrl-C. A terminal is redrawn in place.
int run_status_watch(const host_layout& l, bool test_hooks) {
  (void)std::signal(SIGINT, on_host_signal);
  (void)std::signal(SIGTERM, on_host_signal);
  // Redrawn in place only where the terminal takes the escapes; else printed again.
  const bool tty = console::redraws_in_place();
  std::string shown;
  while (!g_stop.load()) {
    const std::string now = status_text(l, test_hooks);
    if (now != shown) {
      print(tty ? "\x1b[H\x1b[2J" + now + "\n(Ctrl-C to stop)\n" : (shown.empty() ? now : "\n" + now));
      shown = now;
    }
    for (int i = 0; i < 20 && !g_stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}

int run_child(const args& a, const host_layout& l, bool test_hooks) {
#if !defined(_WIN32)
  (void)std::signal(SIGPIPE, SIG_IGN);
#endif
  (void)std::signal(SIGINT, on_host_signal);
  (void)std::signal(SIGTERM, on_host_signal);
  // The supervisor holds the other end: when it closes it, or dies, this stops.
  std::thread([] {
    wait_for_stdin_end();
    g_stop.store(true);
  }).detach();
  host_service_options o;
  o.where = l;
  o.loopback_only = test_hooks && a.test_loopback;
  o.any_port = test_hooks;
  if (test_hooks && a.test_quick) {
    o.config_every_ms = 200;
    o.roots_every_ms = 1000;
    o.retry_ms = 1000;
  }
  o.power = !test_hooks;
  o.keepers = host_keepers();
  if (const char* since = std::getenv(kHostSinceEnv)) {
    char* end = nullptr;
    const long long v = std::strtoll(since, &end, 10);
    if (end && *end == '\0' && v > 0) o.quit_since_ms = v;
  }
  std::string why;
  auto hs = host_service::start(std::move(o), why);
  if (!hs) {
    print("driftwire host: " + why + "\n", true);
    return kHostChildCannotRun;
  }
  while (!g_stop.load() && !hs->quit_asked()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const bool quit = hs->quit_asked();
  hs->stop();
  return quit ? kHostChildQuit : 0;
}

// The program's identity on disk: replaced by an update, it changes.
struct program_stamp {
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
  bool known = false;
  friend bool operator==(const program_stamp&, const program_stamp&) = default;
};
program_stamp stamp_of(const std::string& path) {
  program_stamp s;
  std::error_code ec, ec2;
  s.size = fs::file_size(to_path(path), ec);
  s.mtime = fs::last_write_time(to_path(path), ec2);
  s.known = !ec && !ec2;
  return s;
}

// Waits `ms`, or less when asked to stop; false when asked.
bool pause_unless_stopped(int ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (g_stop.load()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !g_stop.load();
}

void stop_child(child_process& c) {
  c.ask_to_stop();
  if (!c.wait(10000)) {
    c.kill();
    (void)c.wait(5000);
  }
}

int run_supervisor(const args& a, const host_layout& l, const std::string& data, bool test_hooks) {
  leave_own_console();
  if (!make_host_folders(l)) return fail("its folder must be this user's alone (mode 700): " + l.folder, 1);
  std::unique_ptr<net::instance_lock> lock;
  for (int i = 0; i < 20 && !lock; ++i) {  // `host status` may hold it for a moment
    lock = net::try_lock_instance(l.lock);
    if (!lock) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!lock) {
    print("driftwire host: already running.\n");
    return 0;  // not a failure: what starts at login finds it running
  }
#if !defined(_WIN32)
  (void)std::signal(SIGPIPE, SIG_IGN);
#endif
  (void)std::signal(SIGINT, on_host_signal);
  (void)std::signal(SIGTERM, on_host_signal);
  const std::string program = self_path();
  if (program.empty()) return fail("cannot tell where this program is.", 1);
  const std::string watched = stable_program(program, [](const std::string& p) {
    std::error_code ec;
    return fs::exists(to_path(p), ec);
  });
  const program_stamp first = stamp_of(watched);
  const int restart_ms = test_hooks && a.test_quick ? 1000 : kHostRestartMs;
  const int update_every_ms = test_hooks && a.test_quick ? 500 : kHostUpdateEveryMs;
  std::vector<std::string> child_args{"host", "child", "--data", data};
  if (test_hooks && a.test_loopback) child_args.emplace_back("--test-loopback");
  if (test_hooks && a.test_quick) child_args.emplace_back("--test-quick");
  print("driftwire host: hosting from this computer; stop with Ctrl-C (driftwire host status shows it)\n");
  auto next_update_look = std::chrono::steady_clock::now() + std::chrono::milliseconds(update_every_ms);
  // A `host quit` while no child runs (the 30 s before a restart): taken before the next one.
  const std::int64_t started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count();
  // Its children compare a `host quit` with this start, not their own.
  {
    const std::string v = std::to_string(started_ms);
#if defined(_WIN32)
    (void)::_putenv_s(kHostSinceEnv, v.c_str());
#else
    (void)::setenv(kHostSinceEnv, v.c_str(), 1);
#endif
  }
  // A quit taken: its stamp cleared, so a clock set back at the next boot stops nothing.
  const auto quit_taken = [&] {
    host_settings s = load_settings(l);
    if (s.quit_stamp == 0) return;
    s.quit_stamp = 0;
    (void)write_whole(l.settings, host_settings_json(s));
  };
  for (;;) {
    if (const auto q = load_settings(l).quit_stamp; q != 0 && q >= started_ms) {
      quit_taken();
      print("driftwire host: stopped\n");
      return 0;
    }
    auto child = child_process::start(program, child_args);
    if (!child) {
      print("driftwire host: could not start its service; trying again in 30 s\n", true);
      if (!pause_unless_stopped(restart_ms)) return 0;
      continue;
    }
    for (;;) {
      if (child->wait(200)) break;
      if (g_stop.load()) {
        stop_child(*child);
        print("driftwire host: stopped\n");
        return 0;
      }
      if (std::chrono::steady_clock::now() >= next_update_look) {
        next_update_look = std::chrono::steady_clock::now() + std::chrono::milliseconds(update_every_ms);
        const program_stamp now = stamp_of(watched);
        if (first.known && now.known && !(now == first)) {
          stop_child(*child);
          print("driftwire host: driftwire was updated; the new version takes over\n");
          return nas_update::kRestartForUpdate;  // the start-at-login entry starts it
        }
      }
    }
    const child_process::ended how = child->how();
    switch (decide_after_child(how, g_stop.load())) {
      case after_child::stop:
        if (!how.crashed && how.code == kHostChildQuit) quit_taken();
        print("driftwire host: stopped\n");
        return 0;
      case after_child::fail:
        return kHostChildCannotRun;  // the child said why; systemd does not try again (RestartPreventExitStatus)
      case after_child::restart:
        break;
    }
    print(std::string("driftwire host: its service ended (") + (how.crashed ? "a crash" : "exit " + std::to_string(how.code)) +
          "); starting it again in " + std::to_string(restart_ms / 1000) + " s\n",
          true);
    if (!pause_unless_stopped(restart_ms)) return 0;
  }
}

}  // namespace

// ---- for host_setup.cpp ---------------------------------------------------------------------

namespace host_files {

std::optional<std::string> read(const std::string& path, std::size_t max) { return read_small(path, max); }
bool write(const std::string& path, const std::string& text) { return write_whole(path, text); }
std::unique_ptr<net::instance_lock> lock_config(const host_layout& l, int wait_ms) { return config_lock(l, wait_ms); }
host_settings settings(const host_layout& l) { return load_settings(l); }
bool save_settings(const host_layout& l, const host_settings& s) {
  return make_host_folders(l) && write_whole(l.settings, host_settings_json(s));
}
std::optional<serve::config> config(const host_layout& l, bool any_port) { return load_or_new(l, any_port); }
std::unique_ptr<secrets::store> store(const std::string& folder) { return owner_store(folder); }
std::optional<std::string> relay_secret(const host_layout& l) {
  if (!make_host_folders(l)) return std::nullopt;
  auto st = owner_store(l.relay + "/secrets");
  return st ? relay_key(*st) : std::nullopt;
}
std::string share(const host_layout& l, const std::string& typed, const std::string& label, bool backup,
                  std::uint64_t limit, bool any_port, std::string& shared_as) {
  return share_path(l, typed, label, backup, limit, any_port, shared_as);
}
bool running(const host_layout& l) {
  std::error_code ec;
  return fs::is_directory(to_path(l.folder), ec) && net::try_lock_instance(l.lock) == nullptr;
}
std::optional<json::value> state(const host_layout& l) {
  if (!running(l)) return std::nullopt;
  const auto text = read_small(l.state);
  if (!text) return std::nullopt;
  return json::parse(*text, 6);
}
std::string new_id() { return random_id(); }
int status(const host_layout& l, bool test_hooks) { return run_status(l, test_hooks); }
int start_at_login(const args& a, const host_layout& l, bool test_hooks) { return run_start_at_login(a, l, test_hooks); }

}  // namespace host_files

int run_host(const args& a, bool test_hooks, const char* version) {
  (void)version;
  // The LAN share's elevated step is the one `host` command that runs elevated,
  // and only that way (host_users.h); it reads its whole request from the front end.
  if (a.host == host_action::smb_apply) return run_smb_apply(a);
  if (!test_hooks && serve_is_privileged()) {
#if defined(_WIN32)
    return fail("it must not run as administrator; run it from a terminal that is not elevated.", 1);
#else
    return fail("it must not run as root; run it as yourself.", 1);
#endif
  }
  if (sodium_init() < 0) return fail("could not start its cryptography.", 1);
  std::string data = data_folder_of(a);
  if (data.empty()) return fail("no data folder (give --data DIR).", 1);
  data = absolute_of(data);
  {
    std::error_code ec;
    fs::create_directories(to_path(data), ec);
  }
  const host_layout l = host_layout_of(data);
  switch (a.host) {
    case host_action::run:
      return run_supervisor(a, l, data, test_hooks);
    case host_action::child:
      return run_child(a, l, test_hooks);
    case host_action::share:
      return run_share(a, l, test_hooks);
    case host_action::unshare:
      return run_unshare(a, l, test_hooks);
    case host_action::status:
      return a.watch ? run_status_watch(l, test_hooks) : run_status(l, test_hooks);
    case host_action::start_at_login:
      return run_start_at_login(a, l, test_hooks);
    case host_action::stay_awake:
      return run_stay_awake(a, l);
    case host_action::setup:
      return run_host_setup(a, l, test_hooks);
    case host_action::name:
      return run_host_name(a, l, test_hooks);
    case host_action::pair:
      return run_host_pair(a, l, test_hooks);
    case host_action::vpn:
      return run_host_vpn(a, l, test_hooks);
    case host_action::convert:
      return run_host_convert(a, l);
    case host_action::users:
      return run_host_users(a, l, test_hooks);
    case host_action::smb_apply:
      return run_smb_apply(a);
    case host_action::drives:
      return run_host_drives(a, l, data, test_hooks);
    case host_action::pause:
    case host_action::resume:
    case host_action::quit:
      return run_host_control(a, l);
  }
  return 2;
}

}  // namespace mv::driftwire::cli
