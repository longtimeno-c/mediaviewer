// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PhotoKit behind photos_items_mac.h. Read-only: nothing here changes the
// library. Local unless resolve() is asked for the original (the one network
// request, on the user's own viewing or copy). Identifiers never reach a log.
#include "shell/photos_items_mac.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "core/trace.h"
#include "image/thumb.h"
#include "io/file_port.h"

namespace mv::shell::photos {
namespace {

NSString* ns(std::string_view s) {
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

std::string utf8(NSString* s) { return s.UTF8String ? std::string(s.UTF8String) : std::string(); }

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The user's own library: iCloud Photos and local imports, not shared albums
// (someone else's photos) and not the Hidden album (the user hid them).
PHFetchOptions* library_options() {
  PHFetchOptions* o = [[PHFetchOptions alloc] init];
  o.includeHiddenAssets = NO;
  o.includeAllBurstAssets = NO;
  o.includeAssetSourceTypes = PHAssetSourceTypeUserLibrary;
  o.sortDescriptors = @[ [NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:YES] ];
  return o;
}

PHAsset* asset_for(std::string_view key) {
  if (!is_key(key)) return nil;
  NSString* s = ns(key.substr(kKeyPrefix.size()));
  if (!s) return nil;
  return [PHAsset fetchAssetsWithLocalIdentifiers:@[ s ] options:nil].firstObject;
}

// The original's resource, in order of preference. A Live Photo's still, a
// RAW+JPEG pair's JPEG (its RAW is the alternate), an edited clip's original.
PHAssetResource* original_resource(PHAsset* a) {
  NSArray<PHAssetResource*>* rs = [PHAssetResource assetResourcesForAsset:a];
  NSArray<NSNumber*>* order = a.mediaType == PHAssetMediaTypeVideo
      ? @[ @(PHAssetResourceTypeFullSizeVideo), @(PHAssetResourceTypeVideo) ]
      : @[ @(PHAssetResourceTypeFullSizePhoto), @(PHAssetResourceTypePhoto) ];
  for (NSNumber* t in order) {
    for (PHAssetResource* r in rs) {
      if (r.type == static_cast<PHAssetResourceType>(t.integerValue)) return r;
    }
  }
  return rs.firstObject;
}

std::string base_name(PHAsset* a) {
  PHAssetResource* r = original_resource(a);
  const std::string name = r ? utf8(r.originalFilename) : std::string();
  return name.empty() ? std::string("Photo") : name;
}

item item_of(PHAsset* a) {
  item it;
  it.key = std::string(kKeyPrefix) + utf8(a.localIdentifier);
  it.name = base_name(a);
  NSDate* m = a.modificationDate ?: a.creationDate;
  it.mtime = m ? static_cast<std::int64_t>(m.timeIntervalSince1970) : 0;
  it.created = a.creationDate ? static_cast<std::int64_t>(a.creationDate.timeIntervalSince1970) : it.mtime;
  it.size = static_cast<std::uint64_t>(a.pixelWidth) * static_cast<std::uint64_t>(a.pixelHeight);
  it.video = a.mediaType == PHAssetMediaTypeVideo;
  return it;
}

// The largest picture of `a` this Mac has, without the network. A synchronous
// request is answered in full quality or not at all, so the sizes are asked
// in turn, largest first; Optimize Mac Storage keeps a screen-sized one.
// `first_edge` 0 starts at the full size and still tries the smaller ones
// (it skipped them all, so an iCloud-only photo had no preview, 2026-10-05).
NSImage* best_local_image(PHAsset* a, CGFloat first_edge) {
  PHImageRequestOptions* o = [[PHImageRequestOptions alloc] init];
  o.synchronous = YES;
  o.networkAccessAllowed = NO;
  o.deliveryMode = PHImageRequestOptionsDeliveryModeHighQualityFormat;
  o.resizeMode = PHImageRequestOptionsResizeModeFast;
  o.version = PHImageRequestOptionsVersionCurrent;
  CGFloat edges[] = {first_edge, 2048, 1024, 512, 256, 128};
  for (CGFloat edge : edges) {
    if (first_edge > 0 && edge > first_edge) continue;
    __block NSImage* got = nil;
    const CGSize size = edge > 0 ? CGSizeMake(edge, edge) : PHImageManagerMaximumSize;
    [[PHImageManager defaultManager] requestImageForAsset:a
                                               targetSize:size
                                              contentMode:PHImageContentModeAspectFit
                                                  options:o
                                            resultHandler:^(NSImage* image, NSDictionary*) {
                                              got = image;
                                            }];
    if (got) return got;
  }
  return nil;
}

// Packed RGBA, <= max_edge on the long side, colour-matched to sRGB by
// CoreGraphics (D6: a tagged picture is never read as sRGB).
bool to_rgba(CGImageRef image, std::uint32_t max_edge, std::vector<std::uint8_t>& rgba,
             std::uint32_t& w, std::uint32_t& h) {
  const std::size_t w0 = CGImageGetWidth(image), h0 = CGImageGetHeight(image);
  if (w0 == 0 || h0 == 0) return false;
  const double scale = std::min(1.0, static_cast<double>(max_edge) / static_cast<double>(std::max(w0, h0)));
  w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(w0 * scale + 0.5));
  h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(h0 * scale + 0.5));
  rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, static_cast<std::size_t>(w) * 4, srgb,
                                           static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(srgb);
  if (!ctx) return false;
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);
  for (std::size_t i = 3, n = rgba.size(); i < n; i += 4) rgba[i] = 255;
  return true;
}

