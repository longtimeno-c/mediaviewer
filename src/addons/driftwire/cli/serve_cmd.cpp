// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/serve_cmd.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/grants.h"
#include "addons/driftwire/owner_pass.h"
#include "addons/driftwire/secrets.h"
#include "addons/driftwire/serve.h"
#include "addons/driftwire/transfer.h"
#include "core/json.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace mv::driftwire::cli {

namespace {

constexpr const char* kConfigFile = "serve.json";

std::atomic<bool> g_stop{false};

extern "C" void on_stop_signal(int) { g_stop.store(true); }

std::string data_folder(const args& a) {
  if (!a.data.empty()) return a.data;
  if (!a.test_data.empty()) return a.test_data;
  return data_dir();
}

int fail(const std::string& why, int code) {
  print("driftwire serve: " + why + "\n", true);
  return code;
}

std::unique_ptr<secrets::store> open_store(const std::string& data) {
  const std::string folder = data + "/secrets";
  if (!secrets::owner_only_folder(folder)) return nullptr;
  return secrets::file_store(folder);
}

// The config names what is served: nobody but this user may change it.
bool only_mine_writes(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  struct stat st{};
  return ::lstat(path.c_str(), &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) && st.st_uid == ::geteuid() &&
         (st.st_mode & 022) == 0;
#endif
}

// The list, or false for one that is there and cannot be read: saving over
// it would forget every other device.
bool load_devices(secrets::store& store, std::vector<known::device>& out) {
  out.clear();
  if (!store.exists(known::kDevicesName)) return true;
  auto v = store.get(known::kDevicesName);
  if (!v) return false;
  auto d = known::decode(*v);
  sodium_memzero(v->data(), v->size());
  if (!d) return false;
  out = std::move(*d);
  return true;
}

// Whole and renamed in, owner-only: the running service is the only writer.
bool write_config(const std::string& path, const serve::config& c) {
  json::writer w;
  w.begin_object().key("port").integer(c.port).key("name").string(c.name).key("roots").begin_array();
  for (const auto& r : c.roots) {
    w.begin_object().key("id").string(r.id).key("label").string(r.label).key("path").string(r.path).end_object();
  }
  w.end_array().end_object();
  const std::string tmp = path + ".part";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(w.str().data(), static_cast<std::streamsize>(w.str().size()));
    if (!f.flush()) return false;
  }
#if !defined(_WIN32)
  if (::chmod(tmp.c_str(), 0600) != 0) return false;
#endif
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(tmp.data()), tmp.size())),
                          std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), ec);
  return !ec;
}

int run_service(const args& a, bool test_hooks, const std::string& data, secrets::store& store) {
  if (!only_mine_writes(data) || !only_mine_writes(data + "/" + kConfigFile)) {
    return fail(std::string("the data folder and ") + kConfigFile + " must be writable by this user alone.", 1);
  }
  std::ifstream f(data + "/" + kConfigFile, std::ios::binary);
  if (!f) return fail(std::string("no ") + kConfigFile + " in the data folder (run from the service's folder).", 1);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto cfg = serve::parse_config(text, test_hooks);
  if (!cfg) return fail(std::string(kConfigFile) + " is not valid (a port, and at least one folder).", 1);

  // One service per data folder.
  const auto lock = net::try_lock_instance(data + "/serve.lock");
  if (!lock) return fail("another service is already running in this folder.", 1);

  serve::server::options o;
  o.cfg = std::move(*cfg);
  o.store = &store;
  o.loopback_only = test_hooks && a.test_loopback;
  o.claimable = true;
  o.devices_lock = data + "/serve-devices.lock";
  o.save_config = [path = data + "/" + kConfigFile](const serve::config& c) { return write_config(path, c); };
  auto srv = serve::server::start(o);
  if (!srv) {
    if (srv.error() == status::not_found) return fail("no access key yet: run `driftwire serve init` first.", 1);
    if (srv.error() == status::io) return fail("could not listen on that port.", 1);
    return fail("could not start.", 1);
  }

#if !defined(_WIN32)
  // A closed stdout (a supervisor that went away) must not kill the service.
  (void)std::signal(SIGPIPE, SIG_IGN);
#endif
  (void)std::signal(SIGINT, on_stop_signal);
  (void)std::signal(SIGTERM, on_stop_signal);
  // Numbers only: this line goes to the service's log, which may be read
  // beside the shares (rule 6: no address, no path).
  char line[96];
  std::snprintf(line, sizeof line, "driftwire serve: listening on port %u\n", static_cast<unsigned>((*srv)->port()));
  print(line);
  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  (*srv)->stop();
  print("driftwire serve: stopped\n");
  return 0;
}

