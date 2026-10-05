// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PhotoKit behind photos_source (photos_source.h, issue #72). Read-only, local
// only: every image and video request has networkAccessAllowed = NO, and
// nothing here changes the library. Identifiers never reach a log (rule 6).
// The one network read is fetch_video, the opt-in iCloud fetch (2026-10-05).
#include "addons/ai/photos_source.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Network/Network.h>
#import <Photos/Photos.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

@interface MVAIPhotosObserver : NSObject <PHPhotoLibraryChangeObserver>
@end

@implementation MVAIPhotosObserver {
 @public
  std::mutex _m;
  std::function<void()> _changed;
}
- (void)photoLibraryDidChange:(PHChange*)change {
  (void)change;
  // Called with the lock held, so once observe(nullptr) has cleared _changed
  // (under the same lock) no call is still on its way into an engine that is
  // going away. The callback only sets a flag and wakes a thread.
  std::lock_guard lock(_m);
  if (_changed) _changed();
}
@end

namespace mv::ai {
namespace {

photos_access map(PHAuthorizationStatus s) {
  switch (s) {
    case PHAuthorizationStatusNotDetermined: return photos_access::not_determined;
    case PHAuthorizationStatusRestricted:    return photos_access::restricted;
    case PHAuthorizationStatusDenied:        return photos_access::denied;
    case PHAuthorizationStatusAuthorized:    return photos_access::full;
    case PHAuthorizationStatusLimited:       return photos_access::limited;
  }
  return photos_access::denied;
}

// The user's own library: iCloud Photos and local imports, not shared albums
// (someone else's photos) and not the Hidden album (the user hid them).
PHFetchOptions* library_options() {
  PHFetchOptions* o = [[PHFetchOptions alloc] init];
  o.includeHiddenAssets = NO;
  o.includeAllBurstAssets = NO;
  o.includeAssetSourceTypes = PHAssetSourceTypeUserLibrary;
  return o;
}

PHAsset* asset_for(std::string_view id) {
  NSString* s = [[NSString alloc] initWithBytes:id.data() length:id.size() encoding:NSUTF8StringEncoding];
  if (!s) return nil;
  PHFetchResult<PHAsset*>* r = [PHAsset fetchAssetsWithLocalIdentifiers:@[ s ] options:nil];
  return r.firstObject;
}

// Draws `image` into packed sRGB RGB at most `max_edge` on its long side.
// CoreGraphics colour-matches from the image's own profile (D6: a tagged
// image is never read as sRGB), and Photos has already applied orientation.
result<rgb_frame> to_rgb(CGImageRef image, std::uint32_t max_edge) {
  const std::size_t w0 = CGImageGetWidth(image), h0 = CGImageGetHeight(image);
  if (w0 == 0 || h0 == 0) return err(status::corrupt);
  const double scale = std::min(1.0, static_cast<double>(max_edge) / static_cast<double>(std::max(w0, h0)));
  const std::uint32_t w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(w0 * scale + 0.5));
  const std::uint32_t h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(h0 * scale + 0.5));
  std::vector<std::uint8_t> rgbx(static_cast<std::size_t>(w) * h * 4);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(rgbx.data(), w, h, 8, static_cast<std::size_t>(w) * 4, srgb,
                                           static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(srgb);
  if (!ctx) return err(status::out_of_memory);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);
  rgb_frame f;
  f.width = w;
  f.height = h;
  f.rgb.resize(static_cast<std::size_t>(w) * h * 3);
  for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
    f.rgb[i * 3] = rgbx[i * 4];
    f.rgb[i * 3 + 1] = rgbx[i * 4 + 1];
    f.rgb[i * 3 + 2] = rgbx[i * 4 + 2];
  }
  return f;
}

// The resource fetch_video downloads: the rendered current edit when Photos
// keeps one (a trimmed or filtered clip, as Photos plays it), else the
// original video. Not paired Live Photo video (a Live Photo is a photo here).
PHAssetResource* video_resource(PHAsset* a) {
  PHAssetResource* original = nil;
  for (PHAssetResource* r in [PHAssetResource assetResourcesForAsset:a]) {
    if (r.type == PHAssetResourceTypeFullSizeVideo) return r;
    if (r.type == PHAssetResourceTypeVideo && !original) original = r;
  }
  return original;
}

