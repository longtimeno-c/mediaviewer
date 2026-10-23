// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over the freedesktop Secret Service (GNOME Keyring, KWallet,
// KeePassXC; inside a Flatpak, libsecret's Secret portal), through libsecret
// loaded at run time: nothing to build against, and a machine without it (a
// server, a container, a session with no keyring) has no store, so the
// identity is that run's and nothing is remembered. Items use the schema
// "io.github.longtimeno_c.Driftwire" with two attributes, service ("MediaViewer
// Driftwire", "Driftwire CLI") and name, in the default collection (the login
// keyring, which PAM unlocks at login).
//
// The Secret Service has no per-app access control: any process of the same
// user on the session bus can read or plant an item. So, as with DPAPI on
// Windows (#501), each value goes in sealed (crypto_secretbox) under a key
// derived from "<service> secrets v1", and one that does not open is refused,
// not believed: a value cannot be read by listing the keyring (Seahorse
// shows only the sealed text), and an item another program wrote in plain
// does not open. It is not a boundary against a program of the same user that
// does the same (the constant is in the source).
//
// Never blocking for long:
//   - with no session bus nothing is loaded, so GLib never starts dbus-launch
//     (X11) or waits for a bus that is not there (a container);
//   - each call has its own GCancellable and a watchdog that cancels it after
//     kCallMs (an unlock prompt nobody answers, a daemon that does not reply);
//   - the first use probes the service once, with a lookup that matches
//     nothing (so no unlock prompt); a failed probe is no store for the rest
//     of the process.
// libsecret's *_sync calls run a main context of their own, so they need no
// GLib loop on the calling worker. The library is never unloaded (GLib's type
// system cannot be).
#include <dlfcn.h>
#include <sodium.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::secrets {