// One line from stdin, its newline gone; echo off when it is a terminal.
// False at the end of input.
bool read_line(std::string& out, bool hidden) {
  out.clear();
  out.reserve(serve::owner::kMaxBytes + 2);  // never reallocated: no copy left behind unwiped
#if defined(_WIN32)
  HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool console = hidden && ::GetConsoleMode(in, &mode);
  if (console) ::SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
#else
  termios before{};
  const bool console = hidden && ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &before) == 0;
  if (console) {
    termios quiet = before;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  }
#endif
  bool got = false;
  for (int c; (c = std::fgetc(stdin)) != EOF;) {
    got = true;
    if (c == '\n') break;
    if (out.size() < serve::owner::kMaxBytes + 1) out.push_back(static_cast<char>(c));
  }
  if (!out.empty() && out.back() == '\r') out.pop_back();
#if defined(_WIN32)
  if (console) ::SetConsoleMode(in, mode);
#else
  if (console) (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &before);
#endif
  if (console) print("\n");
  return got;
}

bool stdin_is_terminal() {
#if defined(_WIN32)
  DWORD mode = 0;
  return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
  return ::isatty(STDIN_FILENO) != 0;
#endif
}

struct wiped_string {
  std::string s;
  ~wiped_string() {
    if (!s.empty()) sodium_memzero(s.data(), s.size());
  }
};

// The owner's passphrase: typed twice on the terminal, or one line of stdin
// (--passphrase-stdin). Empty with `why` set when there is none to have.
bool ask_passphrase(const args& a, wiped_string& out, std::string& why) {
  if (a.passphrase_stdin) {
    if (!read_line(out.s, false)) {
      why = "no passphrase on stdin.";
      return false;
    }
  } else {
    if (!stdin_is_terminal()) {
      why = "no terminal to ask for the owner's passphrase: give --passphrase-stdin (or, for init, --no-passphrase).";
      return false;
    }
    print("Owner's passphrase (at least 10 characters): ");
    if (!read_line(out.s, true)) {
      why = "no passphrase given.";
      return false;
    }
    wiped_string again;
    print("Again: ");
    if (!read_line(again.s, true) || again.s.size() != out.s.size() ||
        sodium_memcmp(again.s.data(), out.s.data(), out.s.size()) != 0) {
      why = "the two did not match; nothing changed.";
      return false;
    }
  }
  if (!serve::owner::acceptable(out.s)) {
    why = "a passphrase needs at least 10 characters; nothing changed.";
    return false;
  }
  return true;
}

// Argon2id at 256 MiB (call C11), kept as a verifier: the passphrase itself is never written.
int set_passphrase(const std::string& passphrase, secrets::store& store) {
  const auto v = serve::owner::make(passphrase, std::time(nullptr));
  if (!v) return fail("could not stretch the passphrase (not enough memory?); nothing changed.", 1);
  if (!serve::owner::save(store, *v)) return fail("could not keep the passphrase.", 1);
  return 0;
}

int run_init(const args& a, secrets::store& store) {
  // The passphrase first, so a mistyped one leaves nothing half done; never
  // over one that is set (`serve passphrase set` changes it).
  serve::owner::verifier there;
  const auto held = serve::owner::load(store, there);
  if (held == serve::owner::stored::unreadable) return fail("the owner's passphrase there cannot be read; run `serve passphrase reset`.", 1);
  wiped_string pass;
  if (held == serve::owner::stored::none && !a.no_passphrase) {
    std::string why;
    if (!ask_passphrase(a, pass, why)) return fail(why, 2);
  }
  std::array<std::uint8_t, 32> pub{}, key{};
  bool made = false;
  if (!serve::set_up(store, pub, key, made)) return fail("could not write the service's keys.", 1);
  // The key is shown once, when made; afterwards only the identity is.
  std::string out = "identity " + key_hex(pub) + "\n";
  if (made) out += "access-key " + key_hex(key) + "\n";
  sodium_memzero(key.data(), key.size());
  if (!pass.s.empty()) {
    if (const int r = set_passphrase(pass.s, store); r != 0) {
      print(out);
      return r;
    }
  }
  const bool has = serve::owner::load(store, there) == serve::owner::stored::set;
  out += std::string("passphrase ") + (has ? "set" : "none") + "\n";
  print(out);
  return 0;
}