NSURL* cache_url() { return [NSURL fileURLWithPath:ns(cache_dir()) isDirectory:YES]; }

resolved stamped(std::string path, bool preview, bool downloaded) {
  resolved r;
  r.path = std::move(path);
  r.preview = preview;
  r.downloaded = downloaded;
  if (auto st = io::stat_path(r.path)) {
    r.mtime_unix = st->mtime_unix;
    r.size = st->size;
  }
  return r;
}

// One folder per asset version (an edit in Photos makes a new one).
NSURL* folder_for(PHAsset* a, std::string_view key, NSString* kind) {
  NSDate* d = a.modificationDate ?: a.creationDate;
  const long long stamp = d ? static_cast<long long>(d.timeIntervalSince1970) : 0;
  NSString* id = [ns(key.substr(kKeyPrefix.size())) stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
  NSString* name = [NSString stringWithFormat:@"%@-%lld", id, stamp];
  return [[cache_url() URLByAppendingPathComponent:kind isDirectory:YES] URLByAppendingPathComponent:name
                                                                                       isDirectory:YES];
}

// The current rendition's file when it is on this Mac (never the network).
NSURL* local_image_url(PHAsset* a) {
  PHContentEditingInputRequestOptions* o = [[PHContentEditingInputRequestOptions alloc] init];
  o.networkAccessAllowed = NO;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block NSURL* url = nil;
  [a requestContentEditingInputWithOptions:o
                         completionHandler:^(PHContentEditingInput* input, NSDictionary*) {
                           url = input.fullSizeImageURL;
                           dispatch_semaphore_signal(done);
                         }];
  if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) return nil;
  return url.isFileURL ? url : nil;
}

// Current first (a trimmed or filtered clip as Photos plays it); an edited
// clip's Current can be a composition with no file, so then the original.
NSURL* local_video_url(PHAsset* a) {
  for (PHVideoRequestOptionsVersion v : {PHVideoRequestOptionsVersionCurrent, PHVideoRequestOptionsVersionOriginal}) {
    PHVideoRequestOptions* o = [[PHVideoRequestOptions alloc] init];
    o.networkAccessAllowed = NO;
    o.version = v;
    o.deliveryMode = PHVideoRequestOptionsDeliveryModeHighQualityFormat;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSURL* url = nil;
    [[PHImageManager defaultManager] requestAVAssetForVideo:a
                                                    options:o
                                              resultHandler:^(AVAsset* av, AVAudioMix*, NSDictionary*) {
                                                if ([av isKindOfClass:[AVURLAsset class]]) url = ((AVURLAsset*)av).URL;
                                                dispatch_semaphore_signal(done);
                                              }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) return nil;
    if (url.isFileURL) return url;
  }
  return nil;
}

