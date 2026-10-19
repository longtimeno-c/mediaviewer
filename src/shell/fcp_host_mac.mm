// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host half of the Final Cut Pro search agent (fcp_host_mac.h): the add-on
// (src/addons/fcp) owns the listener, the peer checks and the idle exit; the
// app lends it what only the app has: the add-on store's verified load and
// the AI pack's read-only reader (nle/search_session), as `--mcp` does for
// the AI agents add-on (shell/mcp_host.cpp).
#include "shell/fcp_host_mac.h"

#include <os/log.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_fcp.h>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "io/paths.h"
#include "nle/search_session.h"
#include "nle/search_wire.h"

namespace {

constexpr int kSearchTimeoutMs = 30000;

os_log_t host_log() {
  static os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.fcp", "host");
  return log;
}

// The pack loads at the first call (on the add-on's work queue), never on
// the listener's thread. Calls arrive one at a time.
struct services {
  std::unique_ptr<mv::addon::store> store;
  std::string thumbs;
  std::unique_ptr<mv::nle::search_session> session;
  mv::status failed = mv::status::ok;

  mv::nle::search_session* get() {
    if (session || failed != mv::status::ok) return session.get();
    auto opened = mv::nle::search_session::open(*store, thumbs);
    if (!opened) {
      // unsupported_format: the installed pack predates the reader.
      failed = opened.error();
      os_log_error(host_log(), "Local search did not load: %{public}s", mv::status_name(failed));
      return nullptr;
    }
    session = std::move(*opened);
    return session.get();
  }

  static services& of(void* ctx) { return *static_cast<services*>(ctx); }

  static mv_status MV_CALL run(void* ctx, const void* request, uint64_t size, const char* text, const char* scope,
                               mv_fcp_bytes_fn out, void* user) {
    try {
      auto req = mv::nle::decode_request(
          std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(request), static_cast<std::size_t>(size)));
      if (!req) return static_cast<mv_status>(req.error());
      services& self = of(ctx);
      mv::nle::reply rep;
      if (mv::nle::search_session* s = self.get()) {
        rep = s->run(*req, text ? text : "", scope ? scope : "", kSearchTimeoutMs);
      } else {
        rep.correlation_id = req->correlation_id;
        rep.code = self.failed;
      }
      const std::vector<std::uint8_t> bytes = mv::nle::encode(rep);
      out(user, bytes.data(), bytes.size());
      return MV_OK;
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }

  static mv_status MV_CALL roots(void* ctx, mv_fcp_bytes_fn out, void* user) {
    try {
      mv::nle::search_session* s = of(ctx).get();
      if (!s) return MV_ERR_NOT_FOUND;
      auto json = s->roots_json(kSearchTimeoutMs);
      if (!json) return static_cast<mv_status>(json.error());
      out(user, json->data(), json->size());
      return MV_OK;
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }

  static mv_status MV_CALL suggest(void* ctx, const char* text, mv_fcp_bytes_fn out, void* user) {
    try {
      mv::nle::search_session* s = of(ctx).get();
      if (!s) return MV_ERR_NOT_FOUND;
      auto json = s->suggest_json(text ? text : "", 2000);
      if (!json) return static_cast<mv_status>(json.error());
      out(user, json->data(), json->size());
      return MV_OK;
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }
};

}  // namespace

int MvSearchAgentMain() {
  auto root = mv::io::addons_dir();
  auto thumbs = mv::io::thumb_cache_dir();
  if (!root || !thumbs) return 1;
  static services svc;  // lives until the process exits (the add-on's idle _exit)
  const auto key = mv::addon::pinned_public_key();
  svc.store = std::make_unique<mv::addon::store>(*root, std::vector<std::uint8_t>(key.begin(), key.end()),
                                                 MV_ADDON_HOST_API);
  svc.thumbs = *thumbs;
  mv::addon::host_services hs;
  hs.post = [](const mv_addon_event&) {};
  hs.should_yield = [] { return false; };
  auto loaded = mv::addon::loaded_addon::load(*svc.store, "fcp", std::move(hs));
  if (!loaded) {
    // Not installed (removed while its launchd job was still registered), or
    // it failed verification: nothing to serve.
    auto found = svc.store->find("fcp");
    os_log_error(host_log(), "the Final Cut Pro add-on did not load: %{public}s (%{public}s)",
                 mv::status_name(loaded.error()), found ? mv::addon::rejection_name(found->why) : "not installed");
    return 1;
  }
  static std::unique_ptr<mv::addon::loaded_addon> addon = std::move(*loaded);
  const auto* api = static_cast<const mv_fcp_api*>(addon->query(MV_FCP_INTERFACE));
  if (!api || api->struct_size < sizeof(mv_fcp_api) || !api->run_agent) {
    os_log_error(host_log(), "the Final Cut Pro add-on is too old for this MediaViewer");
    return 1;
  }
  static mv_fcp_services table{};
  table.struct_size = sizeof table;
  table.ctx = &svc;
  table.run = &services::run;
  table.roots_json = &services::roots;
  table.suggest_json = &services::suggest;
  table.thumbs_dir_utf8 = svc.thumbs.c_str();
#if defined(MV_FCP_DEV_ALLOW_UNSIGNED)
  constexpr uint32_t allow_unsigned = 1;  // an ad hoc developer build has no team to check peers against
#else
  constexpr uint32_t allow_unsigned = 0;
#endif
  const mv_status st = api->run_agent(addon->api().addon, MV_FCP_MACH_SERVICE, &table, allow_unsigned);
  os_log_error(host_log(), "the agent stopped: %d", static_cast<int>(st));
  return 1;
}
