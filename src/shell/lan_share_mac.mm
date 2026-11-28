// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#define __STDC_WANT_LIB_EXT1__ 1  // memset_s, before any header pulls in <string.h>
#include "shell/lan_share_mac.h"

#import <Foundation/Foundation.h>
#import <NetFS/NetFS.h>

#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>

#include <string.h>  // memset_s: a wipe the compiler keeps

#include <cstring>
#include <memory>

namespace mv::shell::lan_share_mac {
namespace ls = remote::lan_share;

std::vector<volume> smb_volumes() {
  std::vector<volume> out;
  const int n = getfsstat(nullptr, 0, MNT_NOWAIT);
  if (n <= 0) return out;
  std::vector<struct statfs> all(static_cast<std::size_t>(n) + 4);
  const int got = getfsstat(all.data(), static_cast<int>(all.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int i = 0; i < got; ++i) {
    const struct statfs& f = all[static_cast<std::size_t>(i)];
    if (std::strncmp(f.f_fstypename, "smbfs", sizeof f.f_fstypename) != 0) continue;
    auto m = ls::parse_mounted_from(f.f_mntfromname);
    if (!m) continue;
    out.push_back(volume{std::move(*m), f.f_mntonname});
  }
  return out;
}

std::vector<ls::mounted> mounted_now() {
  std::vector<ls::mounted> out;
  for (auto& v : smb_volumes()) out.push_back(std::move(v.from));
  return out;
}

bool needs_sign_in(int st) noexcept {
  return st == EAUTH || st == EACCES || st == EPERM || st == -128 /* userCanceledErr */ ||
         st == ENETFSPWDNEEDSCHANGE || st == ENETFSPWDPOLICY || st == ENETFSACCOUNTRESTRICTED;
}

namespace {

struct run {
  std::vector<std::string> urls;
  std::size_t next = 0;
  bool allow_ui = false;
  std::string user, password;
  ls::outcome got;
  ~run() { (void)memset_s(password.data(), password.size(), 0, password.size()); }
  std::function<void(ls::outcome)> done;
};

dispatch_queue_t mount_queue() {
  static dispatch_queue_t q = dispatch_queue_create("mediaviewer.lan-share", DISPATCH_QUEUE_SERIAL);
  return q;
}

void finish(const std::shared_ptr<run>& r) {
  auto done = std::move(r->done);
  const ls::outcome o = r->got;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (done) done(o);
  });
}

// One share at a time: a sign-in the OS asks for once covers the rest of the server's shares.
void step(const std::shared_ptr<run>& r) {
  if (r->next >= r->urls.size()) {
    finish(r);
    return;
  }
  NSString* text = [NSString stringWithUTF8String:r->urls[r->next++].c_str()];
  NSURL* url = text ? [NSURL URLWithString:text] : nil;
  if (!url) {
    ++r->got.failed;
    step(r);
    return;
  }
  NSMutableDictionary* open = [NSMutableDictionary dictionary];
  open[(__bridge NSString*)kNAUIOptionKey] =
      (__bridge NSString*)(r->allow_ui ? kNAUIOptionAllowUI : kNAUIOptionNoUI);
  NSMutableDictionary* mount = [NSMutableDictionary dictionary];
  AsyncRequestID request = nullptr;
  std::shared_ptr<run> keep = r;
  NSString* user = r->user.empty() ? nil : [NSString stringWithUTF8String:r->user.c_str()];
  NSString* password = r->password.empty() ? nil : [NSString stringWithUTF8String:r->password.c_str()];
  const int started = NetFSMountURLAsync(
      (__bridge CFURLRef)url, nullptr, (__bridge CFStringRef)user, (__bridge CFStringRef)password,
      (__bridge CFMutableDictionaryRef)open,
      (__bridge CFMutableDictionaryRef)mount, &request, mount_queue(), ^(int st, AsyncRequestID, CFArrayRef) {
        if (st == 0 || st == EEXIST) {
          ++keep->got.mounted;
        } else if (needs_sign_in(st)) {
          keep->got.needs_sign_in = true;
          // No sign-in works for this server now: the rest would ask (or fail) the same way.
          keep->got.failed += static_cast<int>(keep->urls.size() - keep->next) + 1;
          keep->next = keep->urls.size();
        } else {
          ++keep->got.failed;
        }
        step(keep);
      });
  if (started != 0) {
    ++r->got.failed;
    dispatch_async(mount_queue(), ^{
      step(keep);
    });
  }
}

}  // namespace

void mount(std::vector<std::string> urls, bool allow_ui, std::string user, std::string password,
           std::function<void(ls::outcome)> done) {
  auto r = std::make_shared<run>();
  r->urls = std::move(urls);
  r->allow_ui = allow_ui;
  r->user = std::move(user);
  r->password = std::move(password);
  (void)memset_s(password.data(), password.size(), 0, password.size());
  r->got.asked = allow_ui;
  r->done = std::move(done);
  dispatch_async(mount_queue(), ^{
    step(r);
  });
}

void unmount(std::string host, std::function<void(int, int)> done) {
  dispatch_async(mount_queue(), ^{
    std::vector<volume> mine;
    for (auto& v : smb_volumes()) {
      if (!ls::mounted_from(host, {v.from}).empty()) mine.push_back(std::move(v));
    }
    struct tally {
      int left = 0, ok = 0, failed = 0;
      std::function<void(int, int)> done;
    };
    auto t = std::make_shared<tally>();
    t->left = static_cast<int>(mine.size());
    t->done = done;
    if (mine.empty()) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (t->done) t->done(0, 0);
      });
      return;
    }
    for (const auto& v : mine) {
      NSURL* at = [NSURL fileURLWithPath:[NSString stringWithUTF8String:v.path.c_str()] isDirectory:YES];
      [[NSFileManager defaultManager]
          unmountVolumeAtURL:at
                     options:NSFileManagerUnmountWithoutUI
           completionHandler:^(NSError* error) {
             dispatch_async(dispatch_get_main_queue(), ^{
               if (error) {
                 ++t->failed;
               } else {
                 ++t->ok;
               }
               if (--t->left == 0 && t->done) t->done(t->ok, t->failed);
             });
           }];
    }
  });
}

}  // namespace mv::shell::lan_share_mac