// One resource's bytes to `dst`, from this Mac or from iCloud (the network
// request), through a part file beside it: a cancel or a failure leaves
// nothing in place. The chunked request rather than writeData...: it hands
// back a request id the cancel can stop.
expected stream_resource(PHAssetResource* res, NSURL* dst, const std::atomic<bool>* cancel) {
  if (!res || !dst) return err(status::invalid_arg);
  NSFileManager* fm = [NSFileManager defaultManager];
  NSURL* dir = dst.URLByDeletingLastPathComponent;
  [fm createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
  NSURL* part = [dir URLByAppendingPathComponent:[dst.lastPathComponent stringByAppendingString:@".part"]];
  [fm removeItemAtURL:part error:nil];
  NSFileHandle* sink = ({
    [fm createFileAtPath:part.path contents:nil attributes:nil];
    [NSFileHandle fileHandleForWritingToURL:part error:nil];
  });
  if (!sink) return err(status::io);
  PHAssetResourceRequestOptions* o = [[PHAssetResourceRequestOptions alloc] init];
  o.networkAccessAllowed = YES;
  PHAssetResourceManager* mgr = [PHAssetResourceManager defaultManager];
  __block PHAssetResourceDataRequestID request = PHInvalidAssetResourceDataRequestID;
  __block bool stopped = false;
  __block bool write_failed = false;
  if (cancel) {
    o.progressHandler = ^(double) {
      if (cancel->load(std::memory_order_acquire) && !stopped) {
        stopped = true;
        [mgr cancelDataRequest:request];
      }
    };
  }
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block bool ok = false;
  request = [mgr requestDataForAssetResource:res
                                     options:o
                         dataReceivedHandler:^(NSData* data) {
                           if (write_failed) return;
                           NSError* werr = nil;
                           if (![sink writeData:data error:&werr]) write_failed = true;
                           if (cancel && cancel->load(std::memory_order_acquire) && !stopped) {
                             stopped = true;
                             [mgr cancelDataRequest:request];
                           }
                         }
                           completionHandler:^(NSError* error) {
                             ok = error == nil;
                             dispatch_semaphore_signal(done);
                           }];
  // No deadline: a 4K clip over a slow link takes what it takes; cancel stops it.
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  [sink closeFile];
  if (!ok || write_failed || (cancel && cancel->load(std::memory_order_acquire))) {
    [fm removeItemAtURL:part error:nil];
    return err(cancel && cancel->load() ? status::cancelled : status::io);
  }
  [fm removeItemAtURL:dst error:nil];
  if (![fm moveItemAtURL:part toURL:dst error:nil]) {
    [fm removeItemAtURL:part error:nil];
    return err(status::io);
  }
  return {};
}

// The original from iCloud, into `dir`, for viewing or copying out.
result<std::string> download_original(PHAsset* a, NSURL* dir, const std::atomic<bool>* cancel) {
  PHAssetResource* res = original_resource(a);
  if (!res) return err(status::unsupported_format);
  NSString* file = res.originalFilename.length ? res.originalFilename : @"Original";
  NSURL* dst = [dir URLByAppendingPathComponent:file];
  if ([[NSFileManager defaultManager] fileExistsAtPath:dst.path]) return utf8(dst.path);
  if (auto streamed = stream_resource(res, dst, cancel); !streamed) return err(streamed.error());
  return utf8(dst.path);
}

// The backup's view of an asset: its ORIGINAL files, by backup kind (0 the
// photo or video as shot, 1 a Live Photo's paired video, 2 a RAW+JPEG pair's
// RAW). Not the edited renditions.
PHAssetResource* backup_resource(PHAsset* a, int kind) {
  NSArray<PHAssetResource*>* rs = [PHAssetResource assetResourcesForAsset:a];
  PHAssetResourceType want;
  switch (kind) {
    case 1: want = PHAssetResourceTypePairedVideo; break;
    case 2: want = PHAssetResourceTypeAlternatePhoto; break;
    default: want = a.mediaType == PHAssetMediaTypeVideo ? PHAssetResourceTypeVideo : PHAssetResourceTypePhoto; break;
  }
  for (PHAssetResource* r in rs) {
    if (r.type == want) return r;
  }
  return nil;
}

}  // namespace

bool added() noexcept { return [[NSUserDefaults standardUserDefaults] boolForKey:@(kAddedDefault)]; }
void set_added(bool on) noexcept { [[NSUserDefaults standardUserDefaults] setBool:on forKey:@(kAddedDefault)]; }

bool readable() noexcept {
  const PHAuthorizationStatus s = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
  return s == PHAuthorizationStatusAuthorized || s == PHAuthorizationStatusLimited;
}

bool available() noexcept { return added() && readable(); }