int run_status(secrets::store& store) {
  const auto identity = store.get(known::kIdentityName);
  if (!identity || identity->size() != 32) return fail("no keys yet: run `driftwire serve init` first.", 1);
  std::array<std::uint8_t, 32> priv{}, pub{};
  std::memcpy(priv.data(), identity->data(), priv.size());
  crypto_scalarmult_base(pub.data(), priv.data());
  sodium_memzero(priv.data(), priv.size());
  std::string out = "identity " + key_hex(pub) + "\n";
  out += std::string("owner ") + (store.exists(serve::kOwnerName) ? "claimed" : "none") + "\n";
  serve::owner::verifier v;
  const auto s = serve::owner::load(store, v);
  out += std::string("passphrase ") +
         (s == serve::owner::stored::set ? "set" : s == serve::owner::stored::none ? "none" : "unreadable (run serve passphrase reset)") + "\n";
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const auto l = serve::owner::load_lockout(store, now);
  for (const auto& [label, c] : {std::pair<const char*, serve::owner::counter>{"claims", l.claim}, {"owner", l.owner}}) {
    out += std::string("lockout ") + label + " ";
    out += c.locked(now) ? "until " + std::to_string(c.until) + " (unix seconds)" : std::to_string(c.misses) + " wrong";
    out += "\n";
  }
  print(out);
  return 0;
}

// Here, as the service's user, whoever can run this can edit the store
// anyway, so it asks nothing first (call C3).
int run_passphrase_set(const args& a, secrets::store& store) {
  serve::owner::verifier there;
  if (serve::owner::load(store, there) == serve::owner::stored::unreadable) {
    return fail("the passphrase there cannot be read; run `serve passphrase reset`.", 1);
  }
  wiped_string pass;
  std::string why;
  if (!ask_passphrase(a, pass, why)) return fail(why, 2);
  if (const int r = set_passphrase(pass.s, store); r != 0) return r;
  print("passphrase set; unlocked sessions end\n");
  return 0;
}

// Recovery: a new passphrase replaces the old, and both lockouts go. It is
// stretched first, so a NAS without the memory for it is left as it was,
// never with none (with none, the next claim would choose it).
int run_passphrase_reset(const args& a, secrets::store& store) {
  wiped_string pass;
  std::string why;
  if (!ask_passphrase(a, pass, why)) return fail(why, 2);
  const auto v = serve::owner::make(pass.s, std::time(nullptr));
  if (!v) return fail("could not stretch the passphrase (not enough memory?); nothing changed.", 1);
  // Written over the old one, never through "none": an unowned service is
  // claimable with whatever passphrase the claim brings while there is none.
  if (!serve::owner::save(store, *v)) return fail("could not write the new passphrase; the old one stays.", 1);
  (void)store.erase(serve::owner::kLockoutName);
  if (store.exists(serve::owner::kLockoutName)) return fail("the new passphrase is set, but the lockouts could not be cleared.", 1);
  print("passphrase reset at " + std::to_string(std::time(nullptr)) + "; lockouts cleared; unlocked sessions end\n");
  return 0;
}

// A device on the list its limit says (grants.h): unlimited in the paired
// list, timed only in the grants, never both. The list it goes to is written
// first, as the service does. Under the file lock.
int put_device(secrets::store& store, const std::string& name, const std::array<std::uint8_t, 32>& key,
               std::int64_t for_s, std::int64_t now) {
  std::vector<known::device> devices;
  if (!load_devices(store, devices)) return fail("the device list cannot be read; not changing it.", 1);
  std::vector<serve::grants::grant> limited;
  if (!serve::grants::load(store, limited)) return fail("the timed device list cannot be read; not changing it.", 1);
  (void)serve::grants::prune(limited, now);
  const bool was_unlimited = std::any_of(devices.begin(), devices.end(), [&](const known::device& d) { return d.key == key; });
  const bool was_live = std::any_of(limited.begin(), limited.end(), [&](const auto& g) { return g.key == key && !g.expired(now); });
  std::erase_if(devices, [&](const known::device& d) { return d.key == key; });
  std::erase_if(limited, [&](const auto& g) { return g.key == key; });
  if (!was_unlimited && !was_live && !serve::grants::make_room(limited, devices.size(), now)) {
    return fail("too many paired devices; revoke one first.", 1);
  }
  if (for_s > 0) {
    serve::grants::grant g;
    g.name = name;
    g.key = key;
    g.expires = now + for_s;
    g.created = now;
    limited.push_back(std::move(g));
    if (!serve::grants::save(store, limited) || !known::save(store, devices)) return fail("could not save the device lists.", 1);
  } else {
    known::device d;
    d.name = name;
    d.key = key;
    devices.push_back(std::move(d));
    if (!known::save(store, devices) || !serve::grants::save(store, limited)) return fail("could not save the device lists.", 1);
  }
  return 0;
}

