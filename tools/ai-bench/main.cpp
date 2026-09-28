// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ai-bench: the AI pack without the chrome (Milestone H, plan/17 PR 21-22
// timings). Loads the installed add-on exactly as the app does (the store
// verifies it, the loader maps it, the host table carries the viewer's own
// media services), remembers a folder, prints status as JSON lines while it
// indexes, then times queries.
//
//   ai-bench --addons <folder> --index <media folder> [--recursive] [--media 1|2|3]
//            [--compute 0..4] [--quality 0..2] [--timeout <s>] [--query "text"]...
//   ai-bench --addons <folder> --photos ...   the Mac's Photos library instead (issue #72;
//            macOS, run through tools/ai-bench/photos-bench.sh for the permission prompt)
//            [--busy-after <s> --busy-for <s>]   (the viewer "presents" then: yield check)
//            [--quit-after <s> [--quit-budget <s>] [--quit-hash] [--quit-legacy]]
//
// --quit-after: after the index and query steps, keeps the pack running for
// <s> seconds, then quits the way the hosts do (addon/host.h "Quit": the stop
// gets --quit-budget, default 0.5 s, then the exit skips static destructors
// if the pack is still stopping) and prints the time the quit took and the
// wall clock it exited at. --quit-legacy quits the way the Mac host did
// before (a normal exit after up to 5 s for the pack's shutdown). --quit-hash
// re-verifies the installed pack on a thread meanwhile, as Settings' state
// read does (store::find hashes every file).
//
// Nothing here logs a path beyond what the caller typed. Needs a build whose
// add-on key trusts the pack (MV_ADDON_DEV_PUBLIC_KEY for a dev-signed one).
#include <mediaviewer/mediaviewer_ai.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "abi/addon_media.h"
#include "addon/store.h"

#if defined(__APPLE__)
bool ai_bench_ask_photos();
#endif

namespace {

std::atomic<bool> g_busy{false};

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void print_status(const mv_ai_api* ai, double t0) {
  mv_ai_status s{};
  s.struct_size = sizeof(s);
  if (ai->status(ai->ctx, &s) != MV_OK) return;
  std::printf("{\"t\":%.1f,\"state\":%u,\"yield\":%u,\"backend\":%u,\"fault\":%u,\"model\":\"%s\","
              "\"assets\":%llu,\"done\":%llu,\"failed\":%llu,\"frames\":%llu,\"assets_per_s\":%.2f,"
              "\"frames_per_s\":%.2f,\"eta\":[%.0f,%.0f],\"sound\":[%llu,%llu],\"speech\":[%llu,%llu],"
              "\"index_bytes\":%llu,\"flags\":%u,\"unavailable\":%llu}\n",
              now_s() - t0, s.state, s.yield_reason, s.backend, s.provider_fault, s.model_utf8,
              static_cast<unsigned long long>(s.assets_total), static_cast<unsigned long long>(s.assets_done),
              static_cast<unsigned long long>(s.assets_failed), static_cast<unsigned long long>(s.frames_indexed),
              s.assets_per_second, s.frames_per_second, s.eta_low_seconds, s.eta_high_seconds,
              static_cast<unsigned long long>(s.sound_done), static_cast<unsigned long long>(s.sound_total),
              static_cast<unsigned long long>(s.speech_done), static_cast<unsigned long long>(s.speech_total),
              static_cast<unsigned long long>(s.index_bytes), s.flags,
              static_cast<unsigned long long>(s.assets_unavailable));
  std::fflush(stdout);
}

bool idle(const mv_ai_api* ai) {
  mv_ai_status s{};
  s.struct_size = sizeof(s);
  if (ai->status(ai->ctx, &s) != MV_OK) return false;
  return s.state == MV_AI_STATE_IDLE && s.assets_total > 0;
}

// Quit as the hosts do (addon/host.h "Quit"): the pack's stop starts on a
// thread of its own and gets `budget`; one still stopping after it is left to
// the exit, which then skips static destructors.
bool quit_pack(std::unique_ptr<mv::addon::loaded_addon> pack, double budget) {
  mv::addon::stop_for_exit(std::move(pack));
  return mv::addon::wait_stopped_for_exit(std::chrono::steady_clock::now() +
                                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(budget)));
}

// --quit-legacy: Quit as the Mac host did before 2026-09-27, for before/after
// numbers: the pack retired on a worker (shutdown joins its threads, then the
// dylib unloads) and up to 5 s waited for it before a normal exit.
bool quit_pack_legacy(std::unique_ptr<mv::addon::loaded_addon> pack) {
  struct shared {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
  };
  auto s = std::make_shared<shared>();
  std::thread([s, p = std::move(pack)]() mutable {
    p.reset();
    std::lock_guard lock(s->m);
    s->done = true;
    s->cv.notify_all();
  }).detach();
  std::unique_lock lock(s->m);
  return s->cv.wait_for(lock, std::chrono::seconds(5), [&] { return s->done; });
}

}  // namespace