result<std::vector<item>> enumerate() {
  if (!readable()) return err(status::permission_denied);
  std::vector<item> out;
  @autoreleasepool {
    const double t0 = now_s();
    PHFetchResult<PHAsset*>* all = [PHAsset fetchAssetsWithOptions:library_options()];
    out.reserve(static_cast<std::size_t>(all.count));
    std::vector<item>* sink = &out;  // a block captures C++ objects by const copy
    [all enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger, BOOL*) {
      @autoreleasepool {
        const PHAssetMediaType t = a.mediaType;
        if (t != PHAssetMediaTypeImage && t != PHAssetMediaTypeVideo) return;
        if (!a.localIdentifier.UTF8String) return;
        sink->push_back(item_of(a));
      }
    }];
    MV_LOG_INFO("photos: listed %zu assets in %.0f ms", out.size(), (now_s() - t0) * 1000.0);
  }
  return out;
}

result<std::vector<std::optional<item>>> describe(std::span<const std::string> keys) {
  if (!readable()) return err(status::permission_denied);
  std::vector<std::optional<item>> out(keys.size());
  @autoreleasepool {
    NSMutableArray<NSString*>* ids = [NSMutableArray arrayWithCapacity:keys.size()];
    for (const std::string& k : keys) {
      if (!is_key(k)) continue;
      if (NSString* s = ns(std::string_view(k).substr(kKeyPrefix.size()))) [ids addObject:s];
    }
    if (ids.count == 0) return out;
    std::unordered_map<std::string, item> by_key;
    std::unordered_map<std::string, item>* sink = &by_key;
    PHFetchResult<PHAsset*>* found = [PHAsset fetchAssetsWithLocalIdentifiers:ids options:nil];
    [found enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger, BOOL*) {
      @autoreleasepool {
        if (a.mediaType != PHAssetMediaTypeImage && a.mediaType != PHAssetMediaTypeVideo) return;
        item it = item_of(a);
        std::string key = it.key;
        sink->emplace(std::move(key), std::move(it));
      }
    }];
    for (std::size_t i = 0; i < keys.size(); ++i) {
      auto hit = by_key.find(keys[i]);
      if (hit != by_key.end()) out[i] = hit->second;
    }
  }
  return out;
}

result<std::vector<std::uint8_t>> thumb_jpeg(std::string_view key, std::uint32_t max_edge) {
  if (!readable()) return err(status::permission_denied);
  @autoreleasepool {
    PHAsset* a = asset_for(key);
    if (!a) return err(status::not_found);
    NSImage* got = best_local_image(a, static_cast<CGFloat>(max_edge));
    if (!got) return err(status::io);
    CGImageRef cg = [got CGImageForProposedRect:nullptr context:nil hints:nil];
    if (!cg) return err(status::corrupt);
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!to_rgba(cg, max_edge, rgba, w, h)) return err(status::corrupt);
    return image::encode_thumb_rgba(rgba, w, h);
  }
}

result<resolved> resolve(std::string_view key, bool want_original, const std::atomic<bool>* cancel) {
  if (!readable()) return err(status::permission_denied);
  @autoreleasepool {
    PHAsset* a = asset_for(key);
    if (!a) return err(status::not_found);
    // In place, read-only, when this Mac has the current rendition.
    NSURL* local = a.mediaType == PHAssetMediaTypeVideo ? local_video_url(a) : local_image_url(a);
    if (local) return stamped(utf8(local.path), false, false);
    if (cancel && cancel->load(std::memory_order_acquire)) return err(status::cancelled);
    if (want_original) {
      auto got = download_original(a, folder_for(a, key, @"icloud"), cancel);
      if (!got) return err(got.error());
      return stamped(std::move(got).value(), false, true);
    }
    // Only iCloud has it: the best picture this Mac holds, said to be a preview.
    NSURL* dir = folder_for(a, key, @"preview");
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* base = [ns(base_name(a)) stringByDeletingPathExtension];
    NSURL* dst = [[dir URLByAppendingPathComponent:[base stringByAppendingString:@" (preview)"]]
        URLByAppendingPathExtension:@"jpg"];
    if ([fm fileExistsAtPath:dst.path]) return stamped(utf8(dst.path), true, false);
    [fm createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSImage* got = best_local_image(a, 0);
    if (!got) return err(status::io);
    CGImageRef cg = [got CGImageForProposedRect:nullptr context:nil hints:nil];
    if (!cg) return err(status::corrupt);
    CGImageDestinationRef out = CGImageDestinationCreateWithURL((__bridge CFURLRef)dst, CFSTR("public.jpeg"), 1, nullptr);
    if (!out) return err(status::io);
    NSDictionary* props = @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality : @0.92};
    CGImageDestinationAddImage(out, cg, (__bridge CFDictionaryRef)props);
    const bool ok = CGImageDestinationFinalize(out);
    CFRelease(out);
    if (!ok) return err(status::io);
    return stamped(utf8(dst.path), true, false);
  }
}