int run_add(const args& a, secrets::store& store, const std::string& data) {
  const auto key = parse_key_hex(a.serve_key);
  if (!key) return fail("that is not a device key (64 hex digits).", 2);
  const auto lock = net::try_lock_instance(data + "/serve-devices.lock");
  if (!lock) return fail("the device list is being changed by something else; try again.", 1);
  if (!a.for_duration.empty()) {
    const auto for_s = serve::grants::parse_duration(a.for_duration);
    if (!for_s) return fail("--for takes a duration up to 365d: 90m, 24h, 7d or seconds.", 2);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (const int r = put_device(store, a.name, *key, *for_s, now); r != 0) return r;
    print("paired until " + std::to_string(now + *for_s) + " (unix seconds)\n");
    return 0;
  }
  std::vector<known::device> devices;
  if (!load_devices(store, devices)) return fail("the device list cannot be read; not changing it.", 1);
  for (auto& d : devices) {
    if (d.key == *key) {
      d.name = a.name;
      if (!known::save(store, devices)) return fail("could not save the device list.", 1);
      print("updated\n");
      return 0;
    }
  }
  if (devices.size() >= known::kMaxDevices) return fail("too many paired devices; revoke one first.", 1);
  known::device d;
  d.name = a.name;
  d.key = *key;
  devices.push_back(std::move(d));
  if (!known::save(store, devices)) return fail("could not save the device list.", 1);
  print("paired\n");
  return 0;
}

int run_revoke(const args& a, secrets::store& store, const std::string& data) {
  const auto lock = net::try_lock_instance(data + "/serve-devices.lock");
  if (!lock) return fail("the device list is being changed by something else; try again.", 1);
  std::vector<known::device> devices;
  if (!load_devices(store, devices)) return fail("the device list cannot be read; not changing it.", 1);
  std::vector<serve::grants::grant> limited;
  if (!serve::grants::load(store, limited)) return fail("the timed device list cannot be read; not changing it.", 1);
  const auto key = parse_key_hex(a.serve_key);
  const auto before = devices.size() + limited.size();
  std::erase_if(devices, [&](const known::device& d) { return (key && d.key == *key) || d.name == a.serve_key; });
  std::erase_if(limited, [&](const serve::grants::grant& g) { return (key && g.key == *key) || g.name == a.serve_key; });
  if (devices.size() + limited.size() == before) return fail("no such device.", 1);
  // save() does not report a last entry it could not erase: read it back.
  std::vector<known::device> kept;
  if (!serve::grants::save(store, limited) || !known::save(store, devices) || !load_devices(store, kept) ||
      kept.size() != devices.size()) {
    return fail("could not save the device list; the device may still be paired.", 1);
  }
  print("revoked\n");
  return 0;
}

// A device's limit from now: --for D (timed) or --forever (unlimited again).
// Here, as the service's user, it asks nothing (call C3).
int run_grant(const args& a, secrets::store& store, const std::string& data) {
  const auto lock = net::try_lock_instance(data + "/serve-devices.lock");
  if (!lock) return fail("the device list is being changed by something else; try again.", 1);
  std::vector<known::device> devices;
  if (!load_devices(store, devices)) return fail("the device list cannot be read; not changing it.", 1);
  std::vector<serve::grants::grant> limited;
  if (!serve::grants::load(store, limited)) return fail("the timed device list cannot be read; not changing it.", 1);
  const auto key = parse_key_hex(a.serve_key);
  std::optional<std::pair<std::string, std::array<std::uint8_t, 32>>> found;
  for (const auto& d : devices) {
    if ((key && d.key == *key) || d.name == a.serve_key) found.emplace(d.name, d.key);
  }
  for (const auto& g : limited) {
    if ((key && g.key == *key) || g.name == a.serve_key) found.emplace(g.name, g.key);
  }
  if (!found) return fail("no such device.", 1);
  std::int64_t for_s = 0;
  if (!a.forever) {
    const auto parsed = serve::grants::parse_duration(a.for_duration);
    if (!parsed) return fail("--for takes a duration up to 365d: 90m, 24h, 7d or seconds.", 2);
    for_s = *parsed;
  }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  if (const int r = put_device(store, found->first, found->second, for_s, now); r != 0) return r;
  print(for_s > 0 ? "until " + std::to_string(now + for_s) + " (unix seconds)\n" : std::string("unlimited\n"));
  return 0;
}

