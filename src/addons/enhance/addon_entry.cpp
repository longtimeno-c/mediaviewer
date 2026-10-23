// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_enhance's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/plans/enhance.md E1; docs/design/25 PRs 58-59).
//
// Enhance's one contribution in E1 is its page in Settings -> Add-ons,
// "models" (place "settings"): this machine, and per kind the models that
// fit it, with Download, Cancel, Resume and Delete. Loading does nothing:
// no catalogue read, no machine probe, no hash, no thread. The page's first
// open reads the catalogue from the add-on's own verified folder (self_dir.h)
// and makes the download store (downloads.h) over its data folder; every open
// probes the machine again, for the disk's free space now.
//
// Threads. The host's worker for this add-on brings every open, close and
// click here, one at a time. The store calls back on any thread; that only
// marks the page dirty for the poster, a thread of this add-on's made at the
// first open, which posts the tree (or, when only bytes moved, a diff) for
// the open page's generation at most four times a second, and says busy to
// the host while a download runs (Restart now and an update of this add-on
// wait for it). A post for a closed or reopened page is refused by the host
// and dropped here.
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path or a file name, a model's included (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/enhance/catalog.h"
#include "addons/enhance/downloads.h"
#include "addons/enhance/machine.h"
#include "addons/enhance/self_dir.h"
#include "addons/enhance/view.h"

#ifndef MV_ENHANCE_VERSION
#define MV_ENHANCE_VERSION "0.0.0-dev"
#endif

namespace {

namespace en = mv::enhance;

// The host tables this build understands (the manifest says the same): v3
// carries the screen and slot calls.
constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "models";  // manifest.json, place "settings"
// What the host's "Waiting for … to finish" names while a download runs.
constexpr const char* kBusy = "an Enhance model download";
// At most four posts a second; what changes meanwhile rides with the next.
constexpr auto kPostGap = std::chrono::milliseconds(250);

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

// #510: busy_post is appended to the host table; an older host has none.
bool has_busy_post(const mv_host_api* h) {
  return h->struct_size >= offsetof(mv_host_api, busy_post) + sizeof(h->busy_post) && h->busy_post != nullptr;
}

bool running(const std::vector<en::kind_status>& status) {
  return std::any_of(status.begin(), status.end(), [](const en::kind_status& s) {
    return s.state == en::download_state::queued || s.state == en::download_state::downloading ||
           s.state == en::download_state::waiting;
  });
}

struct addon_state {
  const mv_host_api* host = nullptr;

  // Made at the page's first open, on the host's worker; they live until
  // shutdown. The poster starts after the store and stops before it.
  std::string data_dir;
  std::unique_ptr<en::model_store> store;
  std::thread poster;

  // The page as the host holds it, and the poster's wake-ups.
  std::mutex m;
  std::condition_variable cv;
  bool open = false;
  uint64_t generation = 0;
  en::machine here;  // probed at the open
  bool dirty = false;
  bool quit = false;
  std::chrono::steady_clock::time_point last_post{};

  // One post at a time, in order: `shown` is what the host's tree shows.
  // Taken before `m`, never inside it.
  std::mutex post_m;
  std::vector<en::kind_status> shown;
  uint64_t shown_generation = 0;
  bool busy = false;

  // The add-on's data folder (made by the host), or "".
  std::string host_data_dir() const {
    char buf[4096];
    if (host->data_dir(host->host, buf, sizeof buf) != MV_OK || !*buf) return {};
    return buf;
  }

  // The store and the poster, at the first open. False without a data
  // folder: the weights have nowhere to go (asked again at the next open).
  bool ready() {
    if (!store) {
      std::string data = host_data_dir();
      if (data.empty()) return false;
      // An empty catalogue (all three kinds, no model) when the version
      // folder has none or it does not read.
      en::catalog cat;
      if (const std::string dir = en::self_dir(); !dir.empty()) {
        auto read = en::read_catalog(dir);
        if (read) {
          cat = std::move(*read);
        } else if (host->log) {
          host->log(host->host, 1, "enhance: the model catalogue could not be read");
        }
      }
      data_dir = data;
      store = std::make_unique<en::model_store>(
          std::move(data), std::move(cat), [] { return mv::ai::make_http_transport(); },
          [d = data_dir] { return en::probe(d); }, [this] { changed(); });
    }
    if (!poster.joinable()) poster = std::thread([this] { post_loop(); });
    return true;
  }

