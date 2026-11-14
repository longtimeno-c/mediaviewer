// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "abi/host_tray_glue.h"

#include <memory>
#include <mutex>
#include <string>

#include "addon/slots.h"
#include "phone/host_tray.h"

namespace mv::abi {

namespace {

// docs/plans/self-hosted.md "Tray": while `driftwire host` runs, its section
// of the app's menu-bar / notification-area menu, as the registry's shell keep
// (slots.h). The first window's process only, started with Phone access.
struct tray_glue {
  std::mutex m;
  std::unique_ptr<mv::phone::host_tray::watcher> w;  // under m
};
tray_glue& tray() {
  static tray_glue* g = new tray_glue();  // never destroyed: no join at static exit
  return *g;
}

}  // namespace

void start_host_tray() {
  namespace ht = mv::phone::host_tray;
  std::lock_guard lock(tray().m);
  if (tray().w) return;
  ht::options o;
  o.changed = [](const ht::model& m) {
    mv::addon::slot_registry::shell_keep k;
    k.kept = m.shown;
    k.login = m.login;
    k.screen = ht::kScreen;
    k.title = "Phone access settings";
    k.label = m.label;
    for (const ht::row& r : m.rows) k.menu.push_back({r.kind, r.id, r.label, r.value});
    // A pick goes to the watcher's thread; never waited on here (the host's UI thread).
    k.on_menu = [](const std::string& id, const std::string& value) {
      std::lock_guard g(tray().m);
      if (tray().w) tray().w->pick(id, value);
    };
    (void)mv::addon::default_slots().set_shell_keep(ht::kKeepId, std::move(k));
  };
  tray().w = ht::watcher::start(std::move(o));
}

void stop_host_tray() noexcept {
  std::unique_ptr<mv::phone::host_tray::watcher> w;
  {
    std::lock_guard lock(tray().m);
    w = std::move(tray().w);
  }
  // Told, not waited for: a `host quit` it is running must not hold the app's
  // quit on the UI thread. Its thread may still be inside one last `changed`;
  // the entry is removed after, and again by the registry going with the process.
  if (w) {
    w->stop(false);
    (void)w.release();  // never destroyed: its detached thread holds its state
  }
  (void)mv::addon::default_slots().set_shell_keep(mv::phone::host_tray::kKeepId, std::nullopt);
}

}  // namespace mv::abi
