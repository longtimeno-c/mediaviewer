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
#include "shell/single_instance_win.h"
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#endif
#include <chrono>
#include <thread>

#include "addon/index_request.h"

#if !defined(_WIN32)
extern char** environ;
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

// ---- "index these folders": to the window that hosts the add-ons (docs/design/29) -------

std::string self_exe() {
#if defined(_WIN32)
  std::wstring buf(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  const int m = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
  std::string out(m > 0 ? static_cast<std::size_t>(m) : 0, '\0');
  if (m > 0) ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), m, nullptr, nullptr);
  return out;
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  buf.resize(std::strlen(buf.c_str()));
  return buf;
#else
  return {};
#endif
}

// Starts MediaViewer in the background (no window, `--agent-index`): it hosts
// the add-ons, takes the request once its AI pack attaches, stays while the
// pack indexes and quits after; start at login is left alone.
bool launch_background() {
  const std::string exe = self_exe();
  if (exe.empty()) return false;
#if defined(_WIN32)
  std::wstring w(static_cast<std::size_t>(::MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), -1, nullptr, 0)), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), -1, w.data(), static_cast<int>(w.size()));
  if (!w.empty() && w.back() == L'\0') w.pop_back();
  std::wstring cmd = L"\"" + w + L"\" --agent-index";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
#else
  std::vector<std::string> argv;
  const std::size_t app = exe.find(".app/Contents/MacOS/");
  if (app != std::string::npos) {
    // The app bundle through LaunchServices, hidden.
    argv = {"/usr/bin/open", "-g", "-a", exe.substr(0, app + 4), "--args", "--agent-index"};
  } else {
    argv = {exe, "--agent-index"};  // a bare developer binary
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_SETSID)
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
  std::vector<char*> args;
  for (std::string& a : argv) args.push_back(a.data());
  args.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, args[0], &fa, &attr, args.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  return rc == 0;
#endif
}

#if defined(__APPLE__)
struct ack_wait {
  std::string id;
  std::string answer;
};

CFStringRef notification_name(const char* suffix) {
  CFStringRef bundle = CFBundleGetIdentifier(CFBundleGetMainBundle());
  char id[512] = "";
  if (bundle) (void)CFStringGetCString(bundle, id, sizeof id, kCFStringEncodingUTF8);
  const std::string name = addon::index_notification_name(id) + suffix;
  return CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
}

void on_ack(CFNotificationCenterRef, void* observer, CFNotificationName, const void* object, CFDictionaryRef) {
  auto* w = static_cast<ack_wait*>(observer);
  if (!object || CFGetTypeID(object) != CFStringGetTypeID()) return;
  char buf[512];
  if (!CFStringGetCString(static_cast<CFStringRef>(object), buf, sizeof buf, kCFStringEncodingUTF8)) return;
  const auto v = json::parse(buf, 4);
  if (v && v->str("id") && *v->str("id") == w->id && v->str("state")) w->answer = *v->str("state");
}

// Posts the request and waits up to `ms` for the window's acknowledgement.
std::string post_and_wait(const std::string& json, const std::string& id, int ms) {
  ack_wait w{id, {}};
  CFNotificationCenterRef c = CFNotificationCenterGetDistributedCenter();
  CFStringRef ack = notification_name(".ack");
  CFStringRef name = notification_name("");
  CFNotificationCenterAddObserver(c, &w, &on_ack, ack, nullptr, CFNotificationSuspensionBehaviorDeliverImmediately);
  CFStringRef body = CFStringCreateWithCString(nullptr, json.c_str(), kCFStringEncodingUTF8);
  CFNotificationCenterPostNotification(c, name, body, nullptr, true);
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (w.answer.empty() && std::chrono::steady_clock::now() < until) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
  }
  CFNotificationCenterRemoveObserver(c, &w, ack, nullptr);
  CFRelease(body);
  CFRelease(name);
  CFRelease(ack);
  return w.answer;
}
#endif

// One request: to the running window, or to one started for it.
std::string deliver(const std::string& json, const std::string& id, bool& launched) {
  launched = false;
#if defined(__APPLE__)
  std::string state = post_and_wait(json, id, 1500);
  if (!state.empty()) return state;
  if (!launch_background()) return "no_app";
  launched = true;
  for (int i = 0; i < 15 && state.empty(); ++i) state = post_and_wait(json, id, 2000);
  return state.empty() ? "no_app" : state;
#elif defined(_WIN32)
  std::wstring w(static_cast<std::size_t>(::MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, nullptr, 0)), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, w.data(), static_cast<int>(w.size()));
  if (!w.empty() && w.back() == L'\0') w.pop_back();
  const std::wstring message = std::wstring(kAgentIndexPrefix) + w;
  if (forward_message_to_running_instance(message)) return "started";
  if (!launch_background()) return "no_app";
  launched = true;
  for (int i = 0; i < 30; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (forward_message_to_running_instance(message)) return "queued";
  }
  return "no_app";
#else
  (void)json;
  (void)id;
  return "no_app";
#endif
}

mv_status out_text(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return MV_ERR_INVALID_ARG;
  std::memcpy(out, text.c_str(), need);
  return MV_OK;
}

mv_status MV_CALL request_index(void*, const char* request, char* out, uint32_t cap, uint32_t* needed) {
  try {
    if (!request) return MV_ERR_INVALID_ARG;
    const auto r = addon::index_request_from_json(request);
    if (!r) return out_text(R"({"state":"refused","launched":false})", out, cap, needed);
    bool launched = false;
    const std::string state = deliver(request, r->id, launched);
    json::writer w;
    w.begin_object().key("state").string(state).key("launched").boolean(launched).end_object();
    return out_text(w.str(), out, cap, needed);
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
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
  static mv_status MV_CALL coverage(void* ctx, const char* paths, char* out, uint32_t cap, uint32_t* needed) {
    try {
      auto* self = static_cast<search_services*>(ctx);
      const auto list = paths ? json::parse(paths, 4) : std::nullopt;
      if (!list || list->k != json::kind::array) return MV_ERR_INVALID_ARG;
      nle::search_session* s = self->get();
      if (!s) return to_mv(self->failed);
      json::writer w;
      w.begin_array();
      for (const json::value& p : list->a) {
        if (p.k != json::kind::string) continue;
        auto c = s->folder_coverage(p.s, 30000);
        w.begin_object().key("path").string(p.s).key("state").integer(c ? *c : 0).end_object();
      }
      w.end_array();
      return out_json(w.str(), out, cap, needed);
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
  services.coverage_json = &search_services::coverage;
#endif
  services.request_index = &request_index;
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
