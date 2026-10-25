// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// power.h on Linux: a systemd-logind inhibitor ("sleep", mode "block") taken
// over the system bus, as systemd-inhibit takes one (systemd-inhibit --list
// shows it), held as long as its file descriptor is open. libsystemd, or
// elogind's libelogind (the same sd-bus API), is loaded at run time: a machine
// with neither, without a system bus (a container, WSL), or whose polkit
// refuses the inhibitor gets nullptr, and the transfer runs anyway.
//
// "sleep", not "idle": an idle inhibitor would also keep the screen from
// blanking and locking. A closed lid still suspends (logind's
// LidSwitchIgnoreInhibited=yes), as on the other OSes.
//
// Bounded at kCallMs in all: the connection is our own, the call is sent
// without waiting and the bus is driven here with a deadline. (sd_bus_call and
// sd_bus_flush wait for the connection to be up with no bound of their own: a
// bus that accepts and never answers would hold them for sd-bus's 90 s
// authentication timeout.) The bus is closed without a flush: once logind has
// answered, nothing of ours is left to send. Called on a transfer's thread,
// never the UI or render thread.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "addons/driftwire/power.h"

namespace mv::driftwire::power {

namespace {

constexpr int kCallMs = 2000;
constexpr const char* kDefaultSystemBus = "/run/dbus/system_bus_socket";

// sd-bus's C entry points (systemd/sd-bus.h; a stable ABI). The handler's
// third argument is an sd_bus_error*, unused here.
using handler_fn = int (*)(void* m, void* userdata, void* ret_error);
using bus_new_fn = int (*)(void** bus);
using bus_set_address_fn = int (*)(void* bus, const char* address);
using bus_set_bus_client_fn = int (*)(void* bus, int b);
using bus_start_fn = int (*)(void* bus);
using bus_process_fn = int (*)(void* bus, void** ret);
using bus_wait_fn = int (*)(void* bus, std::uint64_t timeout_usec);
using bus_close_fn = void (*)(void* bus);
using bus_unref_fn = void* (*)(void* bus);
using new_call_fn = int (*)(void* bus, void** m, const char* destination, const char* path, const char* interface,
                            const char* member);
using append_fn = int (*)(void* m, const char* types, ...);
using call_async_fn = int (*)(void* bus, void** slot, void* m, handler_fn callback, void* userdata,
                              std::uint64_t usec);
using read_fn = int (*)(void* m, const char* types, ...);
using is_error_fn = int (*)(void* m, const char* name);
using message_unref_fn = void* (*)(void* m);

struct api {
  bus_new_fn bus_new = nullptr;
  bus_set_address_fn set_address = nullptr;
  bus_set_bus_client_fn set_bus_client = nullptr;
  bus_start_fn start = nullptr;
  bus_process_fn process = nullptr;
  bus_wait_fn wait = nullptr;
  bus_close_fn close = nullptr;
  bus_unref_fn unref = nullptr;
  new_call_fn new_call = nullptr;
  append_fn append = nullptr;
  call_async_fn call_async = nullptr;
  read_fn read = nullptr;
  is_error_fn is_error = nullptr;
  message_unref_fn message_unref = nullptr;
};

template <typename T>
bool find(void* lib, const char* name, T& out) {
  out = reinterpret_cast<T>(::dlsym(lib, name));
  return out != nullptr;
}

const api* load() {
  static api a;
  for (const char* name : {"libsystemd.so.0", "libelogind.so.0"}) {
    void* lib = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib) continue;
    if (find(lib, "sd_bus_new", a.bus_new) && find(lib, "sd_bus_set_address", a.set_address) &&
        find(lib, "sd_bus_set_bus_client", a.set_bus_client) && find(lib, "sd_bus_start", a.start) &&
        find(lib, "sd_bus_process", a.process) && find(lib, "sd_bus_wait", a.wait) &&
        find(lib, "sd_bus_close", a.close) && find(lib, "sd_bus_unref", a.unref) &&
        find(lib, "sd_bus_message_new_method_call", a.new_call) && find(lib, "sd_bus_message_append", a.append) &&
        find(lib, "sd_bus_call_async", a.call_async) && find(lib, "sd_bus_message_read", a.read) &&
        find(lib, "sd_bus_message_is_method_error", a.is_error) &&
        find(lib, "sd_bus_message_unref", a.message_unref)) {
      return &a;
    }
    a = api{};  // kept loaded: a library is not unloaded under code it may have started
  }
  return nullptr;
}

// Loaded once per process.
const api* library() {
  static const api* const a = load();
  return a;
}

// The system bus's address, where there is one to talk to (nothing is
// loaded otherwise).
const char* system_bus() {
  if (const char* addr = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"); addr && *addr) return addr;
  struct stat st {};
  return ::stat(kDefaultSystemBus, &st) == 0 && S_ISSOCK(st.st_mode) ? "unix:path=/run/dbus/system_bus_socket"
                                                                      : nullptr;
}

// logind's answer, filled in by on_reply while the bus is driven.
struct answer {
  const api* a = nullptr;
  bool done = false;
  int fd = -1;  // a copy of the lock's descriptor, ours to close
};

// Inhibit's reply, or the error in its place (refused by polkit, no logind,
// timed out). The reply's own descriptor goes with the reply, so it is copied.
int on_reply(void* m, void* userdata, void* /*ret_error*/) {
  auto& got = *static_cast<answer*>(userdata);
  got.done = true;
  int theirs = -1;
  if (got.a->is_error(m, nullptr) == 0 && got.a->read(m, "h", &theirs) >= 0 && theirs >= 0) {
    got.fd = ::fcntl(theirs, F_DUPFD_CLOEXEC, 3);
  }
  return 0;
}

// Inhibit on a new connection to `address`, within kCallMs; -1 when there is
// no lock by then.
int inhibit(const api& a, const char* address, const char* who, const std::string& why) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCallMs);
  void* bus = nullptr;
  if (a.bus_new(&bus) < 0 || !bus) return -1;
  answer got;
  got.a = &a;
  void* m = nullptr;
  // Started and sent without waiting: the Hello and the call queue up until
  // the connection is through; the slot is the bus's own (nullptr).
  if (a.set_address(bus, address) >= 0 && a.set_bus_client(bus, 1) >= 0 && a.start(bus) >= 0 &&
      a.new_call(bus, &m, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                 "Inhibit") >= 0 &&
      a.append(m, "ssss", "sleep", who, why.c_str(), "block") >= 0 &&
      a.call_async(bus, nullptr, m, on_reply, &got, static_cast<std::uint64_t>(kCallMs) * 1000) >= 0) {
    while (!got.done) {
      const int r = a.process(bus, nullptr);
      if (r < 0) break;  // the connection failed or closed
      if (r > 0) continue;  // more may be ready at once
      const auto left = std::chrono::duration_cast<std::chrono::microseconds>(until - std::chrono::steady_clock::now());
      if (left.count() <= 0) break;
      if (a.wait(bus, static_cast<std::uint64_t>(left.count())) < 0) break;
    }
  }
  if (m) a.message_unref(m);
  // No flush (it waits unbounded on a bus that is not up). Closing drops the
  // pending call, if any, without calling on_reply.
  a.close(bus);
  a.unref(bus);
  return got.fd;
}

class inhibitor final : public stay_awake {
 public:
  explicit inhibitor(int fd) : fd_(fd) {}
  ~inhibitor() override { ::close(fd_); }  // logind lets go once no copy is open

 private:
  int fd_;
};

}  // namespace

std::unique_ptr<stay_awake> hold(bool receiving) noexcept {
  int fd = -1;
  try {
    const char* address = system_bus();
    if (!address) return nullptr;
    const api* a = library();
    if (!a) return nullptr;
    fd = inhibit(*a, address, owner(), reason(receiving));
    if (fd < 0) return nullptr;
    return std::make_unique<inhibitor>(fd);
  } catch (...) {
    if (fd >= 0) ::close(fd);
    return nullptr;
  }
}

}  // namespace mv::driftwire::power
