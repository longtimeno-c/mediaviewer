// SPDX-License-Identifier: GPL-2.0-or-later
// Move-to-Applications and eject, for a launch straight from the DMG. The
// original on the image is never modified (rule 5): the app is copied, the copy
// is launched, and a detached shell detaches the image once this process exits
// (an image cannot be unmounted while its executable is running).
#include "shell/install_from_dmg_mac.h"

#import <AppKit/AppKit.h>
#import <DiskArbitration/DiskArbitration.h>
#include <sys/mount.h>

#include <cstring>

namespace mv::shell {
namespace {

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

}  // namespace mv::shell
