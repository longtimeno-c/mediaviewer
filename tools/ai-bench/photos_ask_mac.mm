// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ai-bench --photos (issue #72): the system prompt for Photos access, which
// the pack never raises itself (the app's chrome asks from a click). The
// bench needs a bundle with NSPhotoLibraryUsageDescription for it:
// tools/ai-bench/photos-bench.sh wraps it in one.
#import <Photos/Photos.h>

bool ai_bench_ask_photos() {
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block PHAuthorizationStatus got = PHAuthorizationStatusNotDetermined;
  [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                             handler:^(PHAuthorizationStatus s) {
                                               got = s;
                                               dispatch_semaphore_signal(done);
                                             }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC));
  return got == PHAuthorizationStatusAuthorized || got == PHAuthorizationStatusLimited;
}