// A single-use code for the QR (M3); only its hash is kept. Printed alone, for
// the desktop to read.
int run_pair_code(const args& a, secrets::store& store, const std::string& data) {
  std::int64_t seconds = serve::kDefaultPairCodeSeconds;
  if (!a.serve_key.empty()) {
    const auto parsed = std::strtoll(a.serve_key.c_str(), nullptr, 10);
    if (parsed <= 0 || parsed > 24 * 3600) return fail("the code lasts between 1 and 86400 seconds.", 2);
    seconds = parsed;
  }
  const auto lock = net::try_lock_instance(data + "/serve-devices.lock");
  if (!lock) return fail("the device list is being changed by something else; try again.", 1);
  std::string code;
  if (!a.for_duration.empty()) {
    // A guest code: its phone gets this long from when it is redeemed (call C7).
    const auto for_s = serve::grants::parse_duration(a.for_duration);
    if (!for_s) return fail("--for takes a duration up to 365d: 90m, 24h, 7d or seconds.", 2);
    serve::grants::limit gives;
    gives.for_s = *for_s;
    code = serve::grants::issue_guest_code(store, seconds, gives, std::time(nullptr));
  } else {
    code = serve::issue_pair_code(store, seconds, std::time(nullptr));
  }
  if (code.empty()) return fail("could not keep the code.", 1);
  print(code + "\n");
  return 0;
}

// The desktop that claimed this service is gone: it can be claimed again.
int run_disown(secrets::store& store) {
  if (!store.exists(serve::kOwnerName)) return fail("it has no owner.", 1);
  if (!store.erase(serve::kOwnerName)) return fail("could not forget the owner.", 1);
  print("disowned; restart not needed\n");
  return 0;
}

int run_devices(secrets::store& store) {
  const auto devices = known::load(store);
  std::vector<serve::grants::grant> limited;
  const bool readable = serve::grants::load(store, limited);
  if (devices.empty() && limited.empty()) {
    print(readable ? "No paired devices.\n" : "No paired devices; the timed list cannot be read.\n");
    return readable ? 0 : 1;
  }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  std::string out;
  for (const auto& d : devices) out += d.name + "  " + key_hex(d.key) + "\n";
  for (const auto& g : limited) {
    out += g.name + "  " + key_hex(g.key) + "  ";
    out += g.expired(now) ? "expired " : "until ";
    out += g.expires != 0 ? std::to_string(g.expires) + " (unix seconds)" : std::string("no end");
    if (g.roots) out += "  " + std::to_string(g.roots->size()) + " folders";
    if (g.guest) out += "  guest";
    out += "\n";
  }
  if (!readable) out += "(the timed list cannot be read)\n";
  print(out);
  return 0;
}

}  // namespace

int run_serve(const args& a, bool test_hooks) {
#if !defined(_WIN32)
  // It faces the internet, and its keys would then be root's: it runs as its own unprivileged user.
  if (!test_hooks && ::geteuid() == 0) return fail("it must not run as root; run it as its own user.", 1);
#endif
  if (sodium_init() < 0) return fail("could not start its cryptography.", 1);
  const std::string data = data_folder(a);
  if (data.empty()) return fail("no data folder (give --data DIR).", 1);
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(data.data()), data.size())), ec);
  auto store = open_store(data);
  if (!store) return fail("its secrets folder must be this user's alone (mode 700).", 1);
  switch (a.serve) {
    case serve_action::run:
      return run_service(a, test_hooks, data, *store);
    case serve_action::init:
      return run_init(a, *store);
    case serve_action::status:
      return run_status(*store);
    case serve_action::grant:
      return run_grant(a, *store, data);
    case serve_action::passphrase_set:
      return run_passphrase_set(a, *store);
    case serve_action::passphrase_reset:
      return run_passphrase_reset(a, *store);
    case serve_action::devices:
      return run_devices(*store);
    case serve_action::add:
      return run_add(a, *store, data);
    case serve_action::revoke:
      return run_revoke(a, *store, data);
    case serve_action::pair_code:
      return run_pair_code(a, *store, data);
    case serve_action::disown:
      return run_disown(*store);
  }
  return 2;
}

}  // namespace mv::driftwire::cli
