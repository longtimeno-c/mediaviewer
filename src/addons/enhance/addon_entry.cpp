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
// probes the machine again, for the disk's free space now, and reads the
// choices again, for what other windows chose since. The store asks whether
// the add-on is still installed (its version folder's manifest.json): a
// Remove in another window leaves this one loaded until it closes.
//
// Threads. The host's worker for this add-on brings every open, close and
// click here, one at a time. The store calls back on any thread; that only
// marks the page dirty for the poster, a thread of this add-on's made at the
// first open, which posts the tree for the open page's generation at most
// four times a second, or, when only bytes moved, a diff at most once a
// second and only when a line or a bar's whole percent changed (the Windows
// host rebuilds the page for each), and says busy to the host while a
// download runs (Restart now and an update of this add-on wait for it).
// While a kind's line depends on another window's download (stopped here,
// or that window's and not queued here), it looks again every kLockLook, as
// nothing here hears when that window starts or finishes; it posts only
// what changed. A post for a closed or reopened page is refused by the host
// and dropped here. A click is read against the status the page was drawn
// from: a card's main button keeps its id across Download, Cancel and
// Resume (view.h), so focus stays on it. A click on one whose meaning a post
// changed under it moments before (kClickSettle) does nothing: the host
// sends only the id, and the person pressed what the tree before showed
// (a double click on Cancel is not a Resume).
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path or a file name, a model's included (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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
// Bytes alone: at most a diff a second.
constexpr auto kDiffGap = std::chrono::seconds(1);
// Another window's download, looked at again (the store's lock_retry).
constexpr auto kLockLook = std::chrono::seconds(5);
// How long after a post changed what a main button does a click on it is
// taken for one on the button as it was.
constexpr auto kClickSettle = std::chrono::milliseconds(500);

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

// A line that says what another window's download means here: `stopped`
// (status() says other_window while one holds the lock) and other_window
// with nothing queued here (the store's own wait looks by itself).
bool follows_lock(const std::vector<en::kind_status>& status) {
  return std::any_of(status.begin(), status.end(), [](const en::kind_status& s) {
    return !s.model.empty() && (s.state == en::download_state::stopped ||
                                (s.state == en::download_state::other_window && !s.lock_wait));
  });
}

