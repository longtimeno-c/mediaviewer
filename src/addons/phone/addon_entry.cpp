// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_phone_access's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/plans/phone-access-addon.md; docs/design/25 PRs 58-59).
//
// Phone access is one screen, "settings" (place "settings"), which both hosts
// draw in Settings -> Phone access. The link (service.h) runs from load in the
// process that holds the feature's instance lock, so a library set up before
// carries on (the nearby copy, revokes the NAS has not heard) with Settings
// never opened. While the screen shows, a ticker on this add-on's own thread
// reads the link's state from memory and posts the tree when it changed; the
// QR is a surface drawn here. Events arrive on the host's worker for this
// add-on and become the link's requests.
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14).
// Nothing here logs a path, an address, a key or anything typed (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "addons/phone/bundled.h"
#include "addons/phone/qr.h"
#include "addons/phone/screens.h"
#include "addons/phone/self_host.h"
#include "addons/phone/service.h"
#include "wire/net.h"

#ifndef MV_PHONE_ACCESS_VERSION
#define MV_PHONE_ACCESS_VERSION "0.0.0-dev"
#endif

namespace {

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "settings";  // manifest.json, place "settings"
constexpr const char* kLockName = "instance.lock";
constexpr int kQrPixels = 480;  // the 240-point surface at 2x

template <typename Fn>
mv_status guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return MV_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

#define MV_PA_HAS(h, field) \
  ((h)->struct_size >= offsetof(mv_host_api, field) + sizeof((h)->field) && (h)->field != nullptr)

// The link's thread is detached at stop (it never waits on the NAS), so this
// library must never be unmapped under it: it holds a reference to itself
// that is never let go. A remove takes effect at the next launch.
void pin_self() noexcept {
#if defined(_WIN32)
  HMODULE self = nullptr;
  (void)::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&pin_self), &self);
#else
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void*>(&pin_self), &info) != 0 && info.dli_fname) {
    (void)::dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
  }
#endif
}

// The folder this library was loaded from (the add-on's verified version folder).
std::string module_dir() {
#if defined(_WIN32)
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_dir), &self)) {
    return {};
  }
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) return {};
  buf.resize(n);
  const auto slash = buf.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  buf.resize(slash);
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(len > 0 ? len : 0), '\0');
  if (len > 0) (void)::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), len, nullptr, nullptr);
  return out;
#else
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void*>(&module_dir), &info) == 0 || !info.dli_fname) return {};
  std::string path = info.dli_fname;
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
#endif
}

std::string join_native(const std::string& dir, const char* name) {
#if defined(_WIN32)
  return dir + "\\" + name;
#else
  return dir + "/" + name;
#endif
}

// The add-on's own driftwire (docs/plans/phone-access-addon.md §8), installed
// into the feature's bin folder, which survives add-on updates and is where the
// login entry points. Copied only when this version is not there yet; a
// running host restarts on its own (it ends 75 when its program changes).
void install_bundled(const std::string& feature) {
  const std::string mine = module_dir();
  if (mine.empty()) return;
  const std::string source = join_native(mine, "host");  // the program and what it loads
  auto got = mv::phone::bundled::install(source, mv::phone::bundled::bin_dir(feature), MV_PHONE_ACCESS_VERSION,
                                         mv::phone::bundled::rename_aside_here());
  if (got) mv::phone::self_host::set_bundled(got->program);
  // A build without the program (a developer's): the card finds an installed one.
}

mv::phone::card::computer this_computer() {
  switch (mv::phone::self_host::this_os()) {
    case mv::phone::self_host::os::mac: return mv::phone::card::computer::mac;
    case mv::phone::self_host::os::windows: return mv::phone::card::computer::pc;
    default: return mv::phone::card::computer::other;
  }
}

// A UTF-8 path as the OS's (wide on Windows).
std::filesystem::path fs_path(const std::string& utf8) {
#if defined(_WIN32)
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) (void)::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return std::filesystem::path(w);
#else
  return std::filesystem::path(utf8);
#endif
}