result<std::vector<library_file>> enumerate_files(const std::atomic<bool>* cancel) {
  if (!readable()) return err(status::permission_denied);
  std::vector<library_file> out;
  @autoreleasepool {
    PHFetchOptions* o = library_options();
    o.includeHiddenAssets = YES;  // a backup keeps what the user hid, too
    PHFetchResult<PHAsset*>* all = [PHAsset fetchAssetsWithOptions:o];
    out.reserve(static_cast<std::size_t>(all.count));
    std::vector<library_file>* sink = &out;
    __block bool stopped = false;
    [all enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger, BOOL* stop) {
      @autoreleasepool {
        if (cancel && cancel->load(std::memory_order_acquire)) {
          stopped = true;
          *stop = YES;
          return;
        }
        const PHAssetMediaType t = a.mediaType;
        if (t != PHAssetMediaTypeImage && t != PHAssetMediaTypeVideo) return;
        if (!a.localIdentifier.UTF8String) return;
        const std::string key = std::string(kKeyPrefix) + utf8(a.localIdentifier);
        const std::int64_t created =
            a.creationDate ? static_cast<std::int64_t>(a.creationDate.timeIntervalSince1970) : 0;
        for (PHAssetResource* r in [PHAssetResource assetResourcesForAsset:a]) {
          int kind = -1;
          switch (r.type) {
            case PHAssetResourceTypePhoto:
            case PHAssetResourceTypeVideo: kind = 0; break;
            case PHAssetResourceTypePairedVideo: kind = 1; break;
            case PHAssetResourceTypeAlternatePhoto: kind = 2; break;
            default: break;
          }
          if (kind < 0) continue;
          library_file f;
          f.key = key;
          f.kind = kind;
          f.name = utf8(r.originalFilename);
          if (f.name.empty()) f.name = kind == 1 ? "Live Photo.MOV" : "Photo";
          f.created = created;
          sink->push_back(std::move(f));
        }
      }
    }];
    if (stopped) return err(status::cancelled);
  }
  return out;
}

result<std::string> local_original(std::string_view key, int kind) {
  if (!readable()) return err(status::permission_denied);
  if (kind != 0) return std::string();  // paired and alternate files have no path API: fetched
  @autoreleasepool {
    PHAsset* a = asset_for(key);
    if (!a) return err(status::not_found);
    if (a.mediaType == PHAssetMediaTypeVideo) {
      PHVideoRequestOptions* o = [[PHVideoRequestOptions alloc] init];
      o.networkAccessAllowed = NO;
      o.version = PHVideoRequestOptionsVersionOriginal;
      o.deliveryMode = PHVideoRequestOptionsDeliveryModeHighQualityFormat;
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      __block NSURL* url = nil;
      [[PHImageManager defaultManager] requestAVAssetForVideo:a
                                                      options:o
                                                resultHandler:^(AVAsset* av, AVAudioMix*, NSDictionary*) {
                                                  if ([av isKindOfClass:[AVURLAsset class]]) url = ((AVURLAsset*)av).URL;
                                                  dispatch_semaphore_signal(done);
                                                }];
      if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) return std::string();
      return url.isFileURL ? utf8(url.path) : std::string();
    }
    // The editing input's full-size image is the original only while the photo
    // has no edits: for an edited one Photos hands back its render (a JPEG of
    // the edit, FullSizeRender), which the backup would file under the
    // original's name. An edited photo has a full-size render (or adjustment
    // data) among its resources; then the original is streamed from its own
    // resource instead (fetch_file: a local read when this Mac has it).
    for (PHAssetResource* r in [PHAssetResource assetResourcesForAsset:a]) {
      if (r.type == PHAssetResourceTypeFullSizePhoto || r.type == PHAssetResourceTypeAdjustmentData) {
        return std::string();
      }
    }
    NSURL* url = local_image_url(a);
    return url ? utf8(url.path) : std::string();
  }
}