// Whether a main button of kind `at` does something else in `b` than in `a`:
// only the chosen model's card can (every other card's reads Download).
bool main_changed(const std::vector<en::kind_status>& a, const std::vector<en::kind_status>& b, std::size_t at) {
  for (const std::string* model : {&a[at].model, &b[at].model}) {
    if (model->empty()) continue;
    const std::string id = std::string("a.") + en::kind_id(b[at].k) + "." + *model;
    if (en::action_of(id, a).what != en::action_of(id, b).what) return true;
  }
  return false;
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
  std::string notice;  // a click that could not be saved, until the next click or open
  bool dirty = false;
  bool quit = false;
  std::chrono::steady_clock::time_point last_post{};
  // When the poster refreshes next with nothing changed: a diff held back,
  // or another look at another window's download.
  std::optional<std::chrono::steady_clock::time_point> again;

  // One post at a time, in order: `shown` is what the host's tree shows.
  // Taken before `m`, never inside it.
  std::mutex post_m;
  std::vector<en::kind_status> shown;
  uint64_t shown_generation = 0;
  std::string shown_notice;
  std::chrono::steady_clock::time_point last_diff{};  // or the tree that set the bars
  bool busy = false;
  // Per kind, when a post last changed what one of its main buttons does,
  // and the kind as shown before that post (all three kinds, by index).
  std::array<std::chrono::steady_clock::time_point, 3> changed_at{};
  std::vector<en::kind_status> shown_before;

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
      const std::string dir = en::self_dir();
      if (!dir.empty()) {
        auto read = en::read_catalog(dir);
        if (read) {
          cat = std::move(*read);
        } else if (host->log) {
          host->log(host->host, 1, "enhance: the model catalogue could not be read");
        }
      }
      en::store_options options;
      options.still_installed = [dir] {
        const std::string manifest = dir + "/manifest.json";
        std::error_code ec;
        return !dir.empty() &&
               std::filesystem::exists(std::filesystem::path(std::u8string(manifest.begin(), manifest.end())), ec);
      };
      data_dir = data;
      store = std::make_unique<en::model_store>(
          std::move(data), std::move(cat), [] { return mv::ai::make_http_transport(); },
          [d = data_dir] { return en::probe(d); }, [this] { changed(); }, std::move(options));
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
        // A change, or a refresh that is due (`again`, which an open or a
        // click may bring forward).
        while (!quit && !dirty) {
          if (!again) {
            cv.wait(lock);
          } else if (std::chrono::steady_clock::now() >= *again) {
            dirty = true;
          } else {
            cv.wait_until(lock, *again);
          }
        }
        if (quit) return;
        const auto due = last_post + kPostGap;
        if (std::chrono::steady_clock::now() < due && cv.wait_until(lock, due, [this] { return quit; })) return;
        dirty = false;
        again.reset();
        lock.unlock();
        std::optional<std::chrono::steady_clock::time_point> later;
        try {
          later = refresh(false);
        } catch (...) {
          // Out of memory building a tree: the next change tries again.
        }
        lock.lock();
        if (later && (!again || *later < *again)) again = later;
      }
    } catch (...) {
    }
  }

  // A refresh made off the poster (an open, a click): what it says is due
  // next is the poster's to do.
  void schedule(std::optional<std::chrono::steady_clock::time_point> later) {
    if (!later) return;
    {
      std::lock_guard lock(m);
      if (again && *again <= *later) return;
      again = later;
    }
    cv.notify_one();
  }

  // The store's state to the host: busy or not, and the open page's tree
  // (`whole`) or, when only bytes moved since the tree it holds, a diff.
  // Returns when to refresh again: a diff held back, or the next look at
  // another window's download.
  std::optional<std::chrono::steady_clock::time_point> refresh(bool whole) {
    std::lock_guard post(post_m);
    const std::vector<en::kind_status> now = store->status();
    const bool on = running(now);
    if (on != busy && has_busy_post(host) && host->busy_post(host->host, on ? kBusy : "") == MV_OK) busy = on;
    bool is_open = false;
    uint64_t gen = 0;
    en::machine at;
    std::string note;
    {
      std::lock_guard lock(m);
      is_open = open;
      gen = generation;
      if (is_open) {
        at = here;
        note = notice;
      }
      last_post = std::chrono::steady_clock::now();
    }
    if (!is_open) {
      shown.clear();
      return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::time_point> later = show(whole, now, gen, std::move(note), at);
    if (follows_lock(now)) {
      const auto look = std::chrono::steady_clock::now() + kLockLook;
      if (!later || look < *later) later = look;
    }
    return later;
  }

  // refresh() for the open page, under post_m: the diff or the tree. Returns
  // when a diff held back is due.
  std::optional<std::chrono::steady_clock::time_point> show(bool whole, const std::vector<en::kind_status>& now,
                                                            uint64_t gen, std::string note, const en::machine& at) {
    if (!whole && shown_generation == gen && !shown.empty() && note == shown_notice) {
      const std::string diff = en::progress_diff(shown, now);
      if (diff == "[]") return std::nullopt;
      if (!diff.empty()) {
        const auto t = std::chrono::steady_clock::now();
        if (t < last_diff + kDiffGap) return last_diff + kDiffGap;
        const mv_status r = host->screen_diff(host->host, kScreen, gen, diff.c_str());
        if (r == MV_OK) {
          shown = now;  // bytes alone: no button does anything else
          last_diff = t;
        }
        if (r != MV_ERR_INVALID_ARG) return std::nullopt;  // posted, or the page closed or opened again
      }
    }
    const en::model_store& s = *store;
    const std::string tree =
        en::models_tree(s.models(), at, now, [&s](const en::model& md) { return s.still_to_fetch(md); }, note);
    if (host->screen_post(host->host, kScreen, gen, tree.c_str()) == MV_OK) {
      const auto t = std::chrono::steady_clock::now();
      if (shown_generation != gen || shown.size() != now.size()) {
        shown_before = now;  // a page of its own: nothing it showed before
        changed_at = {};
      } else {
        for (std::size_t i = 0; i < now.size() && i < changed_at.size(); ++i) {
          if (!main_changed(shown, now, i)) continue;
          shown_before[i] = shown[i];
          changed_at[i] = t;
        }
      }
      shown = now;
      shown_generation = gen;
      shown_notice = std::move(note);
      last_diff = t;
    }
    return std::nullopt;
  }

  // A click on the open page, read against the status its tree was drawn
  // from: verb::none when that page has gone or the id has no action there,
  // and for a main button a post changed less than kClickSettle ago when the
  // tree before had it do something else (the person saw that one).
  en::action action_of(const char* node_id, uint64_t generation_clicked) {
    std::lock_guard post(post_m);
    if (shown_generation != generation_clicked || shown.empty()) return {};
    en::action a = en::action_of(node_id, shown);
    if (a.what == en::verb::none || node_id[0] != 'a') return a;
    const auto i = static_cast<std::size_t>(a.k);
    if (i < changed_at.size() && std::chrono::steady_clock::now() - changed_at[i] < kClickSettle &&
        en::action_of(node_id, shown_before).what != a.what) {
      return {};
    }
    return a;
  }

  // A click's outcome: a choice that could not be saved says so on the
  // page (the store changed nothing, so nothing else would post it).
  void clicked(mv::expected r) {
    std::string line;
    if (!r) {
      line = r.error() == mv::status::busy
                 ? "Another MediaViewer window was saving its choice. Try again."
                 : "Your choice could not be saved on this computer.";
    }
    {
      std::lock_guard lock(m);
      if (notice == line) return;
      notice = std::move(line);
    }
    schedule(refresh(true));
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
    g->store->reload_choices();  // what other windows chose since
    en::machine here = en::probe(g->data_dir);
    {
      std::lock_guard lock(g->m);
      g->open = true;
      g->generation = r->generation;
      g->here = std::move(here);
      g->notice.clear();
    }
    g->schedule(g->refresh(true));
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
    const en::action a = g->action_of(e->node_id, e->generation);
    switch (a.what) {
      case en::verb::download: g->clicked(g->store->download(a.k, a.model)); break;
      case en::verb::cancel:
        g->store->cancel(a.k);
        g->clicked({});
        break;
      case en::verb::resume:
        g->store->resume(a.k);
        g->clicked({});
        break;
      case en::verb::remove: g->clicked(g->store->remove(a.k)); break;
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