class photokit_source final : public photos_source {
 public:
  photokit_source() {
    // The latest path, read by network_unmetered(); the monitor runs on its
    // own queue from the first source to the last.
    monitor_ = nw_path_monitor_create();
    nw_path_monitor_set_queue(monitor_, dispatch_queue_create("mv.ai.photos.network", DISPATCH_QUEUE_SERIAL));
    std::atomic<bool>* ok = &unmetered_;
    nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
      ok->store(nw_path_get_status(path) == nw_path_status_satisfied && !nw_path_is_expensive(path) &&
                !nw_path_is_constrained(path));
    });
    nw_path_monitor_start(monitor_);
  }
  ~photokit_source() override {
    observe(nullptr);
    if (monitor_) nw_path_monitor_cancel(monitor_);
  }

  bool network_unmetered() const override { return unmetered_.load(); }

  result<std::string> fetch_video(std::string_view id, const std::string& dest_stem,
                                  const std::function<bool(double)>& progress) override {
    if (!readable(access())) return err(status::permission_denied);
    @autoreleasepool {
      PHAsset* a = asset_for(id);
      if (!a) return err(status::not_found);
      if (a.mediaType != PHAssetMediaTypeVideo) return err(status::unsupported_format);
      PHAssetResource* r = video_resource(a);
      if (!r) return err(status::unsupported_format);
      NSString* ext = r.originalFilename.pathExtension.lowercaseString;
      std::string path = dest_stem + "." + (ext.length > 0 && ext.length <= 5 && ext.UTF8String ? ext.UTF8String : "mov");
      std::FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) return err(status::io);
      PHAssetResourceRequestOptions* o = [[PHAssetResourceRequestOptions alloc] init];
      o.networkAccessAllowed = YES;
      // Shared with PhotoKit's queue. It outlives every handler: this waits
      // for the completion handler, which PhotoKit calls after a cancel too.
      struct shared {
        std::atomic<double> fraction{0};
        std::atomic<bool> cancel{false};
        bool write_failed = false;  // the data handler's (serial) only
      } st;
      shared* sp = &st;
      o.progressHandler = ^(double p) { sp->fraction.store(p); };
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      __block NSError* failed = nil;
      const PHAssetResourceDataRequestID req = [[PHAssetResourceManager defaultManager]
          requestDataForAssetResource:r
                              options:o
                  dataReceivedHandler:^(NSData* data) {
                    if (sp->write_failed || sp->cancel.load()) return;
                    if (std::fwrite(data.bytes, 1, data.length, f) != data.length) sp->write_failed = true;
                  }
                    completionHandler:^(NSError* e) {
                      failed = e;
                      dispatch_semaphore_signal(done);
                    }];
      // The caller's progress runs here, on its own thread, four times a second.
      while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC)) != 0) {
        if (!st.cancel.load() && progress && !progress(st.fraction.load())) {
          st.cancel = true;
          [[PHAssetResourceManager defaultManager] cancelDataRequest:req];
        }
      }
      const bool closed = std::fclose(f) == 0;
      if (st.cancel.load() || failed || st.write_failed || !closed) {
        std::remove(path.c_str());
        if (st.cancel.load()) return err(status::cancelled);
        // A full disk, or iCloud would not answer (offline, signed out).
        return err(status::io);
      }
      if (progress) (void)progress(1.0);
      return path;
    }
  }

  photos_access access() const override {
    return map([PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite]);
  }

  expected enumerate(const std::function<bool(const photos_item&)>& visit) override {
    if (!readable(access())) return err(status::permission_denied);
    @autoreleasepool {
      PHFetchResult<PHAsset*>* all = [PHAsset fetchAssetsWithOptions:library_options()];
      __block bool stopped = false;
      [all enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger, BOOL* stop) {
        @autoreleasepool {
          const PHAssetMediaType t = a.mediaType;
          if (t != PHAssetMediaTypeImage && t != PHAssetMediaTypeVideo) return;
          photos_item it;
          const char* id = a.localIdentifier.UTF8String;
          if (!id) return;
          it.id = id;
          NSDate* d = a.modificationDate ?: a.creationDate;
          it.mtime = d ? static_cast<std::int64_t>(d.timeIntervalSince1970) : 0;
          it.size = static_cast<std::uint64_t>(a.pixelWidth) * static_cast<std::uint64_t>(a.pixelHeight);
          it.kind = t == PHAssetMediaTypeVideo ? asset_kind::video : asset_kind::photo;
          if (!visit(it)) {
            stopped = true;
            *stop = YES;
          }
        }
      }];
      if (stopped) return err(status::cancelled);
    }
    return {};
  }

  result<rgb_frame> still(std::string_view id, std::uint32_t max_edge) override {
    if (!readable(access())) return err(status::permission_denied);
    @autoreleasepool {
      PHAsset* a = asset_for(id);
      if (!a) return err(status::not_found);
      PHImageRequestOptions* o = [[PHImageRequestOptions alloc] init];
      o.synchronous = YES;  // a worker thread; PhotoKit answers on it
      o.networkAccessAllowed = NO;
      o.deliveryMode = PHImageRequestOptionsDeliveryModeHighQualityFormat;
      o.resizeMode = PHImageRequestOptionsResizeModeFast;
      o.version = PHImageRequestOptionsVersionCurrent;
      __block NSImage* got = nil;
      __block bool in_cloud = false;
      const CGFloat edge = static_cast<CGFloat>(max_edge);
      [[PHImageManager defaultManager] requestImageForAsset:a
                                                 targetSize:CGSizeMake(edge, edge)
                                                contentMode:PHImageContentModeAspectFit
                                                    options:o
                                              resultHandler:^(NSImage* image, NSDictionary* info) {
                                                got = image;
                                                in_cloud = [info[PHImageResultIsInCloudKey] boolValue];
                                              }];
      if (!got) return err(in_cloud ? status::io : status::corrupt);
      CGImageRef cg = [got CGImageForProposedRect:nullptr context:nil hints:nil];
      if (!cg) return err(status::corrupt);
      return to_rgb(cg, max_edge);
    }
  }

  result<std::string> video_file(std::string_view id) override {
    if (!readable(access())) return err(status::permission_denied);
    @autoreleasepool {
      PHAsset* a = asset_for(id);
      if (!a) return err(status::not_found);
      // Current first (a trimmed or filtered clip as Photos plays it); a
      // slow-motion clip's Current is a composition, so fall back to Original.
      for (PHVideoRequestOptionsVersion v : {PHVideoRequestOptionsVersionCurrent, PHVideoRequestOptionsVersionOriginal}) {
        PHVideoRequestOptions* o = [[PHVideoRequestOptions alloc] init];
        o.networkAccessAllowed = NO;
        o.version = v;
        o.deliveryMode = PHVideoRequestOptionsDeliveryModeHighQualityFormat;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSURL* url = nil;
        __block bool in_cloud = false;
        [[PHImageManager defaultManager] requestAVAssetForVideo:a
                                                        options:o
                                                  resultHandler:^(AVAsset* av, AVAudioMix*, NSDictionary* info) {
                                                    if ([av isKindOfClass:[AVURLAsset class]]) url = ((AVURLAsset*)av).URL;
                                                    in_cloud = [info[PHImageResultIsInCloudKey] boolValue];
                                                    dispatch_semaphore_signal(done);
                                                  }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
          return err(status::timeout);
        }
        if (url.isFileURL && url.path.UTF8String) return std::string(url.path.UTF8String);
        if (in_cloud) return err(status::io);
      }
      return err(status::unsupported_format);
    }
  }

  void observe(std::function<void()> changed) override {
    std::lock_guard lock(m_);
    if (!changed) {
      if (observer_) {
        [[PHPhotoLibrary sharedPhotoLibrary] unregisterChangeObserver:observer_];
        std::lock_guard l2(observer_->_m);
        observer_->_changed = nullptr;
      }
      observer_ = nil;
      return;
    }
    // Registering before access is granted would raise the permission prompt.
    if (!readable(access())) return;
    if (!observer_) {
      observer_ = [[MVAIPhotosObserver alloc] init];
      [[PHPhotoLibrary sharedPhotoLibrary] registerChangeObserver:observer_];
    }
    std::lock_guard l2(observer_->_m);
    observer_->_changed = std::move(changed);
  }

 private:
  std::mutex m_;
  MVAIPhotosObserver* observer_ = nil;
  nw_path_monitor_t monitor_ = nil;
  std::atomic<bool> unmetered_{false};
};

}  // namespace

// macOS 14 is the floor (LSMinimumSystemVersion); every call above exists there.
std::unique_ptr<photos_source> make_photos_source() { return std::make_unique<photokit_source>(); }

}  // namespace mv::ai
