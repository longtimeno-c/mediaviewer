// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "MediaViewer for Final Cut Pro.app" (plan/23): the container the workflow
// extension ships in, installed as its own add-on beside MediaViewer rather
// than inside it (plan/18 "absent means absent": the app's bundle is
// unchanged, and FCP lists no MediaViewer extension for anyone who has not
// installed this). Opening it once:
//   - registers the search agent with launchd (SMAppService.agent), so FCP's
//     panel can reach Local search; macOS may ask to allow it in the
//     background (Login Items), which this window says and links to;
//   - lets LaunchServices register the extension in Contents/PlugIns, which
//     is where FCP finds it (Extensions button in the browser).
// It keeps no state and runs nothing when closed.
#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#ifndef MV_FCP_AGENT_PLIST
#define MV_FCP_AGENT_PLIST "dev.mediaviewer.fcp.search.plist"
#endif

@interface MVContainer : NSObject <NSApplicationDelegate>
@end

@implementation MVContainer {
  NSWindow* _window;
  NSTextField* _status;
  NSButton* _settings;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 200)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"MediaViewer for Final Cut Pro";
  NSTextField* title = [NSTextField labelWithString:@"Search your footage from inside Final Cut Pro."];
  title.font = [NSFont boldSystemFontOfSize:14];
  NSTextField* how = [NSTextField
      wrappingLabelWithString:@"In Final Cut Pro, click the Extensions button in the browser and choose "
                              @"MediaViewer Search. Results come from MediaViewer's Local search index; "
                              @"nothing is re-indexed and nothing leaves this Mac."];
  _status = [NSTextField wrappingLabelWithString:@""];
  _settings = [NSButton buttonWithTitle:@"Open Login Items Settings…" target:self action:@selector(openSettings:)];
  _settings.hidden = YES;
  NSStackView* stack = [NSStackView stackViewWithViews:@[ title, how, _status, _settings ]];
  stack.orientation = NSUserInterfaceLayoutOrientationVertical;
  stack.alignment = NSLayoutAttributeLeading;
  stack.spacing = 10;
  stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
  _window.contentView = stack;
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [self registerAgent];
}

- (void)registerAgent {
  SMAppService* agent = [SMAppService agentServiceWithPlistName:@MV_FCP_AGENT_PLIST];
  NSError* error = nil;
  if (agent.status != SMAppServiceStatusEnabled && ![agent registerAndReturnError:&error]) {
    // requiresApproval comes back as an error the first time on some systems.
  }
  switch (agent.status) {
    case SMAppServiceStatusEnabled:
      _status.stringValue = @"Ready. Local search is available to Final Cut Pro.";
      _settings.hidden = YES;
      break;
    case SMAppServiceStatusRequiresApproval:
      _status.stringValue = @"Allow “MediaViewer for Final Cut Pro” in System Settings > General > Login Items "
                            @"so Final Cut Pro can reach Local search.";
      _settings.hidden = NO;
      break;
    default:
      _status.stringValue = @"The search service could not be registered. Move this app to Applications and "
                            @"open it again.";
      _settings.hidden = YES;
      break;
  }
}

- (void)openSettings:(id)sender {
  (void)sender;
  [SMAppService openSystemSettingsLoginItems];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  (void)app;
  return YES;
}

@end

int main(int argc, const char** argv) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    MVContainer* delegate = [[MVContainer alloc] init];
    app.delegate = delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app run];
  }
  return 0;
}
