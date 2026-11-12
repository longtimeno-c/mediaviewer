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
#include "wire/known.h"
#include "wire/net.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "addons/driftwire/transfer.h"
#include "core/json.h"

#if !defined(_WIN32)
#include <sys/stat.h>
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

int run_init(secrets::store& store) {
  std::array<std::uint8_t, 32> pub{}, key{};
  bool made = false;
  if (!serve::set_up(store, pub, key, made)) return fail("could not write the service's keys.", 1);
  // The key is shown once, when made; afterwards only the identity is.
  std::string out = "identity " + key_hex(pub) + "\n";
  if (made) out += "access-key " + key_hex(key) + "\n";
  sodium_memzero(key.data(), key.size());
  print(out);
  return 0;
}

int run_add(const args& a, secrets::store& store, const std::string& data) {
  const auto key = parse_key_hex(a.serve_key);
  if (!key) return fail("that is not a device key (64 hex digits).", 2);
  const auto lock = net::try_lock_instance(data + "/serve-devices.lock");
  if (!lock) return fail("the device list is being changed by something else; try again.", 1);
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
  const auto key = parse_key_hex(a.serve_key);
  const auto before = devices.size();
  std::erase_if(devices, [&](const known::device& d) { return (key && d.key == *key) || d.name == a.serve_key; });
  if (devices.size() == before) return fail("no such device.", 1);
  // save() does not report a last entry it could not erase: read it back.
  std::vector<known::device> kept;
  if (!known::save(store, devices) || !load_devices(store, kept) || kept.size() != devices.size()) {
    return fail("could not save the device list; the device may still be paired.", 1);
  }
  print("revoked\n");
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
  const std::string code = serve::issue_pair_code(store, seconds, std::time(nullptr));
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
  if (devices.empty()) {
    print("No paired devices.\n");
    return 0;
  }
  std::string out;
  for (const auto& d : devices) out += d.name + "  " + key_hex(d.key) + "\n";
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
      return run_init(*store);
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