int main(int argc, char** argv) {
  std::string addons, folder;
  bool recursive = false;
  int media = -1, compute = -1, quality = -1;
  double timeout = 3600, busy_after = -1, busy_for = 0, quit_after = -1, quit_budget = 0.5;
  bool quit_hash = false, quit_legacy = false, photos = false;
  std::vector<std::string> queries;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--addons") addons = next();
    else if (a == "--index") folder = next();
    else if (a == "--recursive") recursive = true;
    else if (a == "--media") media = std::stoi(next());
    else if (a == "--compute") compute = std::stoi(next());
    else if (a == "--quality") quality = std::stoi(next());
    else if (a == "--timeout") timeout = std::stod(next());
    else if (a == "--query") queries.push_back(next());
    else if (a == "--busy-after") busy_after = std::stod(next());
    else if (a == "--busy-for") busy_for = std::stod(next());
    else if (a == "--quit-after") quit_after = std::stod(next());
    else if (a == "--quit-hash") quit_hash = true;
    else if (a == "--quit-legacy") quit_legacy = true;
    else if (a == "--quit-budget") quit_budget = std::stod(next());
    else if (a == "--photos") photos = true;
  }
  if (addons.empty()) {
    std::fprintf(stderr, "usage: ai-bench --addons <dir> [--index <dir>] [--query text]...\n");
    return 2;
  }
  const auto key = mv::addon::pinned_public_key();
  // Lives until exit, as the app's does (a state read may still be hashing).
  static mv::addon::store store(addons, std::vector<std::uint8_t>(key.begin(), key.end()), MV_ADDON_HOST_API);
  mv::addon::host_services svc;
  svc.should_yield = [] { return g_busy.load(); };
  svc.post = [](const mv_addon_event&) {};
  svc.still_rgb = &mv::addon::media::decode_still;
  svc.open_sampler = &mv::addon::media::open_sampler;
  svc.video_frame = &mv::addon::media::video_frame;
  svc.moment_thumbnail = &mv::addon::media::moment_thumbnail;
  svc.open_audio = &mv::addon::media::open_audio;
  const double t_load = now_s();
  auto loaded = mv::addon::loaded_addon::load(store, "ai", std::move(svc));
  if (!loaded) {
    std::fprintf(stderr, "load failed: %s\n", mv::status_name(loaded.error()));
    return 1;
  }
  std::printf("{\"loaded_s\":%.2f}\n", now_s() - t_load);
  const auto* ai = static_cast<const mv_ai_api*>((*loaded)->query(MV_AI_INTERFACE));
  if (!ai) return 1;
  if (compute >= 0) ai->set_setting(ai->ctx, "compute", std::to_string(compute).c_str());
  if (quality >= 0) ai->set_setting(ai->ctx, "quality", std::to_string(quality).c_str());
  if (media >= 0) ai->set_setting(ai->ctx, "video_index", std::to_string(media).c_str());
  const double t0 = now_s();
  if (!folder.empty() || photos) {
    uint64_t root = 0;
    if (photos) {
#if defined(__APPLE__)
      if (!ai_bench_ask_photos()) {
        std::fprintf(stderr, "no access to the Photos library\n");
        return 1;
      }
#endif
      const mv_status st = ai->index_photos_library ? ai->index_photos_library(ai->ctx, &root) : MV_ERR_UNSUPPORTED_FORMAT;
      if (st != MV_OK) {
        std::fprintf(stderr, "index_photos_library: %d\n", static_cast<int>(st));
        return 1;
      }
    } else if (ai->index_folder(ai->ctx, folder.c_str(), recursive ? 1u : 0u, &root) != MV_OK) {
      return 1;
    }
    double last = 0;
    while (now_s() - t0 < timeout) {
      const double t = now_s() - t0;
      if (busy_after >= 0) g_busy = t >= busy_after && t < busy_after + busy_for;
      if (t - last >= 5) {
        print_status(ai, t0);
        last = t;
      }
      if (t > 10 && idle(ai)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    print_status(ai, t0);
    std::printf("{\"indexed_s\":%.1f}\n", now_s() - t0);
  }
  for (const std::string& q : queries) {
    const double tq = now_s();
    uint64_t id = 0;
    ai->search_text(ai->ctx, q.c_str(), nullptr, MV_AI_SCOPE_ALL, MV_AI_KIND_ALL, &id);
    uint32_t n = 0;
    while (ai->result_count(ai->ctx, id, &n) != MV_OK) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const double ms = (now_s() - tq) * 1000;
    std::printf("{\"query\":\"%s\",\"ms\":%.1f,\"results\":%u,\"top\":[", q.c_str(), ms, n);
    for (uint32_t i = 0; i < n && i < 5; ++i) {
      mv_ai_result r{};
      char path[4096] = {};
      char snip[512] = {};
      ai->result_at(ai->ctx, id, i, &r);
      ai->result_path(ai->ctx, id, i, path, sizeof path);
      if (ai->result_snippet) ai->result_snippet(ai->ctx, id, i, snip, sizeof snip);
      const char* name = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
      // A Photos asset's identifier is as private as a path: never printed.
      if (std::strncmp(path, "photos:", 7) == 0) name = r.pts_ms >= 0 ? "photos:video" : "photos:photo";
      std::printf("%s{\"file\":\"%s\",\"ms\":%lld,\"score\":%.3f,\"match\":%u,\"snippet\":\"%s\"}", i ? "," : "",
                  name, static_cast<long long>(r.pts_ms), r.score, r.match, snip);
    }
    std::printf("]}\n");
    std::fflush(stdout);
  }
  if (quit_after >= 0) {
    if (quit_hash) {
      std::thread([] {
        for (;;) (void)store.find("ai");
      }).detach();
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(quit_after));
    const double tq = now_s();
    const bool stopped = quit_legacy ? quit_pack_legacy(std::move(*loaded)) : quit_pack(std::move(*loaded), quit_budget);
    // exit_at is wall-clock seconds, to set against the caller's clock once the
    // process has gone (the exit itself is the last step).
    std::printf("{\"quit_pack_s\":%.3f,\"stopped\":%s,\"exit_at\":%.3f}\n", now_s() - tq,
                stopped ? "true" : "false",
                std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::fflush(stdout);
    if (!quit_legacy && mv::addon::running_at_exit()) std::_Exit(0);
  }
  return 0;
}
