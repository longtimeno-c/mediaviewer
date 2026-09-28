// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #72 spike: measures the Photos library source (src/addons/ai/
// photos_mac.mm) on this Mac's real library, with no model and no index:
// enumeration time, the local-derivative fetch rate at the sizes the indexer
// asks for, how many stills and videos are iCloud-only, and peak memory.
//
// PhotoKit needs a bundle with NSPhotoLibraryUsageDescription for the system's
// permission prompt, so tools/ai/photos-spike.sh wraps this in a throwaway
// .app and runs it with `open`. It prints counts and timings only: never an
// identifier, a file name or a path (rule 6), so its output can be pasted into
// plan/17.
//
//   photos-spike.sh [--stills N] [--videos N] [--edge 448] [--threads 2] [--out report.json]
#include "addons/ai/photos_source.h"

#import <Foundation/Foundation.h>
#import <Photos/Photos.h>

#include <mach/mach.h>
#include <sys/clonefile.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace mv::ai;

namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double peak_mb() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS) return -1;
  return static_cast<double>(info.ledger_phys_footprint_peak) / (1024.0 * 1024.0);
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    std::size_t stills = 1000, videos = 20, opens = 200;
    std::uint32_t edge = 448;
    unsigned threads = 2;
    std::string out = "photos-spike.json";
    for (int i = 1; i + 1 < argc; i += 2) {
      if (!std::strcmp(argv[i], "--stills")) stills = std::strtoul(argv[i + 1], nullptr, 10);
      else if (!std::strcmp(argv[i], "--videos")) videos = std::strtoul(argv[i + 1], nullptr, 10);
      else if (!std::strcmp(argv[i], "--edge")) edge = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
      else if (!std::strcmp(argv[i], "--threads")) threads = static_cast<unsigned>(std::strtoul(argv[i + 1], nullptr, 10));
      else if (!std::strcmp(argv[i], "--out")) out = argv[i + 1];
      else if (!std::strcmp(argv[i], "--opens")) opens = std::strtoul(argv[i + 1], nullptr, 10);
    }
    FILE* f = std::fopen(out.c_str(), "w");
    if (!f) return 2;

    // The one place this spike asks: the source never does (photos_source.h).
    dispatch_semaphore_t asked = dispatch_semaphore_create(0);
    [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                               handler:^(PHAuthorizationStatus) { dispatch_semaphore_signal(asked); }];
    dispatch_semaphore_wait(asked, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC));

    auto src = make_photos_source();
    const photos_access acc = src->access();
    std::fprintf(f, "{\n  \"access\": \"%s\",\n", access_name(acc));
    if (!readable(acc)) {
      std::fprintf(f, "  \"error\": \"no access\"\n}\n");
      std::fclose(f);
      return 1;
    }

    std::vector<photos_item> items;
    const double t0 = now_s();
    (void)src->enumerate([&](const photos_item& it) {
      items.push_back(it);
      return true;
    });
    const double enum_s = now_s() - t0;
    // A second pass: the startup delta scan's cost once PhotoKit is warm.
    const double t1 = now_s();
    std::size_t again = 0;
    (void)src->enumerate([&](const photos_item&) {
      ++again;
      return true;
    });
    const double enum2_s = now_s() - t1;
    std::size_t n_photo = 0, n_video = 0;
    for (const photos_item& it : items) (it.kind == asset_kind::video ? n_video : n_photo)++;
    std::fprintf(f, "  \"assets\": %zu, \"photos\": %zu, \"videos\": %zu,\n", items.size(), n_photo, n_video);
    std::fprintf(f, "  \"enumerate_s\": %.3f, \"enumerate_warm_s\": %.3f,\n", enum_s, enum2_s);

    // Stills spread over the whole library (not only the newest), as the
    // indexer's workers would take them, `threads` at a time.
    std::vector<const photos_item*> pick;
    for (const photos_item& it : items) {
      if (it.kind == asset_kind::photo) pick.push_back(&it);
    }
    if (pick.size() > stills && stills > 0) {
      std::vector<const photos_item*> spread;
      for (std::size_t i = 0; i < stills; ++i) spread.push_back(pick[i * pick.size() / stills]);
      pick.swap(spread);
    }
    std::atomic<std::size_t> next{0}, ok{0}, cloud{0}, failed{0};
    std::vector<std::vector<double>> ms(threads);
    const double t2 = now_s();
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; ++w) {
      pool.emplace_back([&, w] {
        @autoreleasepool {
          for (std::size_t i; (i = next++) < pick.size();) {
            const double a = now_s();
            auto r = src->still(pick[i]->id, edge);
            ms[w].push_back((now_s() - a) * 1000.0);
            if (r) ++ok;
            else if (r.error() == mv::status::io) ++cloud;
            else ++failed;
          }
        }
      });
    }
    for (auto& t : pool) t.join();
    const double stills_s = now_s() - t2;
    std::vector<double> all;
    for (auto& v : ms) all.insert(all.end(), v.begin(), v.end());
    std::fprintf(f,
                 "  \"stills\": {\"asked\": %zu, \"edge\": %u, \"threads\": %u, \"local\": %zu, \"icloud_only\": %zu,"
                 " \"failed\": %zu, \"seconds\": %.3f, \"per_second\": %.1f, \"p50_ms\": %.1f, \"p95_ms\": %.1f,"
                 " \"max_ms\": %.1f},\n",
                 pick.size(), edge, threads, ok.load(), cloud.load(), failed.load(), stills_s,
                 stills_s > 0 ? static_cast<double>(pick.size()) / stills_s : 0.0, pct(all, 0.5), pct(all, 0.95),
                 pct(all, 1.0));

    std::size_t v_ok = 0, v_cloud = 0, v_failed = 0, v_asked = 0;
    std::vector<double> vms;
    for (const photos_item& it : items) {
      if (it.kind != asset_kind::video) continue;
      if (v_asked++ >= videos) break;
      const double a = now_s();
      auto r = src->video_file(it.id);
      vms.push_back((now_s() - a) * 1000.0);
      if (r) ++v_ok;
      else if (r.error() == mv::status::io) ++v_cloud;
      else ++v_failed;
    }
    std::fprintf(f,
                 "  \"videos_resolved\": {\"asked\": %zu, \"local\": %zu, \"icloud_only\": %zu, \"failed\": %zu,"
                 " \"p50_ms\": %.1f, \"max_ms\": %.1f},\n",
                 std::min(v_asked, videos), v_ok, v_cloud, v_failed, pct(vms, 0.5), pct(vms, 1.0));
    // Opening a result in the viewer (the chrome's PhotosLibrary.swift): the
    // current rendition's file from PhotoKit, cloned (APFS: no bytes copied)
    // into a scratch folder the viewer may write to without touching Photos.
    {
      NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"mv-photos-open"];
      [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
      [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
      std::vector<std::string> ids;
      for (const photos_item& it : items) {
        if (it.kind == asset_kind::photo) ids.push_back(it.id);
      }
      if (ids.size() > opens && opens > 0) {
        std::vector<std::string> spread;
        for (std::size_t i = 0; i < opens; ++i) spread.push_back(ids[i * ids.size() / opens]);
        ids.swap(spread);
      }
      std::size_t cloned = 0, copied = 0, cloud = 0, other = 0;
      std::vector<double> oms;
      const double t3 = now_s();
      std::size_t n = 0;
      // PhotoKit answers content-editing requests on the main queue: the loop
      // runs on a worker while the main thread's run loop serves it.
      std::atomic<bool> finished{false};
      std::thread worker([&] {
      for (const std::string& id : ids) {
        @autoreleasepool {
          const double a = now_s();
          PHAsset* asset = [PHAsset fetchAssetsWithLocalIdentifiers:@[ [NSString stringWithUTF8String:id.c_str()] ]
                                                            options:nil].firstObject;
          PHContentEditingInputRequestOptions* o = [[PHContentEditingInputRequestOptions alloc] init];
          o.networkAccessAllowed = NO;
          dispatch_semaphore_t got = dispatch_semaphore_create(0);
          __block NSURL* url = nil;
          [asset requestContentEditingInputWithOptions:o
                                     completionHandler:^(PHContentEditingInput* in, NSDictionary*) {
                                       url = in.fullSizeImageURL;
                                       dispatch_semaphore_signal(got);
                                     }];
          dispatch_semaphore_wait(got, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
          if (!url) {
            ++cloud;
          } else {
            NSString* dst = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%zu.%@", n++, url.pathExtension]];
            if (clonefile(url.fileSystemRepresentation, dst.fileSystemRepresentation, 0) == 0) ++cloned;
            else if ([[NSFileManager defaultManager] copyItemAtURL:url toURL:[NSURL fileURLWithPath:dst] error:nil]) ++copied;
            else ++other;
          }
          oms.push_back((now_s() - a) * 1000.0);
        }
      }
      finished = true;
      });
      while (!finished) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
      worker.join();
      const double open_s = now_s() - t3;
      std::fprintf(f,
                   "  \"open\": {\"asked\": %zu, \"cloned\": %zu, \"copied\": %zu, \"icloud_only\": %zu, \"failed\": %zu,"
                   " \"seconds\": %.3f, \"p50_ms\": %.1f, \"p95_ms\": %.1f, \"max_ms\": %.1f},\n",
                   ids.size(), cloned, copied, cloud, other, open_s, pct(oms, 0.5), pct(oms, 0.95), pct(oms, 1.0));
      [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    }
    std::fprintf(f, "  \"peak_footprint_mb\": %.1f\n}\n", peak_mb());
    std::fclose(f);
  }
  return 0;
}
