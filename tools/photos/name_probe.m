// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Times the Photos library walk of File → Open Photos Library (#420) three
// ways, and checks that the fast name is the name the resource lookup gave.
// Prints counts and milliseconds only, and file extensions on a mismatch:
// never an identifier or a name (rule 6). Read-only.
//
//   clang -fobjc-arc -framework Foundation -framework Photos tools/photos/name_probe.m -o name_probe
//   ./name_probe walk|kvc|resources|compare [limit]
//
// The process needs Photos access: run it from a terminal app that macOS has
// granted Photos (System Settings → Privacy & Security → Photos).
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>

static double now_ms(void) { return [NSDate date].timeIntervalSince1970 * 1000.0; }

static NSString* kvc_name(PHAsset* a) {
  @try {
    id v = [a valueForKey:@"filename"];
    return [v isKindOfClass:[NSString class]] ? v : nil;
  } @catch (NSException* e) {
    (void)e;
    return nil;
  }
}

// shell/photos_items_mac.mm original_resource(), as it was before #420.
static NSString* resource_name(PHAsset* a) {
  NSArray<PHAssetResource*>* rs = [PHAssetResource assetResourcesForAsset:a];
  NSArray<NSNumber*>* order = a.mediaType == PHAssetMediaTypeVideo
      ? @[ @(PHAssetResourceTypeFullSizeVideo), @(PHAssetResourceTypeVideo) ]
      : @[ @(PHAssetResourceTypeFullSizePhoto), @(PHAssetResourceTypePhoto) ];
  for (NSNumber* t in order) {
    for (PHAssetResource* r in rs) {
      if (r.type == (PHAssetResourceType)t.integerValue) return r.originalFilename;
    }
  }
  return rs.firstObject.originalFilename;
}

int main(int argc, char** argv) {
  @autoreleasepool {
    const PHAuthorizationStatus s = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
    if (s != PHAuthorizationStatusAuthorized && s != PHAuthorizationStatusLimited) {
      fprintf(stderr, "no Photos access (status %ld)\n", (long)s);
      return 1;
    }
    NSString* mode = argc > 1 ? @(argv[1]) : @"walk";
    const long limit = argc > 2 ? atol(argv[2]) : 0;
    PHFetchOptions* o = [[PHFetchOptions alloc] init];
    o.includeHiddenAssets = NO;
    o.includeAllBurstAssets = NO;
    o.includeAssetSourceTypes = PHAssetSourceTypeUserLibrary;
    o.sortDescriptors = @[ [NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:YES] ];
    const double t0 = now_ms();
    PHFetchResult<PHAsset*>* all = [PHAsset fetchAssetsWithOptions:o];
    __block long n = 0, empty = 0, mismatch = 0;
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
        if ([mode isEqualToString:@"kvc"]) {
          if (!kvc_name(a).length) ++empty;
        } else if ([mode isEqualToString:@"resources"]) {
          if (!resource_name(a).length) ++empty;
        } else if ([mode isEqualToString:@"compare"]) {
          NSString* k = kvc_name(a);
          NSString* r = resource_name(a);
          if (!k.length) ++empty;
          if (k.length && ![k isEqualToString:r]) {
            if (++mismatch <= 10) {
              printf("mismatch subtypes=%lu kvc=.%s resource=.%s\n", (unsigned long)a.mediaSubtypes,
                     k.pathExtension.UTF8String, r.pathExtension.UTF8String);
            }
          }
        }
      }
    }];
    printf("%s: %ld assets in %.0f ms (no fast name %ld, mismatch %ld)\n", mode.UTF8String, n, now_ms() - t0,
           empty, mismatch);
  }
  return 0;
}
