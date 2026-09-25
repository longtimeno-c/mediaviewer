// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Move-to-Applications and eject, for a launch straight from the DMG. The
// original on the image is never modified (rule 5): the app is copied, the copy
// is launched, and a detached shell detaches the image once this process exits
// (an image cannot be unmounted while its executable is running).
//
// After a drag install, the first-launch setup sheet offers to eject the image
// if it is still mounted and to move the .dmg to the Trash (plan/13). hdiutil
// runs on a utility queue only, bounded, never on the main thread (rule 1).
#include "shell/install_from_dmg_mac.h"

#import <AppKit/AppKit.h>
#import <DiskArbitration/DiskArbitration.h>
#include <sys/mount.h>

#include <cstring>

namespace mv::shell {
namespace {

// The .dmg a move-to-Applications relaunch came from, for the copy's setup sheet.
NSString* const kInstallerImageKey = @"MVInstallerImage";

dispatch_queue_t utility_queue() { return dispatch_get_global_queue(QOS_CLASS_UTILITY, 0); }

// Runs hdiutil with `args`, killed after `seconds`. Its exit status, or -1 when it
// could not start; stdout into `out` when given. Utility queue only.
int run_hdiutil(NSArray<NSString*>* args, double seconds, NSData** out) {
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/hdiutil"];
  task.arguments = args;
  NSPipe* pipe = out != nullptr ? [NSPipe pipe] : nil;
  task.standardOutput = pipe != nil ? (id)pipe : (id)NSFileHandle.fileHandleWithNullDevice;
  task.standardInput = task.standardError = NSFileHandle.fileHandleWithNullDevice;
  if (![task launchAndReturnError:nil]) return -1;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(seconds * NSEC_PER_SEC)),
                 utility_queue(), ^{
                   if (task.running) [task terminate];
                 });
  if (out != nullptr) *out = [pipe.fileHandleForReading readDataToEndOfFile];
  [task waitUntilExit];
  return task.terminationStatus;
}

// `hdiutil info`'s attached images, or nil. Utility queue only.
NSArray<NSDictionary*>* attached_images() {
  NSData* data = nil;
  if (run_hdiutil(@[ @"info", @"-plist" ], 10.0, &data) != 0 || data.length == 0) return nil;
  id plist = [NSPropertyListSerialization propertyListWithData:data options:0 format:nullptr error:nil];
  if (![plist isKindOfClass:NSDictionary.class]) return nil;
  id images = plist[@"images"];
  return [images isKindOfClass:NSArray.class] ? images : nil;
}

// `image` (an `hdiutil info` entry) is mounted at `mount`.
bool mounted_at(NSDictionary* image, NSString* mount) {
  id entities = image[@"system-entities"];
  if (![entities isKindOfClass:NSArray.class]) return false;
  for (NSDictionary* e in entities) {
    if (![e isKindOfClass:NSDictionary.class]) continue;
    NSString* at = e[@"mount-point"];
    if ([at isKindOfClass:NSString.class] && [at isEqualToString:mount]) return true;
  }
  return false;
}

// A top-level app on `mount` carries our bundle id: it is our installer disk.
bool holds_this_app(NSString* mount, NSString* bundle_id) {
  NSArray<NSString*>* names = [[[NSFileManager alloc] init] contentsOfDirectoryAtPath:mount error:nil];
  for (NSString* name in names) {
    if (![name.pathExtension isEqualToString:@"app"]) continue;
    NSString* plist = [[mount stringByAppendingPathComponent:name]
        stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:plist];
    if ([info[@"CFBundleIdentifier"] isEqual:bundle_id]) return true;
  }
  return false;
}

// Only a .dmg that is still there is ever offered for the Trash.
bool is_dmg_file(NSString* path) {
  if (![path isKindOfClass:NSString.class] || path.length == 0) return false;
  if ([path.pathExtension caseInsensitiveCompare:@"dmg"] != NSOrderedSame) return false;
  BOOL dir = NO;
  return [[[NSFileManager alloc] init] fileExistsAtPath:path isDirectory:&dir] && !dir;
}

// The .dmg mounted at `mount`, or nil. Utility queue only.
NSString* image_file_for_mount(NSString* mount) {
  for (NSDictionary* image in attached_images()) {
    if (![image isKindOfClass:NSDictionary.class] || !mounted_at(image, mount)) continue;
    NSString* file = image[@"image-path"];
    return is_dmg_file(file) ? file : nil;
  }
  return nil;
}

// The mount point of the volume holding `path`, when that volume is a disk image.
NSString* disk_image_mount_point(NSString* path) {
  struct statfs fs;
  if (statfs(path.fileSystemRepresentation, &fs) != 0) return nil;
  if (std::strncmp(fs.f_mntfromname, "/dev/disk", 9) != 0) return nil;
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (session == nullptr) return nil;
  NSString* result = nil;
  DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, fs.f_mntfromname + 5);
  if (disk != nullptr) {
    CFDictionaryRef desc = DADiskCopyDescription(disk);
    if (desc != nullptr) {
      NSString* model = (__bridge NSString*)CFDictionaryGetValue(desc, kDADiskDescriptionDeviceModelKey);
      if ([model isEqualToString:@"Disk Image"]) result = @(fs.f_mntonname);
      CFRelease(desc);
    }
    CFRelease(disk);
  }
  CFRelease(session);
  return result;
}

}  // namespace

