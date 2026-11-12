// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phone/app.h"

#include <memory>
#include <mutex>

#include "catalogue/publisher.h"
#include "io/paths.h"
#include "phone/desk.h"
#include "wire/net.h"
#include "wire/secrets.h"

namespace mv::phone {

namespace {

namespace secrets = mv::driftwire::secrets;

// The desk, made on the link thread at its first route.
struct desk_holder {
  std::mutex m;
  std::unique_ptr<secrets::store> store;
  std::unique_ptr<mv::driftwire::desk::desk> desk;
  bool tried = false;

  mv::driftwire::desk::desk* get() {
    std::lock_guard lock(m);
    if (desk || tried) return desk.get();
    tried = true;
    store = secrets::os_store(mv::ai::catalog::kPublisherService);
    if (!store) return nullptr;
    // Driftwire kept these before 2026-11-12: taken once, so a claimed NAS and
    // its paired phones carry on.
    if (auto driftwire = secrets::os_store(secrets::kAppService)) mv::driftwire::desk::take_items(*driftwire, *store);
    mv::driftwire::desk::desk::options o;
    o.store = store.get();
    o.device_name = mv::driftwire::net::machine_name();
    desk = std::make_unique<mv::driftwire::desk::desk>(std::move(o));
    return desk.get();
  }
};

struct app_state {
  std::mutex m;
  link* l = nullptr;           // never destroyed: stop_app ends its thread
  catalogue_source pending;    // a source set before the link was made
  bool stopped = false;
  bool secondary = false;      // a later window's process: no Phone access here
  desk_holder desk;
};

app_state& state() {
  static app_state* s = new app_state();  // never destroyed: no join at static exit
  return *s;
}

link* get_link() {
  app_state& s = state();
  std::lock_guard lock(s.m);
  if (s.l || s.stopped || s.secondary) return s.l;
  link::options o;
  if (auto dir = io::phone_dir()) o.data_dir = *dir;
  o.key_store = [] { return secrets::os_store(mv::ai::catalog::kPublisherService); };
  o.legacy_key_store = [] { return secrets::os_store(mv::ai::catalog::kLegacyService); };
  o.route = [&s](const char* method, const std::string& json) -> result<std::string> {
    auto* d = s.desk.get();
    if (!d) return err(status::not_found);  // no secret store on this machine
    return mv::driftwire::desk::route_request(*d, "phone", method, json);
  };
  o.machine_name = mv::driftwire::net::machine_name();
  o.detach_on_stop = true;
  s.l = new link(std::move(o));
  if (s.pending) s.l->set_source(std::move(s.pending));
  s.pending = {};
  return s.l;
}

}  // namespace

expected app_request(const std::string& request_json) {
  link* l = get_link();
  if (!l) {
    std::lock_guard lock(state().m);
    return err(state().secondary ? status::unsupported_format : status::cancelled);
  }
  return l->request(request_json);
}

std::string app_state_json() {
  link* l = get_link();
  if (l) return l->state_json();
  std::lock_guard lock(state().m);
  return state().secondary ? std::string("{\"secondary\":true}") : std::string("{}");
}

void set_app_source(catalogue_source s) {
  app_state& st = state();
  link* l = nullptr;
  {
    std::lock_guard lock(st.m);
    if (!st.l) {
      st.pending = std::move(s);
      return;
    }
    l = st.l;
  }
  l->set_source(std::move(s));
}

void resume_app(bool primary) {
  if (!primary) {
    std::lock_guard lock(state().m);
    state().secondary = true;
    state().pending = {};
    return;
  }
  if (link* l = get_link()) l->resume();
}

void stop_app() noexcept {
  app_state& st = state();
  link* l = nullptr;
  {
    std::lock_guard lock(st.m);
    st.stopped = true;
    st.pending = {};
    l = st.l;
  }
  if (l) {
    l->set_source({});  // Local search is about to go: nothing calls into it now
    l->stop();
  }
}

}  // namespace mv::phone
