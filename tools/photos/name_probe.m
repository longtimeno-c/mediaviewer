// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Times the Photos library walk of File → Open Photos Library (#420) and
// checks its names. Prints counts and milliseconds only, never an identifier
// or a name (rule 6). Read-only.
//
//   clang -fobjc-arc -framework Foundation -framework Photos tools/photos/name_probe.m -o name_probe
//   ./name_probe walk [limit]        the fetch and walk, no name
//   ./name_probe names [limit]       + originalFilename, its property set fetched with the assets
//   ./name_probe unfetched [limit]   + originalFilename, read one asset at a time
//   ./name_probe resources [limit]   + the resource lookup the listing did before #420
//   ./name_probe compare [limit]     names against the resources, by kind
//   ./name_probe describe [limit]    by-identifier fetches: nil options against fetched names
//
// The process needs Photos access. A command-line tool cannot ask for it: wrap
// it in a bundle with NSPhotoLibraryUsageDescription, sign it with a stable
// identity (a Developer ID keeps the grant across rebuilds), and start it with
// `open -W --stdout <log> Probe.app --args <mode> [limit]` to be asked once.
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>

static double now_ms(void) { return [NSDate date].timeIntervalSince1970 * 1000.0; }

// shell/photos_items_mac.mm with_names().
static PHFetchOptions* with_names(PHFetchOptions* o) {
  if ([o respondsToSelector:@selector(setFetchPropertySets:)]) {
    @try {
      [o setValue:@[ @"PHAssetPropertySetOriginalMetadata" ] forKey:@"fetchPropertySets"];
    } @catch (NSException* e) {
      (void)e;
    }
  }
  return o;
}

static NSString* original_filename(PHAsset* a) {
  @try {
    id v = [a valueForKey:@"originalFilename"];
    return [v isKindOfClass:[NSString class]] ? v : nil;
  } @catch (NSException* e) {
    (void)e;
    return nil;
  }
}

// The resource the listing named an item by before #420: an edit's render first.
static PHAssetResource* old_pick(PHAsset* a, NSArray<PHAssetResource*>* rs) {
  NSArray<NSNumber*>* order = a.mediaType == PHAssetMediaTypeVideo
      ? @[ @(PHAssetResourceTypeFullSizeVideo), @(PHAssetResourceTypeVideo) ]
      : @[ @(PHAssetResourceTypeFullSizePhoto), @(PHAssetResourceTypePhoto) ];
  for (NSNumber* t in order) {
    for (PHAssetResource* r in rs) {
      if (r.type == (PHAssetResourceType)t.integerValue) return r;
    }
  }
  return rs.firstObject;
}

static PHAssetResource* of_type(NSArray<PHAssetResource*>* rs, PHAssetResourceType t) {
  for (PHAssetResource* r in rs) {
    if (r.type == t) return r;
  }
  return nil;
}

static bool video_ext(NSString* name) {
  static NSSet* v;
  if (!v) v = [NSSet setWithArray:@[ @"mov", @"mp4", @"m4v", @"avi", @"mkv", @"webm", @"ts", @"mts", @"m2ts", @"3gp" ]];
  return [v containsObject:name.pathExtension.lowercaseString];
}

static PHFetchOptions* library_options(void) {
  PHFetchOptions* o = [[PHFetchOptions alloc] init];
  o.includeHiddenAssets = NO;
  o.includeAllBurstAssets = NO;
  o.includeAssetSourceTypes = PHAssetSourceTypeUserLibrary;
  o.sortDescriptors = @[ [NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:YES] ];
  return o;
}