  // The store's callback, on any thread: never posts from there (the
  // download worker runs at background priority, and a post takes the
  // host's lock), only wakes the poster.
  void changed() noexcept {
    try {
      {
        std::lock_guard lock(m);
        dirty = true;
      }
      cv.notify_one();
    } catch (...) {
    }
  }

  void post_loop() noexcept {
    try {
      std::unique_lock lock(m);
      for (;;) {
        cv.wait(lock, [this] { return quit || dirty; });
        if (quit) return;
        const auto due = last_post + kPostGap;
        if (std::chrono::steady_clock::now() < due && cv.wait_until(lock, due, [this] { return quit; })) return;
        dirty = false;
        lock.unlock();
        try {
          refresh(false);
        } catch (...) {
          // Out of memory building a tree: the next change tries again.
        }
        lock.lock();
      }
    } catch (...) {
    }
  }

  // The store's state to the host: busy or not, and the open page's tree
  // (`whole`) or, when only bytes moved since the tree it holds, a diff.
  void refresh(bool whole) {
    std::lock_guard post(post_m);
    const std::vector<en::kind_status> now = store->status();
    const bool on = running(now);
    if (on != busy && has_busy_post(host) && host->busy_post(host->host, on ? kBusy : "") == MV_OK) busy = on;
    bool is_open = false;
    uint64_t gen = 0;
    en::machine at;
    {
      std::lock_guard lock(m);
      is_open = open;
      gen = generation;
      if (is_open) at = here;
      last_post = std::chrono::steady_clock::now();
    }
    if (!is_open) {
      shown.clear();
      return;
    }
    if (!whole && shown_generation == gen && !shown.empty()) {
      const std::string diff = en::progress_diff(shown, now);
      if (diff == "[]") return;
      if (!diff.empty()) {
        const mv_status r = host->screen_diff(host->host, kScreen, gen, diff.c_str());
        if (r == MV_OK) shown = now;
        if (r != MV_ERR_INVALID_ARG) return;  // posted, or the page closed or opened again
      }
    }
    const std::string tree = en::models_tree(store->models(), at, now);
    if (host->screen_post(host->host, kScreen, gen, tree.c_str()) == MV_OK) {
      shown = now;
      shown_generation = gen;
    }
  }

  void stop() noexcept {
    try {
      {
        std::lock_guard lock(m);
        quit = true;
      }
      cv.notify_all();
      if (poster.joinable()) poster.join();
    } catch (...) {
    }
    // Stops a run (the fetcher sees it between chunks) and joins the
    // download worker; its last callbacks only mark the page dirty.
    store.reset();
  }
};

addon_state* g = nullptr;

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    if (!g->ready()) return MV_ERR_IO;
    en::machine here = en::probe(g->data_dir);
    {
      std::lock_guard lock(g->m);
      g->open = true;
      g->generation = r->generation;
      g->here = std::move(here);
    }
    g->refresh(true);
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->screen_id || std::strcmp(e->screen_id, kScreen) != 0) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      std::lock_guard lock(g->m);
      if (g->generation == e->generation) g->open = false;
      return MV_OK;
    }
    if (e->kind != MV_SLOT_EVENT_CLICK || !e->node_id || !g->store) return MV_OK;
    {
      std::lock_guard lock(g->m);
      if (!g->open || g->generation != e->generation) return MV_OK;  // a click on a page that has gone
    }
    // The store tells the poster what changed; nothing is held here while it runs.
    const en::action a = en::action_of(e->node_id);
    switch (a.what) {
      case en::verb::download: (void)g->store->download(a.k, a.model); break;
      case en::verb::cancel: g->store->cancel(a.k); break;
      case en::verb::resume: g->store->resume(a.k); break;
      case en::verb::remove: g->store->remove(a.k); break;
      case en::verb::none: break;
    }
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  // The poster and the download worker stop before this returns, so the
  // library can go.
  if (!g) return;
  g->stop();
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out ||
        host->struct_size < offsetof(mv_host_api, screen_diff) + sizeof(host->screen_diff) || !host->data_dir ||
        !host->screen_post || !host->screen_diff) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    auto state = std::make_unique<addon_state>();
    state->host = host;
    g = state.release();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "enhance";
    out->version = MV_ENHANCE_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}

// #507: removed without keeping its data. Enhance keeps nothing outside its
// data folder (the weights are under data/cache, its choices in
// data/enhance.json), which the host deletes.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_forget(const char* /*data_dir_utf8*/) { return MV_OK; }