namespace {

constexpr int kCallMs = 10'000;

// The C types of libsecret 0.18+ and GLib (secret-schema.h; a stable ABI).
struct g_error {
  std::uint32_t domain;  // GQuark
  int code;
  char* message;
};
struct schema_attribute {
  const char* name;
  int type;  // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct secret_schema {
  const char* name;
  int flags;  // SECRET_SCHEMA_NONE: the xdg:schema attribute is matched too
  schema_attribute attributes[32];
  int reserved;
  void* reserved1;
  void* reserved2;
  void* reserved3;
  void* reserved4;
  void* reserved5;
  void* reserved6;
  void* reserved7;
};

const secret_schema kSchema{"io.github.longtimeno_c.Driftwire", 0, {{"service", 0}, {"name", 0}, {nullptr, 0}}, 0,
                            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

// The attributes are NULL-terminated name/value pairs after the fixed ones.
using store_fn = int (*)(const secret_schema*, const char* collection, const char* label, const char* password,
                         void* cancellable, g_error**, ...);
using lookup_fn = char* (*)(const secret_schema*, void* cancellable, g_error**, ...);
using clear_fn = int (*)(const secret_schema*, void* cancellable, g_error**, ...);
using password_free_fn = void (*)(char*);  // wipes, then frees
using cancellable_new_fn = void* (*)();
using cancellable_cancel_fn = void (*)(void*);  // thread-safe
using object_unref_fn = void (*)(void*);
using error_free_fn = void (*)(g_error*);

struct api {
  store_fn store = nullptr;
  lookup_fn lookup = nullptr;
  clear_fn clear = nullptr;
  password_free_fn password_free = nullptr;
  cancellable_new_fn cancellable_new = nullptr;
  cancellable_cancel_fn cancel = nullptr;
  object_unref_fn unref = nullptr;
  error_free_fn error_free = nullptr;
};

// One call into libsecret at a time in this process.
std::mutex& calls() {
  static std::mutex m;
  return m;
}

// Runs `call(cancellable)` under the watchdog; `call` reports an error by
// setting the g_error it is given, which is freed here. False on an error,
// a cancel (the watchdog's), or when the watchdog cannot start.
template <typename F>
bool bounded(const api& a, F&& call) {
  std::lock_guard serial(calls());
  void* c = a.cancellable_new();
  if (!c) return false;
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  std::thread dog;
  try {
    dog = std::thread([&] {
      std::unique_lock lock(m);
      if (!cv.wait_for(lock, std::chrono::milliseconds(kCallMs), [&] { return done; })) a.cancel(c);
    });
  } catch (...) {
    a.unref(c);
    return false;
  }
  g_error* e = nullptr;
  call(c, &e);
  {
    std::lock_guard lock(m);
    done = true;
  }
  cv.notify_one();
  dog.join();
  a.unref(c);
  if (!e) return true;
  a.error_free(e);
  return false;
}

// A session bus to talk to, without asking GLib (which would start one).
bool session_bus_present() {
  if (const char* addr = std::getenv("DBUS_SESSION_BUS_ADDRESS"); addr && *addr) return true;
  const char* run = std::getenv("XDG_RUNTIME_DIR");
  if (!run || *run != '/') return false;
  struct stat st {};
  return ::stat((std::string(run) + "/bus").c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

template <typename T>
bool find(void* lib, const char* name, T& out) {
  out = reinterpret_cast<T>(::dlsym(lib, name));
  return out != nullptr;
}

// libsecret, loaded and probed; nullptr where there is no working service.
const api* load_and_probe() {
  if (!session_bus_present()) return nullptr;
  void* lib = ::dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return nullptr;
  static api a;
  // GLib's own symbols come through libsecret's dependencies.
  const bool all = find(lib, "secret_password_store_sync", a.store) &&
                   (find(lib, "secret_password_lookup_nonpageable_sync", a.lookup) ||
                    find(lib, "secret_password_lookup_sync", a.lookup)) &&
                   find(lib, "secret_password_clear_sync", a.clear) &&
                   find(lib, "secret_password_free", a.password_free) &&
                   find(lib, "g_cancellable_new", a.cancellable_new) &&
                   find(lib, "g_cancellable_cancel", a.cancel) && find(lib, "g_object_unref", a.unref) &&
                   find(lib, "g_error_free", a.error_free);
  if (!all) return nullptr;  // never dlclose'd: GLib cannot unload
  // A lookup that matches nothing: the service answers, and no item is
  // there to unlock, so no prompt. Not found (NULL, no error) is a working store.
  char* found = nullptr;
  const bool ok = bounded(a, [&](void* c, g_error** e) {
    found = a.lookup(&kSchema, c, e, "service", "Driftwire probe", "name", "none", static_cast<const char*>(nullptr));
  });
  if (found) a.password_free(found);
  return ok ? &a : nullptr;
}

// Loaded and probed once per process.
const api* library() {
  static const api* const a = load_and_probe();
  return a;
}

class secret_service final : public store {
 public:
  secret_service(const api& a, std::string service) : a_(a), service_(std::move(service)) {
    const std::string context = service_ + " secrets v1";
    crypto_generichash(key_, sizeof key_, reinterpret_cast<const unsigned char*>(context.data()), context.size(),
                       nullptr, 0);
  }
  ~secret_service() override { sodium_memzero(key_, sizeof key_); }

  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue) return false;
    // nonce, then the box (its tag and the value), as base64 text.
    std::vector<unsigned char> sealed(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + value.size());
    randombytes_buf(sealed.data(), crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(sealed.data() + crypto_secretbox_NONCEBYTES, value.data(), value.size(), sealed.data(),
                          key_);
    std::string text(sodium_base64_ENCODED_LEN(sealed.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(text.data(), text.size(), sealed.data(), sealed.size(), sodium_base64_VARIANT_ORIGINAL);
    // The key is no secret (it comes from a constant), so the sealed bytes
    // are wiped as the value would be (#501), as DPAPI's are on Windows.
    sodium_memzero(sealed.data(), sealed.size());
    const std::string label = service_ + " " + name;
    int stored = 0;
    const bool ok = bounded(a_, [&](void* c, g_error** e) {
      stored = a_.store(&kSchema, "default", label.c_str(), text.c_str(), c, e, "service", service_.c_str(), "name",
                        name.c_str(), static_cast<const char*>(nullptr));
    });
    sodium_memzero(text.data(), text.size());
    return ok && stored != 0;
  }

  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    char* text = nullptr;
    const bool ok = bounded(a_, [&](void* c, g_error** e) {
      text = a_.lookup(&kSchema, c, e, "service", service_.c_str(), "name", name.c_str(),
                       static_cast<const char*>(nullptr));
    });
    if (!text) return std::nullopt;  // not there, an error, or cancelled
    std::optional<std::vector<std::uint8_t>> out;
    if (ok) out = open(text);
    a_.password_free(text);
    return out;
  }

  bool erase(const std::string& name) override {
    int removed = 0;
    const bool ok = bounded(a_, [&](void* c, g_error** e) {
      removed = a_.clear(&kSchema, c, e, "service", service_.c_str(), "name", name.c_str(),
                         static_cast<const char*>(nullptr));
    });
    return ok && removed != 0;
  }

 private:
  // The value sealed under this service's key; nullopt for anything else.
  std::optional<std::vector<std::uint8_t>> open(const char* text) const {
    const std::size_t len = std::char_traits<char>::length(text);
    std::vector<unsigned char> sealed(len / 4 * 3 + 3);
    std::size_t n = 0;
    if (sodium_base642bin(sealed.data(), sealed.size(), text, len, nullptr, &n, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        n < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES ||
        n - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES > kMaxValue) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> plain(n - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES);
    const bool opened = crypto_secretbox_open_easy(plain.data(), sealed.data() + crypto_secretbox_NONCEBYTES,
                                                   n - crypto_secretbox_NONCEBYTES, sealed.data(), key_) == 0;
    sodium_memzero(sealed.data(), sealed.size());
    if (!opened) return std::nullopt;  // not sealed by Driftwire under this service: refused
    return plain;
  }

  const api& a_;
  std::string service_;
  unsigned char key_[crypto_secretbox_KEYBYTES];
};

}  // namespace

std::unique_ptr<store> os_store(const std::string& service) {
  if (sodium_init() < 0) return nullptr;
  const api* a = library();
  if (!a) return nullptr;
  return std::make_unique<secret_service>(*a, service);
}

std::unique_ptr<store> os_store() { return os_store(kAppService); }

}  // namespace mv::driftwire::secrets