std::int64_t now_s() { return static_cast<std::int64_t>(std::time(nullptr)); }

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<mv::driftwire::net::instance_lock> lock;
  bool primary = false;

  // The screen, under m.
  std::mutex m;
  mv::phone::card::screen card{this_computer()};
  bool open = false;
  std::uint64_t generation = 0;
  std::string last_tree, last_qr;

  std::condition_variable cv;
  bool ticker_stop = false;
  std::thread ticker;

  // Local search through the host (catalogue_*): "" while it is not loaded.
  mv::phone::catalogue_source source() const {
    mv::phone::catalogue_source s;
    const mv_host_api* h = host;
    s.publish = [h](const std::string& destination) -> mv::expected {
      if (!MV_PA_HAS(h, catalogue_publish)) return mv::err(mv::status::not_found);
      const mv_status r = h->catalogue_publish(h->host, destination.c_str());
      if (r == MV_OK) return {};
      return mv::err(static_cast<mv::status>(r));
    };
    s.cancel = [h] {
      if (MV_PA_HAS(h, catalogue_cancel)) (void)h->catalogue_cancel(h->host);
    };
    s.json = [h]() -> std::string {
      if (!MV_PA_HAS(h, catalogue_json)) return {};
      std::vector<char> buf(16 * 1024);
      for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t needed = 0;
        const mv_status r = h->catalogue_json(h->host, buf.data(), static_cast<uint32_t>(buf.size()), &needed);
        if (r == MV_OK) return std::string(buf.data());
        if (r != MV_ERR_INVALID_ARG || needed <= buf.size()) return {};
        buf.resize(needed + 1024);
      }
      return {};
    };
    return s;
  }

  // Builds and posts under m: only what changed reaches the host.
  void post_locked() {
    if (!open) return;
    const std::string tree = card.build(mv::phone::app_state_json(), now_s());
    // A section shown for the first time asks for its list ([no-block]: queued for the link).
    for (const std::string& r : card.take_queued()) (void)mv::phone::app_request(r);
    if (tree != last_tree) {
      if (host->screen_post(host->host, kScreen, generation, tree.c_str()) == MV_OK) last_tree = tree;
    }
    const std::string& qr = card.qr_text();
    if (qr != last_qr && !qr.empty()) {
      if (auto code = mv::phone::qr::encode(qr)) {
        std::vector<std::uint8_t> bgra;
        int side = 0;
        mv::phone::qr::draw_bgra(*code, kQrPixels, bgra, side);
        if (side > 0 &&
            host->surface_post(host->host, kScreen, mv::phone::card::screen::kQrSurface, generation, bgra.data(),
                               static_cast<uint32_t>(side), static_cast<uint32_t>(side),
                               static_cast<uint32_t>(side) * 4u) == MV_OK) {
          last_qr = qr;
        }
      }
    }
    if (qr.empty()) last_qr.clear();
  }

  // While the screen shows: twice a second (a code showing counts down by the
  // second); the state is read from memory, nothing here waits on the network.
  void tick_loop() {
    std::unique_lock lock(m);
    while (!ticker_stop) {
      cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return ticker_stop; });
      if (ticker_stop) break;
      post_locked();
    }
  }

  void opened(std::uint64_t g) {
    {
      std::lock_guard lock(m);
      generation = g;
      open = true;
      last_tree.clear();
      last_qr.clear();
      post_locked();
      if (!ticker.joinable()) {
        ticker_stop = false;
        ticker = std::thread([this] { tick_loop(); });
      }
    }
  }

  void closed() {
    std::thread t;
    {
      std::lock_guard lock(m);
      open = false;
      card.forget_secrets();  // typed and never sent: gone when Settings closes
      ticker_stop = true;
      t = std::move(ticker);
    }
    cv.notify_all();
    if (t.joinable()) t.join();
  }
};

addon_state* g = nullptr;

void send_all(const std::vector<std::string>& requests) {
  for (const std::string& r : requests) (void)mv::phone::app_request(r);
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    // A fresh look at this computer's server and at Local search (loaded since?).
    if (g->primary) {
      (void)mv::phone::app_request("{\"do\":\"host_status\"}");
      mv::phone::set_app_source(g->source());
    }
    g->opened(r->generation);
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->screen_id || std::strcmp(e->screen_id, kScreen) != 0 || !e->node_id) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      g->closed();
      return MV_OK;
    }
    std::vector<std::string> requests;
    {
      std::lock_guard lock(g->m);
      requests = g->card.on_event(e->node_id, e->kind, e->value_json ? e->value_json : "", now_s());
    }
    send_all(requests);  // [no-block]: queued for the link's thread
    std::lock_guard lock(g->m);
    g->post_locked();
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  if (!g) return;
  g->closed();
  // Local search is no longer called and the link stops; it never waits on
  // the NAS (its thread finishes on its own: this library stays mapped).
  mv::phone::stop_app();
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out || !MV_PA_HAS(host, screen_show) ||
        !host->screen_post || !host->surface_post) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    pin_self();
    auto state = std::make_unique<addon_state>();
    state->host = host;
    // The feature's folder, where the app kept it before the add-on (a library
    // set up then keeps its id); the first window's process to load runs it.
    if (auto dir = mv::phone::feature_dir()) {
      std::error_code ec;
      std::filesystem::create_directories(fs_path(*dir), ec);
      mv::phone::set_app_dir(*dir);
#if defined(_WIN32)
      state->lock = mv::driftwire::net::try_lock_instance(*dir + "\\" + kLockName);
#else
      state->lock = mv::driftwire::net::try_lock_instance(*dir + "/" + kLockName);
#endif
      state->primary = state->lock != nullptr;
      // The first window's process installs it: two would race on the same file.
      if (state->primary) install_bundled(*dir);
    }
    g = state.release();
    // The host loads slot add-ons after first pixel, off the foreground queue:
    // the link's own thread does the rest.
    mv::phone::resume_app(g->primary);
    if (g->primary) mv::phone::set_app_source(g->source());
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "phone-access";
    out->version = MV_PHONE_ACCESS_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}

// Removed (kept data or not; docs/plans/phone-access-addon.md §8): the host
// this add-on set up stops and no longer starts at login, when its login entry
// runs the add-on's own copy; then that copy goes. The shared folders, the
// host's data and every pairing stay. A host the person runs from a separately
// installed driftwire is left alone. On the remover's worker.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_removed(const char* /*data_dir_utf8*/, uint32_t /*keep_data*/) {
  return guard([&] {
    auto dir = mv::phone::feature_dir();
    if (!dir) return MV_OK;
    const std::string bin = mv::phone::bundled::bin_dir(*dir);
    const std::string program = mv::phone::bundled::program_in(bin);
    mv::phone::self_host::where w = mv::phone::self_host::find();
    w.program = program;
    if (mv::phone::self_host::login_entry_runs(w, program)) {
      (void)mv::phone::self_host::run_host(w, {"start-at-login", "off"}, {}, 60000);
    }
    mv::phone::bundled::remove(bin);
    return MV_OK;
  });
}

// Removed without keeping its data: nothing more is forgotten here. The desk's key,
// the paired phones and the publisher key are what the NAS and every paired
// phone trust (docs/plans/phone-access-addon.md §3); they go only through the
// card's own Start over, which tells the NAS. The library's folder is not the
// add-on's data folder, so the library keeps its id.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_forget(const char* /*data_dir_utf8*/) { return MV_OK; }