bool offer_install_from_disk_image() noexcept {
  @autoreleasepool {
    NSString* bundle = NSBundle.mainBundle.bundlePath;
    if (bundle == nil || ![bundle hasPrefix:@"/Volumes/"]) return false;
    NSString* mount = disk_image_mount_point(bundle);
    if (mount == nil) return false;

    NSString* name = bundle.lastPathComponent;
    NSURL* dest = [NSURL fileURLWithPath:[@"/Applications" stringByAppendingPathComponent:name]];
    NSFileManager* fm = NSFileManager.defaultManager;
    const BOOL replacing = [fm fileExistsAtPath:dest.path];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Move MediaViewer to your Applications folder?";
    alert.informativeText =
        replacing ? @"This replaces the copy already in Applications, then ejects the disk image."
                  : @"MediaViewer will be copied to Applications and reopened from there, "
                    @"and the disk image will be ejected.";
    [alert addButtonWithTitle:@"Move to Applications"];
    [alert addButtonWithTitle:@"Not Now"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([alert runModal] != NSAlertFirstButtonReturn) return false;

    // Copy off the main thread; keep servicing the run loop meanwhile.
    __block NSError* error = nil;
    __block BOOL done = NO;
    NSURL* src = [NSURL fileURLWithPath:bundle];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSFileManager* bg = [[NSFileManager alloc] init];
      NSError* e = nil;
      if (replacing) [bg trashItemAtURL:dest resultingItemURL:nil error:&e];
      if (e == nil) [bg copyItemAtURL:src toURL:dest error:&e];
      // The copy's setup sheet offers this .dmg for the Trash once the image
      // below has been detached.
      if (e == nil) {
        if (NSString* file = image_file_for_mount(mount)) {
          [NSUserDefaults.standardUserDefaults setObject:file forKey:kInstallerImageKey];
        }
      }
      error = e;
      done = YES;
    });
    while (!done) {
      [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    if (error != nil) {
      NSAlert* fail = [[NSAlert alloc] init];
      fail.messageText = @"MediaViewer could not be copied to Applications.";
      fail.informativeText = error.localizedDescription;
      [fail runModal];
      return false;  // run from the image this once
    }

    // Detach once we are gone; retry briefly while the volume is still busy.
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
    task.arguments = @[
      @"-c", @"for i in 1 2 3 4 5 6 7 8; do sleep 1; /usr/bin/hdiutil detach \"$1\" -quiet && exit 0; done",
      @"sh", mount
    ];
    task.standardInput = task.standardOutput = task.standardError = NSFileHandle.fileHandleWithNullDevice;
    NSError* spawn = nil;
    [task launchAndReturnError:&spawn];

    NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
    cfg.createsNewApplicationInstance = YES;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:dest configuration:cfg completionHandler:nil];
    // Let LaunchServices take the request before this process exits.
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    return true;
  }
}

void find_installer_leftover(void (^done)(installer_leftover found)) {
  NSString* bundle = NSBundle.mainBundle.bundlePath;
  NSString* bundle_id = NSBundle.mainBundle.bundleIdentifier;
  NSString* remembered = [NSUserDefaults.standardUserDefaults stringForKey:kInstallerImageKey];
  dispatch_async(utility_queue(), ^{
    installer_leftover found;
    // A copy running from an image, or translocated off one by Gatekeeper, is
    // using that disk: it is not left over, and ejecting it would pull the app
    // out from under itself.
    const bool on_image = bundle == nil || [bundle containsString:@"/AppTranslocation/"] ||
                          disk_image_mount_point(bundle) != nil;
    if (!on_image && bundle_id != nil) {
      for (NSDictionary* image in attached_images()) {
        if (![image isKindOfClass:NSDictionary.class]) continue;
        id entities = image[@"system-entities"];
        if (![entities isKindOfClass:NSArray.class]) continue;
        for (NSDictionary* e in entities) {
          if (![e isKindOfClass:NSDictionary.class]) continue;
          NSString* at = e[@"mount-point"];
          if (![at isKindOfClass:NSString.class] || !holds_this_app(at, bundle_id)) continue;
          found.mount = at.fileSystemRepresentation;
          NSString* file = image[@"image-path"];
          if (is_dmg_file(file)) found.image = file.fileSystemRepresentation;
          break;
        }
        if (!found.mount.empty()) break;
      }
      // Already ejected (the move-to-Applications path detaches it): the .dmg
      // that relaunch remembered, if it is still there.
      if (found.mount.empty() && is_dmg_file(remembered)) found.image = remembered.fileSystemRepresentation;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      done(found);
    });
  });
}

void clean_up_installer(installer_leftover what, void (^done)(bool ok)) {
  forget_installer_leftover();
  NSString* mount = what.mount.empty() ? nil : @(what.mount.c_str());
  NSString* image = what.image.empty() ? nil : @(what.image.c_str());
  dispatch_async(utility_queue(), ^{
    bool ok = true;
    if (mount != nil) {
      // Finder can hold the volume for a moment after the drag; retry briefly.
      ok = false;
      for (int i = 0; i < 5 && !ok; ++i) {
        if (i > 0) [NSThread sleepForTimeInterval:1.0];
        ok = run_hdiutil(@[ @"detach", mount, @"-quiet" ], 15.0, nullptr) == 0;
      }
    }
    // The Trash, not a delete: the user can put it back.
    if (ok && image != nil && is_dmg_file(image)) {
      ok = [[[NSFileManager alloc] init] trashItemAtURL:[NSURL fileURLWithPath:image]
                                       resultingItemURL:nil
                                                  error:nil];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      done(ok);
    });
  });
}

void forget_installer_leftover() {
  [NSUserDefaults.standardUserDefaults removeObjectForKey:kInstallerImageKey];
}

}  // namespace mv::shell
