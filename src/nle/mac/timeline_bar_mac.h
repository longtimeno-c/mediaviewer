// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The timeline bar at the top of the Final Cut Pro panel (docs/design/29 "The
// panel"): drop a project from FCP's browser on it and the panel keeps that
// project's FCPXML for the timeline agent; it says which project that is and
// whether FCP changed it since; it holds the folders the editor gives; and it
// writes a new version of the project ("<Project> (Claude 1)") and hands it
// to Final Cut Pro, which imports it beside the original.
#import <AppKit/AppKit.h>

@interface MVTimelineBar : NSView <NSDraggingSource, NSPasteboardItemDataProvider>

// `group` is the app group the panel shares with the timeline agent; its
// container holds Timeline/ (nle/bridge_state.h).
- (instancetype)initWithAppGroup:(NSString*)group;

// What Final Cut Pro has open now and its playhead (seconds, < 0 unknown),
// read by the panel from FCP's host objects off the main thread.
@property(nonatomic, copy) NSString* (^activeProject)(void);
@property(nonatomic, copy) double (^playhead)(void);

// FCP said the active project, or something in it, changed.
- (void)finalCutChanged;
// FCP moved its playhead.
- (void)playheadChanged;

@end
