// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/mcp_host.h"

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_agent.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "abi/addon_media.h"
#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "core/json.h"
#include "io/paths.h"
#if defined(MV_MCP_WITH_SEARCH)
#include "nle/search_session.h"
#endif

#if defined(MV_APP_VERSION)
#define MV_MCP_VERSION MV_APP_VERSION
#else
#define MV_MCP_VERSION "0.0.0-dev"
#endif

namespace mv::shell {
namespace {

void say(const char* line) { std::fprintf(stderr, "MediaViewer --mcp: %s\n", line); }

// The protocol owns stdout: a copy of it for the server, and fd 1 pointed at
// stderr so a stray printf (a library's, an add-on's) cannot break a message.
std::intptr_t take_stdout() {
#if defined(_WIN32)
  const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE copy = nullptr;
  if (out == nullptr || out == INVALID_HANDLE_VALUE ||
      !::DuplicateHandle(::GetCurrentProcess(), out, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return -1;
  }
  ::SetStdHandle(STD_OUTPUT_HANDLE, ::GetStdHandle(STD_ERROR_HANDLE));
  return reinterpret_cast<std::intptr_t>(copy);
#else
  const int copy = dup(1);
  if (copy < 0) return -1;
  (void)dup2(2, 1);
  return copy;
#endif
}

std::intptr_t stdin_handle() {
#if defined(_WIN32)
  return reinterpret_cast<std::intptr_t>(::GetStdHandle(STD_INPUT_HANDLE));
#else
  return 0;
#endif
}

#if defined(MV_MCP_WITH_SEARCH)
// Local search for the agent: the pack's read-only reader, opened at the first
// call (it loads the text towers: seconds, hundreds of MB), never before.
struct search_services {
  const addon::store* store = nullptr;
  std::unique_ptr<nle::search_session> session;
  status failed = status::ok;

  nle::search_session* get() {
    if (session || failed != status::ok) return session.get();
    auto thumbs = io::thumb_cache_dir();
    auto opened = nle::search_session::open(*store, thumbs ? *thumbs : std::string());
    if (!opened) {
      failed = opened.error();
      say(status_name(failed));
      return nullptr;
    }
    session = std::move(*opened);
    return session.get();
  }

  static mv_status to_mv(status s) { return static_cast<mv_status>(s); }

  static mv_status out_json(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
    const auto need = static_cast<uint32_t>(text.size() + 1);
    if (needed) *needed = need;
    if (!out || cap < need) return MV_ERR_INVALID_ARG;
    std::memcpy(out, text.c_str(), need);
    return MV_OK;
  }

  mv_status run(nle::request_kind kind, const char* text, int64_t pts_ms, const char* dir, uint32_t scope,
                uint32_t kinds, uint32_t max, char* out, uint32_t cap, uint32_t* needed) {
    if (scope > 1 || !dir || !*dir) return MV_ERR_INVALID_ARG;  // never the whole index
    nle::search_session* s = get();
    if (!s) return to_mv(failed);
    nle::request r;
    r.kind = kind;
    r.scope = scope;
    r.kinds = kinds;
    r.max_results = max;
    r.pts_ms = pts_ms;
    const nle::reply rep = s->run(r, text ? text : "", dir, 30000);
    if (rep.code != status::ok && rep.rows.empty()) return to_mv(rep.code);
    json::writer w;
    w.begin_array();
    for (const nle::row& x : rep.rows) {
      w.begin_object().key("path").string(x.path).key("pts_ms").integer(x.pts_ms).key("duration_ms")
          .integer(x.duration_ms).key("score").number(x.score).key("kind").integer(x.kind).key("moments").begin_array();
      for (const nle::moment& m : x.moments) w.integer(m.pts_ms);
      w.end_array().end_object();
    }
    w.end_array();
    return out_json(w.str(), out, cap, needed);
  }

  static mv_status MV_CALL search(void* ctx, const char* q, const char* dir, uint32_t scope, uint32_t kinds,
                                  uint32_t max, char* out, uint32_t cap, uint32_t* needed) {
    try {
      return static_cast<search_services*>(ctx)->run(nle::request_kind::text, q, -1, dir, scope, kinds, max, out, cap,
                                                     needed);
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }
  static mv_status MV_CALL similar(void* ctx, const char* path, int64_t pts, const char* dir, uint32_t scope,
                                   uint32_t kinds, uint32_t max, char* out, uint32_t cap, uint32_t* needed) {
    try {
      return static_cast<search_services*>(ctx)->run(nle::request_kind::similar, path, pts, dir, scope, kinds, max,
                                                     out, cap, needed);
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }
  static mv_status MV_CALL transcript(void* ctx, const char* path, char* out, uint32_t cap, uint32_t* needed) {
    try {
      auto* self = static_cast<search_services*>(ctx);
      nle::search_session* s = path ? self->get() : nullptr;
      if (!s) return path ? to_mv(self->failed) : MV_ERR_INVALID_ARG;
      auto j = s->transcript_json(path, 30000);
      if (!j) return to_mv(j.error());
      return out_json(*j, out, cap, needed);
    } catch (...) {
      return MV_ERR_INTERNAL;
    }
  }
};
#endif

}  // namespace

int run_mcp(const std::vector<std::string>& args) {
  std::string timeline;
  std::vector<std::string> folders;
  std::uint32_t flags = 0;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--mcp") continue;
    if (a == "--timeline" && i + 1 < args.size()) {
      timeline = args[++i];
    } else if (a == "--folder" && i + 1 < args.size()) {
      folders.push_back(args[++i]);
    } else if (a == "--no-handoff") {
      flags |= MV_AGENT_SERVE_NO_HANDOFF;
    } else {
      say("usage: MediaViewer --mcp [--timeline FILE.fcpxml] [--folder DIR]... [--no-handoff]");
      return 2;
    }
  }
  const std::intptr_t out = take_stdout();
  if (out < 0) {
    say("no stdout to serve on");
    return 2;
  }
#if defined(_WIN32)
  // The pipes carry UTF-8 bytes as they are.
  (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
  auto root = io::addons_dir();
  if (!root) {
    say("no add-ons folder");
    return 1;
  }
  const auto key = addon::pinned_public_key();
  addon::store store(*root, std::vector<std::uint8_t>(key.begin(), key.end()), MV_ADDON_HOST_API);
  addon::host_services svc;
  svc.post = [](const mv_addon_event&) {};
  svc.should_yield = [] { return false; };
  svc.open_audio = &addon::media::open_audio;
  svc.open_sampler = &addon::media::open_sampler;
  svc.video_frame = &addon::media::video_frame;
  svc.moment_thumbnail = &addon::media::moment_thumbnail;
  svc.still_rgb = &addon::media::decode_still;
  svc.thumbnail_jpeg = &addon::media::thumbnail_jpeg;
  auto loaded = addon::loaded_addon::load(store, "agent", std::move(svc));
  if (!loaded) {
    say(loaded.error() == status::not_found
            ? "the AI agents add-on is not installed: MediaViewer > Settings > Add-ons > AI agents"
            : status_name(loaded.error()));
    return 1;
  }
  const auto* api = static_cast<const mv_agent_api*>((*loaded)->query(MV_AGENT_INTERFACE));
  if (!api || api->struct_size < sizeof(mv_agent_api) || !api->serve) {
    say("the installed AI agents add-on is too old for this MediaViewer");
    return 1;
  }
  mv_agent_services services{};
  services.struct_size = sizeof services;
#if defined(MV_MCP_WITH_SEARCH)
  search_services search;
  search.store = &store;
  services.ctx = &search;
  services.search_json = &search_services::search;
  services.similar_json = &search_services::similar;
  services.transcript_json = &search_services::transcript;
#endif
  std::vector<const char*> folder_ptrs;
  for (const std::string& f : folders) folder_ptrs.push_back(f.c_str());
  mv_agent_serve_args a{};
  a.struct_size = sizeof a;
  a.flags = flags;
  a.in_fd = stdin_handle();
  a.out_fd = out;
  a.timeline_utf8 = timeline.empty() ? nullptr : timeline.c_str();
  a.folders_utf8 = folder_ptrs.empty() ? nullptr : folder_ptrs.data();
  a.folder_count = static_cast<std::uint32_t>(folder_ptrs.size());
  a.host_version = MV_MCP_VERSION;
  a.services = &services;
  const mv_status st = api->serve((*loaded)->api().addon, &a);
  // The pack's threads may still be winding down: no static destructors.
  std::fflush(stderr);
#if defined(_WIN32)
  ::ExitProcess(st == MV_OK ? 0u : 1u);
#else
  _exit(st == MV_OK ? 0 : 1);
#endif
}

}  // namespace mv::shell