int main(int argc, char** argv) {
  @autoreleasepool {
    NSString* mode = argc > 1 ? @(argv[1]) : @"walk";
    const long limit = argc > 2 ? atol(argv[2]) : 0;
    PHAuthorizationStatus s = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
    if (s == PHAuthorizationStatusNotDetermined) {
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                                 handler:^(PHAuthorizationStatus got) {
                                                   (void)got;
                                                   dispatch_semaphore_signal(done);
                                                 }];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      s = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
    }
    if (s != PHAuthorizationStatusAuthorized && s != PHAuthorizationStatusLimited) {
      fprintf(stderr, "no Photos access (status %ld)\n", (long)s);
      return 1;
    }

    if ([mode isEqualToString:@"describe"]) {
      // Identifiers from the library (hidden and shared included), fetched back
      // the way describe() did (nil options) and the way it does now.
      PHFetchOptions* wide = [[PHFetchOptions alloc] init];
      wide.includeHiddenAssets = YES;
      PHFetchResult<PHAsset*>* src = [PHAsset fetchAssetsWithOptions:wide];
      NSMutableArray<NSString*>* ids = [NSMutableArray array];
      for (NSUInteger i = 0; i < src.count && (!limit || (long)ids.count < limit); ++i) {
        [ids addObject:[src objectAtIndex:i].localIdentifier];
      }
      double t0 = now_ms();
      PHFetchResult<PHAsset*>* before = [PHAsset fetchAssetsWithLocalIdentifiers:ids options:nil];
      __block long nb = 0;
      [before enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger idx, BOOL* stop) {
        (void)idx;
        (void)stop;
        if (old_pick(a, [PHAssetResource assetResourcesForAsset:a]).originalFilename.length) ++nb;
      }];
      const double t_before = now_ms() - t0;
      t0 = now_ms();
      PHFetchResult<PHAsset*>* after =
          [PHAsset fetchAssetsWithLocalIdentifiers:ids options:with_names([[PHFetchOptions alloc] init])];
      __block long na = 0;
      [after enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger idx, BOOL* stop) {
        (void)idx;
        (void)stop;
        if (original_filename(a).length) ++na;
      }];
      const double t_after = now_ms() - t0;
      NSMutableSet* sb = [NSMutableSet set];
      NSMutableSet* sa = [NSMutableSet set];
      for (NSUInteger i = 0; i < before.count; ++i) [sb addObject:[before objectAtIndex:i].localIdentifier];
      for (NSUInteger i = 0; i < after.count; ++i) [sa addObject:[after objectAtIndex:i].localIdentifier];
      printf("describe: %lu ids; nil options %lu found, %ld named, %.0f ms; fetched names %lu found, %ld named, "
             "%.0f ms; same set %s\n",
             (unsigned long)ids.count, (unsigned long)before.count, nb, t_before, (unsigned long)after.count, na,
             t_after, [sb isEqualToSet:sa] ? "yes" : "NO");
      return 0;
    }

    const bool names = [mode isEqualToString:@"names"] || [mode isEqualToString:@"compare"];
    PHFetchOptions* o = names ? with_names(library_options()) : library_options();
    const double t0 = now_ms();
    PHFetchResult<PHAsset*>* all = [PHAsset fetchAssetsWithOptions:o];
    __block long n = 0, empty = 0, eq_shot = 0, eq_old = 0, old_render = 0, neither = 0, kind_flips = 0, videos = 0;
    [all enumerateObjectsUsingBlock:^(PHAsset* a, NSUInteger idx, BOOL* stop) {
      @autoreleasepool {
        (void)idx;
        if (limit && n >= limit) {
          *stop = YES;
          return;
        }
        if (a.mediaType != PHAssetMediaTypeImage && a.mediaType != PHAssetMediaTypeVideo) return;
        ++n;
        (void)a.localIdentifier;
        (void)a.creationDate;
        (void)a.modificationDate;
        (void)a.location;
        if ([mode isEqualToString:@"names"] || [mode isEqualToString:@"unfetched"]) {
          if (!original_filename(a).length) ++empty;
        } else if ([mode isEqualToString:@"resources"]) {
          if (!old_pick(a, [PHAssetResource assetResourcesForAsset:a]).originalFilename.length) ++empty;
        } else if ([mode isEqualToString:@"compare"]) {
          NSString* f = original_filename(a);
          if (!f.length) ++empty;
          NSArray<PHAssetResource*>* rs = [PHAssetResource assetResourcesForAsset:a];
          const bool video = a.mediaType == PHAssetMediaTypeVideo;
          NSString* shot = of_type(rs, video ? PHAssetResourceTypeVideo : PHAssetResourceTypePhoto).originalFilename;
          NSString* old = old_pick(a, rs).originalFilename;
          if (video) ++videos;
          if ([f isEqualToString:shot]) ++eq_shot;
          if ([f isEqualToString:old]) {
            ++eq_old;
          } else if ([old hasPrefix:@"FullSizeRender"]) {
            ++old_render;
          }
          if (![f isEqualToString:shot] && ![f isEqualToString:old]) ++neither;
          if (video_ext(f) != video_ext(old)) ++kind_flips;
        }
      }
    }];
    printf("%s: %ld assets in %.0f ms (no name %ld)\n", mode.UTF8String, n, now_ms() - t0, empty);
    if ([mode isEqualToString:@"compare"]) {
      printf("  same as the original resource %ld; same as the old pick %ld; old pick an edit's FullSizeRender %ld; "
             "neither %ld; clip/still by extension flips %ld (videos %ld)\n",
             eq_shot, eq_old, old_render, neither, kind_flips, videos);
    }
  }
  return 0;
}
