// SPDX-License-Identifier: GPL-2.0-or-later
// AppKit host for the Metal present lab. PR 16: no SwiftUI, no decode.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/job_system.h"
#include "core/trace.h"
#include "shell/input_state.h"
#include "shell/present_lab_mac.h"

@interface MvMetalView : NSView
@property(nonatomic, assign) mv::shell::present_lab_mac* lab;
@property(nonatomic, assign) mv::shell::input_snapshot* snap;
@end

@implementation MvMetalView
- (BOOL)wantsLayer {
  return YES;
}
- (CALayer*)makeBackingLayer {
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
  layer.framebufferOnly = YES;
  layer.maximumDrawableCount = 1;
  layer.displaySyncEnabled = YES;
  return layer;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)isOpaque {
  return YES;
}

- (void)publish {
  if (self.lab && self.snap) self.lab->publish(*self.snap);
}

- (void)syncSize {
  const NSSize backing = [self convertSizeToBacking:self.bounds.size];
  self.snap->width = static_cast<std::uint32_t>(std::max(1.0, backing.width));
  self.snap->height = static_cast<std::uint32_t>(std::max(1.0, backing.height));
  self.snap->dpi_scale = static_cast<float>(self.window.backingScaleFactor);
  ++self.snap->resize_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  [self syncSize];
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  [self syncSize];
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  ++self.snap->display_change_seq;
  [self syncSize];
}

- (void)mouseDown:(NSEvent*)event {
  (void)event;
  self.snap->mouse_down[0] = true;
  ++self.snap->activity_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}
- (void)mouseUp:(NSEvent*)event {
  (void)event;
  self.snap->mouse_down[0] = false;
  ++self.snap->activity_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}
- (void)rightMouseDown:(NSEvent*)event {
  (void)event;
  self.snap->mouse_down[1] = true;
  ++self.snap->activity_seq;
  [self publish];
}
- (void)rightMouseUp:(NSEvent*)event {
  (void)event;
  self.snap->mouse_down[1] = false;
  [self publish];
}
- (void)scrollWheel:(NSEvent*)event {
  self.snap->wheel_total += static_cast<std::int64_t>(event.scrollingDeltaY * 120.0);
  ++self.snap->activity_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}
- (void)mouseMoved:(NSEvent*)event {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  const NSPoint backing = [self convertPointToBacking:p];
  self.snap->mouse_x = static_cast<float>(backing.x);
  self.snap->mouse_y = static_cast<float>(self.snap->height) - static_cast<float>(backing.y);
  self.snap->mouse_in_client = NSPointInRect(p, self.bounds);
  [self publish];
}
- (void)mouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
  ++self.snap->activity_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}
- (void)keyDown:(NSEvent*)event {
  const NSString* chars = event.charactersIgnoringModifiers;
  const unichar c = chars.length > 0 ? [chars characterAtIndex:0] : 0;
  if (c == NSF3FunctionKey || c == 'f' || c == 'F') {
    ++self.snap->toggle_overlay_seq;
  } else if (c == ' ') {
    ++self.snap->toggle_animation_seq;
  } else if (c == 'r' || c == 'R') {
    ++self.snap->reset_stats_seq;
  } else if (c == 0x1b) {
    [self.window close];
    return;
  }
  ++self.snap->activity_seq;
  [self publish];
  if (self.lab) self.lab->wake();
}
@end

@interface MvLabApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) MvMetalView* view;
@end

@implementation MvLabApp {
  mv::shell::present_lab_mac _lab;
  mv::shell::input_snapshot _snap;
  mv::job_system _jobs;
  mv::shell::mac_lab_options _options;
}
- (instancetype)initWithOptions:(const mv::shell::mac_lab_options&)options {
  self = [super init];
  if (self) _options = options;
  return self;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  NSRect rect = NSMakeRect(0, 0, 1280, 720);
  self.window = [[NSWindow alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"MediaViewer present lab";
  self.window.delegate = self;
  [self.window center];

  self.view = [[MvMetalView alloc] initWithFrame:self.window.contentView.bounds];
  self.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.view.lab = &_lab;
  self.view.snap = &_snap;
  self.window.contentView = self.view;

  _snap.window_visible = YES;
  _snap.window_active = YES;
  [self.view syncSize];

  if (auto started = _jobs.start(); !mv::ok(started)) {
    MV_LOG_ERROR("job_system failed to start");
    [NSApp terminate:nil];
    return;
  }
  if (auto started = _lab.start((__bridge void*)self.view, _options); !started) {
    MV_LOG_ERROR("present lab failed to start");
    [NSApp terminate:nil];
    return;
  }
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}
- (void)windowWillClose:(NSNotification*)notification {
  (void)notification;
  _lab.stop();
  _jobs.shutdown();
}
- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
  (void)notification;
  _snap.window_visible = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
  [self.view publish];
  _lab.wake();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  (void)notification;
  _snap.window_active = true;
  [self.view publish];
}
- (void)windowDidResignKey:(NSNotification*)notification {
  (void)notification;
  _snap.window_active = false;
  [self.view publish];
}
- (int)exitCode {
  return _lab.exit_code();
}
@end

namespace {

void usage() {
  std::fprintf(stderr,
               "mediaviewer_lab — Metal present lab (PR 16)\n"
               "  --soak N --json PATH [--gate] [--static] [--no-overlay]\n");
}

}  // namespace

int main(int argc, char** argv) {
  mv::trace::provider_register();
  mv::shell::mac_lab_options options;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (std::strcmp(arg, "--soak") == 0) {
      options.soak_seconds = std::strtod(next(), nullptr);
    } else if (std::strcmp(arg, "--json") == 0) {
      options.json_report_path = next();
    } else if (std::strcmp(arg, "--gate") == 0) {
      options.gate_exit_code = true;
    } else if (std::strcmp(arg, "--static") == 0) {
      options.start_animating = false;
    } else if (std::strcmp(arg, "--no-overlay") == 0) {
      options.overlay_visible = false;
    } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "mediaviewer_lab: unrecognised argument %s\n", arg);
      usage();
      return 2;
    }
  }
  if (options.soak_seconds > 0.0) {
    bool saw_static = false;
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--static") == 0) saw_static = true;
    }
    options.start_animating = !saw_static;
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    MvLabApp* app = [[MvLabApp alloc] initWithOptions:options];
    NSApp.delegate = app;
    [NSApp run];
    const int code = [app exitCode];
    mv::trace::provider_unregister();
    return code;
  }
}
