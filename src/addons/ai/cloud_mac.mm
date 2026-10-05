// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// iCloud Drive for cloud_files.h: an evicted (dataless) file in an indexed
// folder is brought down with NSFileManager's ubiquitous-item calls and
// evicted again once indexed. Another sync client's File Provider files are
// not ubiquitous items and answer unsupported_format: they stay unindexed.
#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "addons/ai/cloud_files.h"

namespace mv::ai {
namespace {

bool dataless(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return false;
#ifdef SF_DATALESS
  return (st.st_flags & SF_DATALESS) != 0;
#else
  return false;
#endif
}

class icloud_drive final : public cloud_files {
 public:
  icloud_drive() {
    monitor_ = nw_path_monitor_create();
    nw_path_monitor_set_queue(monitor_, dispatch_queue_create("mv.ai.cloud.network", DISPATCH_QUEUE_SERIAL));
    std::atomic<bool>* ok = &unmetered_;
    nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t p) {
      ok->store(nw_path_get_status(p) == nw_path_status_satisfied && !nw_path_is_expensive(p) &&
                !nw_path_is_constrained(p));
    });
    nw_path_monitor_start(monitor_);
  }
  ~icloud_drive() override {
    if (monitor_) nw_path_monitor_cancel(monitor_);
  }

  expected hydrate(const std::string& path, const std::function<bool(double)>& progress) override {
    @autoreleasepool {
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      if (!url) return err(status::invalid_arg);
      NSNumber* ubiquitous = nil;
      [url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil];
      if (!ubiquitous.boolValue) return err(status::unsupported_format);
      NSError* e = nil;
      if (![[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:url error:&e]) {
        return err(status::io);
      }
      // No byte progress from this call: report time waiting, and finish when
      // the file is no longer dataless (or iCloud says it is current).
      const auto start = std::chrono::steady_clock::now();
      for (;;) {
        if (!dataless(path)) break;
        NSURL* fresh = [NSURL fileURLWithPath:url.path];
        NSString* st = nil;
        [fresh getResourceValue:&st forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
        if ([st isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) break;
        NSError* down = nil;
        [fresh getResourceValue:&down forKey:NSURLUbiquitousItemDownloadingErrorKey error:nil];
        if (down) return err(status::io);
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (waited > 600) return err(status::io);  // iCloud did not answer: not again this run
        if (progress && !progress(std::min(0.95, waited / 60.0))) return err(status::cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
      if (progress) (void)progress(1.0);
      return {};
    }
  }

  expected dehydrate(const std::string& path) override {
    @autoreleasepool {
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      NSError* e = nil;
      return url && [[NSFileManager defaultManager] evictUbiquitousItemAtURL:url error:&e] ? expected{}
                                                                                             : err(status::io);
    }
  }

  bool network_unmetered() override { return unmetered_.load(); }

 private:
  nw_path_monitor_t monitor_ = nullptr;
  std::atomic<bool> unmetered_{false};
};

}  // namespace

std::unique_ptr<cloud_files> make_cloud_files() { return std::make_unique<icloud_drive>(); }

}  // namespace mv::ai