expected fetch_file(std::string_view key, int kind, std::string_view dest_utf8, const std::atomic<bool>* cancel) {
  if (!readable()) return err(status::permission_denied);
  @autoreleasepool {
    PHAsset* a = asset_for(key);
    if (!a) return err(status::not_found);
    PHAssetResource* res = backup_resource(a, kind);
    if (!res) return err(status::not_found);
    return stream_resource(res, [NSURL fileURLWithPath:ns(dest_utf8)], cancel);
  }
}

namespace {

// Photos.sdef: `spotlight` is event IPXS/spot; a media item is class IPmi and
// its scripting id is PHAsset.localIdentifier. Built as a descriptor, not as
// script source, so the identifier is data and nothing is compiled.
NSAppleEventDescriptor* spotlight_event(NSString* local_id) {
  NSAppleEventDescriptor* spec = [NSAppleEventDescriptor recordDescriptor];
  [spec setDescriptor:[NSAppleEventDescriptor descriptorWithTypeCode:'IPmi'] forKeyword:keyAEDesiredClass];
  [spec setDescriptor:[NSAppleEventDescriptor nullDescriptor] forKeyword:keyAEContainer];
  [spec setDescriptor:[NSAppleEventDescriptor descriptorWithEnumCode:formUniqueID] forKeyword:keyAEKeyForm];
  [spec setDescriptor:[NSAppleEventDescriptor descriptorWithString:local_id] forKeyword:keyAEKeyData];
  spec = [spec coerceToDescriptorType:typeObjectSpecifier];
  if (!spec) return nil;
  NSAppleEventDescriptor* ev = [NSAppleEventDescriptor
      appleEventWithEventClass:'IPXS'
                       eventID:'spot'
              targetDescriptor:[NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.Photos"]
                      returnID:kAutoGenerateReturnID
                 transactionID:kAnyTransactionID];
  [ev setParamDescriptor:spec forKeyword:keyDirectObject];
  return ev;
}

}  // namespace

bool show_in_photos(std::string_view key) noexcept {
  if (!is_key(key)) return false;
  @autoreleasepool {
    NSString* local_id = ns(key.substr(kKeyPrefix.size()));
    NSURL* app = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.apple.Photos"];
    if (!local_id || !app) return false;
    NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
    config.activates = YES;
    // Launch (or bring forward) first: an Apple Event to an app that is not
    // running fails. The completion runs on a private queue, never main;
    // the send waits for Photos' reply there, and the first one also waits
    // on the user's answer to the Automation prompt.
    [[NSWorkspace sharedWorkspace]
        openApplicationAtURL:app
               configuration:config
           completionHandler:^(NSRunningApplication* running, NSError* error) {
             if (!running || error) return;
             NSAppleEventDescriptor* ev = spotlight_event(local_id);
             if (!ev) return;
             // A Photos that is still launching cannot take the event yet;
             // try again for a few seconds. Any other answer is final:
             // consent refused (errAEEventNotPermitted) or an asset Photos
             // does not show leaves Photos open, unselected.
             for (int attempt = 0; attempt < 10; ++attempt) {
               NSError* send_error = nil;
               [ev sendEventWithOptions:NSAppleEventSendWaitForReply timeout:30 error:&send_error];
               if (!send_error || (send_error.code != procNotFound && send_error.code != connectionInvalid)) return;
               [NSThread sleepForTimeInterval:0.5];
             }
           }];
  }
  return true;
}

std::string cache_dir() {
  NSArray<NSString*>* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
  NSString* base = caches.firstObject ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches"];
  return utf8([[base stringByAppendingPathComponent:@"MediaViewer"] stringByAppendingPathComponent:@"Photos Library"]);
}

void clear_downloads() noexcept {
  @autoreleasepool {
    [[NSFileManager defaultManager] removeItemAtURL:[cache_url() URLByAppendingPathComponent:@"icloud" isDirectory:YES]
                                              error:nil];
  }
}

void clear_cache() noexcept {
  @autoreleasepool {
    [[NSFileManager defaultManager] removeItemAtURL:cache_url() error:nil];
  }
}

}  // namespace mv::shell::photos
