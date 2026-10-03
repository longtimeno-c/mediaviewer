// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "MediaViewer Search": the Final Cut Pro workflow extension (plan/23). FCP
// shows this view controller in a floating window from its Extensions button.
// Searches and tiles come from the search agent over XPC (agent_protocol.h);
// the panel plays and scrubs the files itself, read-only (Extension.entitlements).
//
//   +-----------------------------------------------------------+
//   | [search field                                  ] [gear]   |
//   | [All|Video|Photos]  [Scope: Library folder "Shoot" v]     |
//   | Anna  Ben                               (people, as typed) |
//   | +-------------------------------------------------------+ |
//   | |  preview: the selected clip at its match, with sound   | |
//   | +-------------------------------------------------------+ |
//   | [tile][tile][tile][tile]   hover scrubs, drag to FCP      |
//   | 42 results in "Shoot"                                      |
//   +-----------------------------------------------------------+
//
// Scope follows the open Final Cut Pro library: by default the folder the
// library sits in and everything below it, read through FCP's host objects
// (ProExtensionHost's FCPXHostSingleton: timeline > activeSequence >
// container ... > FCPXLibrary.url). The picker switches to any indexed
// folder or the whole index; the choice is remembered per library.
//
// Keys: Cmd+F search, arrows move, Space plays / pauses, Return plays from the
// match, N / Shift+N next / previous match in the clip, Cmd+Shift+F find
// similar, Tab accepts the first name suggestion, Esc clears.
//
// Phase 0 (plan/23): FCP needs ProExtension.framework's classes, so main()
// below loads it from the installed Final Cut Pro before the extension starts;
// we ship nothing of Apple's. Our view controller is named by
// NSExtension > ProExtensionPrincipalViewControllerClass.
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <os/log.h>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "nle/fcpxml.h"
#include "nle/search_wire.h"
#import "nle/mac/agent_protocol.h"

namespace {

// FCP's FCPXML pasteboard types: the plain one and the versioned one for the
// document's version (Apple, "Content and metadata exchanges").
NSString* const kFCPXMLType = @"com.apple.finalcutpro.xml";
NSString* const kFCPXMLTypeVersioned = @"com.apple.finalcutpro.xml.v1-10";

// MV_AI_SCOPE_* and MV_AI_KIND_* (mediaviewer_ai.h), as the wire carries them.
constexpr std::uint32_t kScopeTree = 1;
constexpr std::uint32_t kScopeAll = 2;
constexpr std::uint32_t kKindPhotos = 1;
constexpr std::uint32_t kKindVideos = 2;
constexpr std::uint32_t kKindAll = 3;

// Clip handles for the drag: before / after the match, in ms. The last one
// takes the whole clip (range_around clamps to it).
struct handles {
  NSString* title;
  std::int64_t before_ms;
  std::int64_t after_ms;
};
const handles kHandles[] = {
    {@"1 s either side", 1000, 1000},
    {@"2 s before, 3 s after", 2000, 3000},
    {@"5 s either side", 5000, 5000},
    {@"10 s either side", 10000, 10000},
    {@"The whole clip", std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()},
};
constexpr int kDefaultHandles = 1;

os_log_t panel_log() {
  static os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.fcp", "extension");
  return log;
}

std::string home_dir() {
  // The sandbox's NSHomeDirectory is the container; the spike's files are the user's.
  const passwd* pw = getpwuid(getuid());
  return pw && pw->pw_dir ? pw->pw_dir : "";
}

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSString* mmss(std::int64_t ms) {
  if (ms < 0) return @"";
  const long long s = ms / 1000;
  if (s >= 3600) return [NSString stringWithFormat:@"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60];
  return [NSString stringWithFormat:@"%lld:%02lld", s / 60, s % 60];
}

bool is_video(const mv::nle::row& r) { return r.kind == kKindVideos; }

// A folder and its descendants overlap another folder: one contains the other.
bool overlaps(NSString* a, NSString* b) {
  if (a.length == 0 || b.length == 0) return false;
  NSString* sa = [a hasSuffix:@"/"] ? a : [a stringByAppendingString:@"/"];
  NSString* sb = [b hasSuffix:@"/"] ? b : [b stringByAppendingString:@"/"];
  return [sa hasPrefix:sb] || [sb hasPrefix:sa];
}

// ---- Final Cut Pro's host objects (ProExtensionHost.framework) ---------------------------

// Everything here is an Apple Event to FCP underneath: call it off the main thread.
id object_value(id object, NSString* key) {
  if (!object) return nil;
  SEL sel = NSSelectorFromString(key);
  if (![object respondsToSelector:sel]) return nil;
  @try {
    return [object valueForKey:key];
  } @catch (NSException*) {
    return nil;
  }
}

id fcp_host() {
  using singleton_fn = id (*)();
  static singleton_fn fn = reinterpret_cast<singleton_fn>(dlsym(RTLD_DEFAULT, "FCPXHostSingleton"));
  return fn ? fn() : nil;
}

// The open library's file URL: the active sequence's containers, up to the
// library. nil when FCP has no project open or answers nothing.
NSURL* active_library_url() {
  id sequence = object_value(object_value(fcp_host(), @"timeline"), @"activeSequence");
  id object = sequence;
  for (int depth = 0; object && depth < 8; ++depth) {
    id url = object_value(object, @"url");
    if ([url isKindOfClass:[NSURL class]]) return url;
    object = object_value(object, @"container");
  }
  return nil;
}

}  // namespace

// ---- the timeline observer --------------------------------------------------------------

// FCP calls these (FCPXTimelineObserver) when the open project changes.
@interface MVTimelineObserver : NSObject
@property(nonatomic, copy) void (^changed)(void);
@end

@implementation MVTimelineObserver
- (void)activeSequenceChanged {
  if (self.changed) dispatch_async(dispatch_get_main_queue(), self.changed);
}
- (void)sequenceChanged {
  [self activeSequenceChanged];
}
@end

// ---- a result tile ------------------------------------------------------------------------

@class MVTileView;

@protocol MVTileScrubbing <NSObject>
- (void)tile:(MVTileView*)tile scrubbedTo:(double)fraction;  // 0...1, or < 0 when the mouse leaves
@end

@interface MVTileView : NSView
@property(nonatomic, strong) NSImageView* image;
@property(nonatomic, strong) NSTextField* name;
@property(nonatomic, strong) NSTextField* badge;
@property(nonatomic, strong) NSView* scrubLine;
@property(nonatomic, weak) id<MVTileScrubbing> scrubber;
@property(nonatomic) BOOL selected;
@property(nonatomic) NSInteger index;
@end

@implementation MVTileView {
  NSTrackingArea* _tracking;
  NSLayoutConstraint* _scrubLeading;
}

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.layer.cornerRadius = 6;
    _image = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _image.imageScaling = NSImageScaleProportionallyUpOrDown;
    _image.wantsLayer = YES;
    _image.layer.backgroundColor = NSColor.blackColor.CGColor;
    _image.layer.cornerRadius = 4;
    _image.layer.masksToBounds = YES;
    _name = [NSTextField labelWithString:@""];
    _name.font = [NSFont systemFontOfSize:11];
    _name.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _badge = [NSTextField labelWithString:@""];
    _badge.font = [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightMedium];
    _badge.textColor = NSColor.whiteColor;
    _badge.wantsLayer = YES;
    _badge.drawsBackground = YES;
    _badge.backgroundColor = [NSColor colorWithWhite:0 alpha:0.6];
    _scrubLine = [[NSView alloc] initWithFrame:NSZeroRect];
    _scrubLine.wantsLayer = YES;
    _scrubLine.layer.backgroundColor = NSColor.systemYellowColor.CGColor;
    _scrubLine.hidden = YES;
    for (NSView* v in @[ _image, _name, _badge, _scrubLine ]) {
      v.translatesAutoresizingMaskIntoConstraints = NO;
      [self addSubview:v];
    }
    _scrubLeading = [_scrubLine.leadingAnchor constraintEqualToAnchor:_image.leadingAnchor];
    [NSLayoutConstraint activateConstraints:@[
      [_image.topAnchor constraintEqualToAnchor:self.topAnchor constant:4],
      [_image.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:4],
      [_image.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-4],
      [_image.heightAnchor constraintEqualToAnchor:_image.widthAnchor multiplier:9.0 / 16.0],
      [_name.topAnchor constraintEqualToAnchor:_image.bottomAnchor constant:3],
      [_name.leadingAnchor constraintEqualToAnchor:_image.leadingAnchor],
      [_name.trailingAnchor constraintEqualToAnchor:_image.trailingAnchor],
      [_badge.trailingAnchor constraintEqualToAnchor:_image.trailingAnchor constant:-4],
      [_badge.bottomAnchor constraintEqualToAnchor:_image.bottomAnchor constant:-4],
      [_scrubLine.bottomAnchor constraintEqualToAnchor:_image.bottomAnchor],
      _scrubLeading,
      [_scrubLine.heightAnchor constraintEqualToConstant:2],
      [_scrubLine.widthAnchor constraintEqualToConstant:2],
    ]];
  }
  return self;
}

- (void)setSelected:(BOOL)selected {
  _selected = selected;
  self.layer.backgroundColor = selected ? NSColor.selectedContentBackgroundColor.CGColor : nil;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp
             owner:self
          userInfo:nil];
  [self addTrackingArea:_tracking];
}

- (void)mouseMoved:(NSEvent*)event {
  const NSPoint p = [_image convertPoint:event.locationInWindow fromView:nil];
  const double width = std::max<double>(1, _image.bounds.size.width);
  const double f = std::clamp(p.x / width, 0.0, 1.0);
  _scrubLine.hidden = NO;
  _scrubLeading.constant = f * width;
  [self.scrubber tile:self scrubbedTo:f];
}

- (void)mouseExited:(NSEvent*)event {
  (void)event;
  _scrubLine.hidden = YES;
  [self.scrubber tile:self scrubbedTo:-1];
}

@end

@interface MVTileItem : NSCollectionViewItem
@property(nonatomic, strong) MVTileView* tile;
@end

@implementation MVTileItem
- (void)loadView {
  _tile = [[MVTileView alloc] initWithFrame:NSMakeRect(0, 0, 176, 124)];
  self.view = _tile;
}
- (void)setSelected:(BOOL)selected {
  [super setSelected:selected];
  _tile.selected = selected;
}
@end

// ---- the grid: keys the panel handles before the collection view ------------------------

@protocol MVGridKeys <NSObject>
- (BOOL)gridHandledKey:(NSEvent*)event;
- (NSMenu*)gridMenuForIndex:(NSInteger)index;
- (void)gridDoubleClicked;
@end

@interface MVGrid : NSCollectionView
@property(nonatomic, weak) id<MVGridKeys> keys;
@end

@implementation MVGrid
- (void)keyDown:(NSEvent*)event {
  if ([self.keys gridHandledKey:event]) return;
  [super keyDown:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  NSIndexPath* path = [self indexPathForItemAtPoint:p];
  if (!path) return nil;
  if (![self.selectionIndexPaths containsObject:path]) {
    self.selectionIndexPaths = [NSSet setWithObject:path];
    [self.delegate collectionView:self didSelectItemsAtIndexPaths:self.selectionIndexPaths];
  }
  return [self.keys gridMenuForIndex:path.item];
}
- (void)mouseDown:(NSEvent*)event {
  [super mouseDown:event];
  if (event.clickCount == 2) [self.keys gridDoubleClicked];
}
@end

// ---- the panel ---------------------------------------------------------------------------

@interface MVSearchViewController
    : NSViewController <NSCollectionViewDataSource, NSCollectionViewDelegate, NSSearchFieldDelegate,
                        NSPasteboardItemDataProvider, MVTileScrubbing, MVGridKeys, NSMenuDelegate>
@end

@implementation MVSearchViewController {
  NSSearchField* _field;
  NSSegmentedControl* _kinds;
  NSPopUpButton* _scope;
  NSPopUpButton* _options;
  NSStackView* _people;
  AVPlayerView* _player;
  NSImageView* _still;
  NSTextField* _caption;
  MVGrid* _grid;
  NSTextField* _status;
  NSXPCConnection* _agent;

  std::vector<mv::nle::row> _rows;
  std::vector<mv::nle::row> _dragging;
  std::string _query;             // the query the rows answer
  std::uint64_t _generation;      // the newest search; older replies are dropped
  std::uint64_t _suggestGeneration;
  NSCache<NSString*, NSImage*>* _tiles;
  NSTimer* _debounce;
  NSInteger _previewIndex;        // the row in the preview, -1 none
  std::size_t _momentCursor;      // N / Shift+N within the preview's clip
  AVAssetImageGenerator* _scrubGenerator;
  NSInteger _scrubIndex;
  NSArray<NSDictionary*>* _suggestions;

  // Scope: the open library (FCP), the indexed folders (agent), the choice.
  NSURL* _library;                // nil: FCP has no project open, or would not say
  NSArray<NSDictionary*>* _roots; // roots_json
  NSString* _scopeChoice;         // "library" | "all" | "root:<path>"
  MVTimelineObserver* _observer;
  int _handles;
  BOOL _keyword;
}

// ---- layout -------------------------------------------------------------------------------

- (void)loadView {
  NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 720)];
  root.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];  // FCP's look

  _field = [[NSSearchField alloc] initWithFrame:NSZeroRect];
  _field.placeholderString = @"Describe what you are looking for: “dog on a beach”, @Anna, -night";
  _field.delegate = self;
  _field.sendsWholeSearchString = YES;
  _field.target = self;
  _field.action = @selector(searchNow:);

  _options = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
  _options.bezelStyle = NSBezelStyleTexturedRounded;
  _options.menu.delegate = self;

  _kinds = [NSSegmentedControl segmentedControlWithLabels:@[ @"All", @"Video", @"Photos" ]
                                             trackingMode:NSSegmentSwitchTrackingSelectOne
                                                   target:self
                                                   action:@selector(optionsChanged:)];
  _kinds.selectedSegment = 0;
  _scope = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  _scope.target = self;
  _scope.action = @selector(scopeChosen:);
  _scope.toolTip = @"Where to search. By default: the folder the open Final Cut Pro library is in, and below.";

  _people = [NSStackView stackViewWithViews:@[]];
  _people.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  _people.spacing = 6;

  _player = [[AVPlayerView alloc] initWithFrame:NSZeroRect];
  _player.controlsStyle = AVPlayerViewControlsStyleInline;
  _player.showsFullScreenToggleButton = NO;
  _player.wantsLayer = YES;
  _player.layer.backgroundColor = NSColor.blackColor.CGColor;
  _still = [[NSImageView alloc] initWithFrame:NSZeroRect];
  _still.imageScaling = NSImageScaleProportionallyUpOrDown;
  _still.hidden = YES;
  _caption = [NSTextField labelWithString:@""];
  _caption.font = [NSFont systemFontOfSize:11];
  _caption.textColor = NSColor.secondaryLabelColor;
  _caption.lineBreakMode = NSLineBreakByTruncatingMiddle;

  NSCollectionViewFlowLayout* layout = [[NSCollectionViewFlowLayout alloc] init];
  layout.itemSize = NSMakeSize(168, 122);
  layout.minimumInteritemSpacing = 6;
  layout.minimumLineSpacing = 6;
  layout.sectionInset = NSEdgeInsetsMake(6, 8, 6, 8);
  _grid = [[MVGrid alloc] initWithFrame:NSZeroRect];
  _grid.collectionViewLayout = layout;
  _grid.selectable = YES;
  _grid.allowsMultipleSelection = YES;
  _grid.backgroundColors = @[ NSColor.clearColor ];
  _grid.dataSource = self;
  _grid.delegate = self;
  _grid.keys = self;
  [_grid registerClass:[MVTileItem class] forItemWithIdentifier:@"tile"];
  [_grid setDraggingSourceOperationMask:NSDragOperationCopy forLocal:NO];
  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  scroll.documentView = _grid;
  scroll.hasVerticalScroller = YES;
  scroll.drawsBackground = NO;

  _status = [NSTextField labelWithString:@""];
  _status.font = [NSFont systemFontOfSize:11];
  _status.textColor = NSColor.secondaryLabelColor;
  _status.lineBreakMode = NSLineBreakByTruncatingTail;

  for (NSView* v in @[ _field, _options, _kinds, _scope, _people, _player, _still, _caption, scroll, _status ]) {
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:v];
  }
  const CGFloat m = 10;
  [NSLayoutConstraint activateConstraints:@[
    [_field.topAnchor constraintEqualToAnchor:root.topAnchor constant:m],
    [_field.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:m],
    [_options.centerYAnchor constraintEqualToAnchor:_field.centerYAnchor],
    [_options.leadingAnchor constraintEqualToAnchor:_field.trailingAnchor constant:6],
    [_options.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-m],
    [_options.widthAnchor constraintEqualToConstant:44],
    [_kinds.topAnchor constraintEqualToAnchor:_field.bottomAnchor constant:8],
    [_kinds.leadingAnchor constraintEqualToAnchor:_field.leadingAnchor],
    [_scope.centerYAnchor constraintEqualToAnchor:_kinds.centerYAnchor],
    [_scope.leadingAnchor constraintEqualToAnchor:_kinds.trailingAnchor constant:8],
    [_scope.trailingAnchor constraintLessThanOrEqualToAnchor:root.trailingAnchor constant:-m],
    [_people.topAnchor constraintEqualToAnchor:_kinds.bottomAnchor constant:6],
    [_people.leadingAnchor constraintEqualToAnchor:_field.leadingAnchor],
    [_people.trailingAnchor constraintLessThanOrEqualToAnchor:root.trailingAnchor constant:-m],
    [_people.heightAnchor constraintEqualToConstant:22],
    [_player.topAnchor constraintEqualToAnchor:_people.bottomAnchor constant:6],
    [_player.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:m],
    [_player.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-m],
    [_player.heightAnchor constraintEqualToAnchor:_player.widthAnchor multiplier:9.0 / 16.0],
    [_still.topAnchor constraintEqualToAnchor:_player.topAnchor],
    [_still.bottomAnchor constraintEqualToAnchor:_player.bottomAnchor],
    [_still.leadingAnchor constraintEqualToAnchor:_player.leadingAnchor],
    [_still.trailingAnchor constraintEqualToAnchor:_player.trailingAnchor],
    [_caption.topAnchor constraintEqualToAnchor:_player.bottomAnchor constant:4],
    [_caption.leadingAnchor constraintEqualToAnchor:_player.leadingAnchor],
    [_caption.trailingAnchor constraintEqualToAnchor:_player.trailingAnchor],
    [scroll.topAnchor constraintEqualToAnchor:_caption.bottomAnchor constant:4],
    [scroll.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:140],
    [_status.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:4],
    [_status.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:m],
    [_status.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-m],
    [_status.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8],
  ]];
  self.view = root;

  _tiles = [[NSCache alloc] init];
  _tiles.countLimit = 400;
  _generation = 0;
  _suggestGeneration = 0;
  _previewIndex = -1;
  _scrubIndex = -1;
  _scopeChoice = @"library";
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  _handles = [d objectForKey:@"handles"] ? static_cast<int>([d integerForKey:@"handles"]) : kDefaultHandles;
  _handles = std::clamp(_handles, 0, static_cast<int>(std::size(kHandles)) - 1);
  _keyword = [d objectForKey:@"keyword"] ? [d boolForKey:@"keyword"] : YES;
  [self rebuildOptionsMenu];
  [self rebuildScopeMenu];
  _status.stringValue = @"Type to search your indexed footage. Drag results into an event or the timeline.";
}

- (void)viewDidAppear {
  [super viewDidAppear];
  [self.view.window makeFirstResponder:_field];
  [self refreshRoots];
  [self refreshLibrary];
  if (!_observer) {
    _observer = [[MVTimelineObserver alloc] init];
    __weak MVSearchViewController* weak = self;
    _observer.changed = ^{
      [weak refreshLibrary];
    };
    MVTimelineObserver* observer = _observer;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      id timeline = object_value(fcp_host(), @"timeline");
      SEL add = NSSelectorFromString(@"addTimelineObserver:");
      if ([timeline respondsToSelector:add]) {
        @try {
          ((void (*)(id, SEL, id))objc_msgSend)(timeline, add, observer);
        } @catch (NSException*) {
        }
      }
    });
  }
}

- (void)viewWillDisappear {
  [super viewWillDisappear];
  [_player.player pause];
}

// ---- the agent ----------------------------------------------------------------------------

- (NSXPCConnection*)agent {
  if (!_agent) {
    _agent = [[NSXPCConnection alloc] initWithMachServiceName:@MV_FCP_MACH_SERVICE options:0];
    _agent.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MVSearchAgent)];
    __weak MVSearchViewController* weak = self;
    _agent.invalidationHandler = ^{
      dispatch_async(dispatch_get_main_queue(), ^{
        MVSearchViewController* me = weak;
        if (me) me->_agent = nil;  // launchd starts it again on the next lookup
      });
    };
    [_agent resume];
  }
  return _agent;
}

- (id<MVSearchAgent>)proxyReporting:(std::uint64_t)generation {
  __weak MVSearchViewController* weak = self;
  return [[self agent] remoteObjectProxyWithErrorHandler:^(NSError* e) {
    (void)e;
    dispatch_async(dispatch_get_main_queue(), ^{
      MVSearchViewController* me = weak;
      if (me && (generation == 0 || me->_generation == generation)) {
        me->_status.stringValue = @"Local search is not available. In MediaViewer, open Settings > Local search and "
                                  @"turn on Final Cut Pro.";
      }
    });
  }];
}

// ---- scope ----------------------------------------------------------------------------

- (void)refreshRoots {
  __weak MVSearchViewController* weak = self;
  [[self proxyReporting:0] rootsWithReply:^(NSString* json) {
    NSArray* parsed = nil;
    if (json) {
      id obj = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
      if ([obj isKindOfClass:[NSArray class]]) parsed = obj;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      MVSearchViewController* me = weak;
      if (!me) return;
      me->_roots = parsed ?: @[];
      [me rebuildScopeMenu];
    });
  }];
}

- (void)refreshLibrary {
  __weak MVSearchViewController* weak = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSURL* library = active_library_url();
    dispatch_async(dispatch_get_main_queue(), ^{
      MVSearchViewController* me = weak;
      if (!me) return;
      if ((library == nil && me->_library == nil) || [library isEqual:me->_library]) return;
      me->_library = library;
      os_log(panel_log(), "library %{public}s", library ? "known" : "unknown");
      // The remembered choice for this library, or its folder.
      NSString* saved = library ? [NSUserDefaults.standardUserDefaults
                                      stringForKey:[@"scope:" stringByAppendingString:library.path]]
                                : nil;
      me->_scopeChoice = saved ?: @"library";
      [me rebuildScopeMenu];
      if (me->_field.stringValue.length > 0) [me search];
    });
  });
}

- (NSString*)libraryFolder {
  return _library ? _library.URLByDeletingLastPathComponent.path : nil;
}

- (void)rebuildScopeMenu {
  NSMenu* menu = [[NSMenu alloc] init];
  NSString* folder = [self libraryFolder];
  NSMenuItem* lib = [[NSMenuItem alloc]
      initWithTitle:folder ? [NSString stringWithFormat:@"Library folder: %@", folder.lastPathComponent]
                           : @"Library folder (open a project in Final Cut Pro)"
             action:nil
      keyEquivalent:@""];
  lib.representedObject = @"library";
  lib.toolTip = folder;
  lib.enabled = folder != nil;
  [menu addItem:lib];
  if (_roots.count > 0) {
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* head = [[NSMenuItem alloc] initWithTitle:@"Indexed folders" action:nil keyEquivalent:@""];
    head.enabled = NO;
    [menu addItem:head];
    for (NSDictionary* r in _roots) {
      NSString* path = [r[@"path"] isKindOfClass:[NSString class]] ? r[@"path"] : nil;
      if (!path || [path hasPrefix:@"photos:"]) continue;  // the Photos library is not a folder FCP can import from
      NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:path.lastPathComponent action:nil keyEquivalent:@""];
      it.representedObject = [@"root:" stringByAppendingString:path];
      it.toolTip = path;
      it.indentationLevel = 1;
      [menu addItem:it];
    }
  }
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* all = [[NSMenuItem alloc] initWithTitle:@"All indexed folders" action:nil keyEquivalent:@""];
  all.representedObject = @"all";
  [menu addItem:all];
  _scope.menu = menu;
  menu.autoenablesItems = NO;
  NSString* want = _scopeChoice;
  if ([want isEqualToString:@"library"] && !folder) want = @"all";  // until FCP says which library
  for (NSMenuItem* it in menu.itemArray) {
    if ([it.representedObject isEqual:want]) [_scope selectItem:it];
  }
}

- (void)scopeChosen:(id)sender {
  (void)sender;
  NSString* choice = _scope.selectedItem.representedObject;
  if (!choice) return;
  _scopeChoice = choice;
  if (_library) {
    [NSUserDefaults.standardUserDefaults setObject:choice
                                            forKey:[@"scope:" stringByAppendingString:_library.path]];
  }
  if (_field.stringValue.length > 0) [self search];
}

// The request's scope and folder, from the choice.
- (std::uint32_t)scopeFor:(NSString**)dir {
  NSString* choice = _scope.selectedItem.representedObject ?: @"all";
  if ([choice isEqualToString:@"library"] && [self libraryFolder]) {
    *dir = [self libraryFolder];
    return kScopeTree;
  }
  if ([choice hasPrefix:@"root:"]) {
    *dir = [choice substringFromIndex:5];
    return kScopeTree;
  }
  *dir = @"";
  return kScopeAll;
}

// ---- options ------------------------------------------------------------------------------

- (void)rebuildOptionsMenu {
  NSMenu* menu = [[NSMenu alloc] init];
  menu.autoenablesItems = NO;
  NSMenuItem* icon = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
  icon.image = [NSImage imageWithSystemSymbolName:@"slider.horizontal.3" accessibilityDescription:@"Options"];
  [menu addItem:icon];  // a pull-down shows its first item as the button
  NSMenuItem* head = [[NSMenuItem alloc] initWithTitle:@"When dragged into Final Cut Pro" action:nil keyEquivalent:@""];
  head.enabled = NO;
  [menu addItem:head];
  for (int i = 0; i < static_cast<int>(std::size(kHandles)); ++i) {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:kHandles[i].title action:@selector(handlesChosen:) keyEquivalent:@""];
    it.target = self;
    it.tag = i;
    it.indentationLevel = 1;
    it.state = i == _handles ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:it];
  }
  NSMenuItem* kw = [[NSMenuItem alloc] initWithTitle:@"Add the search as a keyword collection"
                                              action:@selector(keywordToggled:)
                                       keyEquivalent:@""];
  kw.target = self;
  kw.state = _keyword ? NSControlStateValueOn : NSControlStateValueOff;
  [menu addItem:kw];
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* rescan = [[NSMenuItem alloc] initWithTitle:@"Refresh folders" action:@selector(refreshScope:) keyEquivalent:@""];
  rescan.target = self;
  [menu addItem:rescan];
  NSMenuItem* test = [[NSMenuItem alloc] initWithTitle:@"Show the test drag (~/Movies/test)"
                                                action:@selector(showTestDrag:)
                                         keyEquivalent:@""];
  test.target = self;
  [menu addItem:test];
  _options.menu = menu;
}

- (void)handlesChosen:(NSMenuItem*)item {
  _handles = static_cast<int>(item.tag);
  [NSUserDefaults.standardUserDefaults setInteger:_handles forKey:@"handles"];
  [self rebuildOptionsMenu];
}

- (void)keywordToggled:(id)sender {
  (void)sender;
  _keyword = !_keyword;
  [NSUserDefaults.standardUserDefaults setBool:_keyword forKey:@"keyword"];
  [self rebuildOptionsMenu];
}

- (void)refreshScope:(id)sender {
  (void)sender;
  [self refreshRoots];
  _library = nil;
  [self refreshLibrary];
}

- (void)optionsChanged:(id)sender {
  (void)sender;
  if (_field.stringValue.length > 0) [self search];
}

// The spike's hard-coded drag (plan/23 Phase 0 verify): two clips with ranges and a photo.
- (void)showTestDrag:(id)sender {
  (void)sender;
  ++_generation;
  const std::string dir = home_dir() + "/Movies/test/";
  mv::nle::row a;
  a.path = dir + "clip1.mov";
  a.kind = kKindVideos;
  a.pts_ms = 4000;
  mv::nle::row b;
  b.path = dir + "clip2.mov";
  b.kind = kKindVideos;
  b.pts_ms = 10000;
  mv::nle::row c;
  c.path = dir + "photo.jpg";
  c.kind = kKindPhotos;
  _rows = {a, b, c};
  _query = "test";
  _status.stringValue = @"Test drag: ~/Movies/test (clip1.mov, clip2.mov, photo.jpg)";
  [self reloadResults];
}

// ---- searching --------------------------------------------------------------------------

- (void)controlTextDidChange:(NSNotification*)note {
  (void)note;
  [_debounce invalidate];
  _debounce = [NSTimer scheduledTimerWithTimeInterval:0.35
                                               target:self
                                             selector:@selector(debounced:)
                                             userInfo:nil
                                              repeats:NO];
  [self suggest];
}

- (void)debounced:(NSTimer*)t {
  (void)t;
  [self search];
}

- (void)searchNow:(id)sender {
  (void)sender;
  [_debounce invalidate];
  [self search];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command {
  (void)control;
  (void)textView;
  if (command == @selector(insertTab:) && _suggestions.count > 0) {
    [self acceptSuggestion:_suggestions.firstObject];
    return YES;
  }
  if (command == @selector(moveDown:) && !_rows.empty()) {
    [self.view.window makeFirstResponder:_grid];
    [self selectIndex:0];
    return YES;
  }
  if (command == @selector(cancelOperation:)) {
    _field.stringValue = @"";
    [self clearResults];
    return YES;
  }
  return NO;
}

- (void)clearResults {
  ++_generation;
  _rows.clear();
  _query.clear();
  [self reloadResults];
  [self showPreview:-1];
  [self showSuggestions:@[]];
  _status.stringValue = @"Type to search your indexed footage. Drag results into an event or the timeline.";
}

- (std::uint32_t)kinds {
  switch (_kinds.selectedSegment) {
    case 1: return kKindVideos;
    case 2: return kKindPhotos;
    default: return kKindAll;
  }
}

- (void)search {
  NSString* text = [_field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (text.length == 0) {
    [self clearResults];
    return;
  }
  mv::nle::request r;
  r.kind = mv::nle::request_kind::text;
  r.kinds = [self kinds];
  r.max_results = 300;
  NSString* dir = @"";
  r.scope = [self scopeFor:&dir];
  [self runRequest:r text:text dir:dir query:text.UTF8String];
}

- (void)findSimilar {
  if (_previewIndex < 0 || static_cast<std::size_t>(_previewIndex) >= _rows.size()) return;
  const mv::nle::row x = _rows[static_cast<std::size_t>(_previewIndex)];
  mv::nle::request r;
  r.kind = mv::nle::request_kind::similar;
  r.kinds = [self kinds];
  r.max_results = 300;
  r.pts_ms = is_video(x) ? [self currentPreviewMs] : -1;
  NSString* dir = @"";
  r.scope = [self scopeFor:&dir];
  NSString* name = ns(x.path).lastPathComponent;
  [self runRequest:r text:ns(x.path) dir:dir query:std::string("like ") + name.UTF8String];
}

- (void)runRequest:(mv::nle::request)r text:(NSString*)text dir:(NSString*)dir query:(std::string)query {
  const std::uint64_t generation = ++_generation;
  r.correlation_id = generation;
  const auto req = mv::nle::encode(r);
  _status.stringValue = @"Searching…";
  NSString* scopeName = dir.length ? dir.lastPathComponent : @"every indexed folder";
  NSString* scopeDir = dir;
  __weak MVSearchViewController* weak = self;
  [[self proxyReporting:generation]
           run:[NSData dataWithBytes:req.data() length:req.size()]
          text:text
      scopeDir:dir
     withReply:^(NSData* reply) {
       auto decoded = mv::nle::decode(
           std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(reply.bytes), reply.length));
       // A block copies what it captures: hand the rows over in a shared box.
       auto rep = decoded ? std::make_shared<mv::nle::reply>(std::move(*decoded)) : nullptr;
       dispatch_async(dispatch_get_main_queue(), ^{
         MVSearchViewController* me = weak;
         if (!me || me->_generation != generation) return;  // a newer query owns the list
         if (!rep) {
           me->_status.stringValue = @"Local search sent an answer this panel cannot read. Update MediaViewer.";
           return;
         }
         me->_rows = std::move(rep->rows);
         me->_query = query;
         [me reloadResults];
         if (rep->code == mv::status::unsupported_format) {
           me->_status.stringValue = @"Local search needs an update: MediaViewer > Settings > Local search.";
         } else if (rep->code == mv::status::not_found) {
           me->_status.stringValue = @"Nothing is indexed yet. Open MediaViewer to index your footage.";
         } else if (rep->code != mv::status::ok) {
           me->_status.stringValue = @"Local search is still loading. Try again in a moment.";
         } else if (me->_rows.empty()) {
           me->_status.stringValue = [me nothingFoundIn:scopeDir name:scopeName];
         } else {
           me->_status.stringValue = [NSString
               stringWithFormat:@"%zu results in %@. Drag into an event or the timeline; Space plays.",
                                me->_rows.size(), scopeName];
           [me selectIndex:0];
         }
       });
     }];
}

- (NSString*)nothingFoundIn:(NSString*)dir name:(NSString*)name {
  if (dir.length == 0) return @"Nothing found.";
  for (NSDictionary* r in _roots) {
    NSString* path = [r[@"path"] isKindOfClass:[NSString class]] ? r[@"path"] : nil;
    if (path && overlaps(path, dir)) return [NSString stringWithFormat:@"Nothing found in %@.", name];
  }
  return [NSString stringWithFormat:@"Nothing is indexed in “%@”. Index it in MediaViewer, or pick another folder.", name];
}

// ---- people, as they are typed -----------------------------------------------------------

- (void)suggest {
  const std::uint64_t generation = ++_suggestGeneration;
  NSString* text = _field.stringValue;
  if (text.length == 0) {
    [self showSuggestions:@[]];
    return;
  }
  __weak MVSearchViewController* weak = self;
  [[[self agent] remoteObjectProxy] suggest:text
                                  withReply:^(NSString* json) {
                                    NSArray* parsed = nil;
                                    if (json) {
                                      id obj = [NSJSONSerialization
                                          JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
                                                     options:0
                                                       error:nil];
                                      if ([obj isKindOfClass:[NSArray class]]) parsed = obj;
                                    }
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                      MVSearchViewController* me = weak;
                                      if (!me || me->_suggestGeneration != generation) return;
                                      [me showSuggestions:parsed ?: @[]];
                                    });
                                  }];
}

- (void)showSuggestions:(NSArray<NSDictionary*>*)list {
  _suggestions = list;
  for (NSView* v in _people.arrangedSubviews.copy) {
    [_people removeArrangedSubview:v];
    [v removeFromSuperview];
  }
  NSInteger i = 0;
  for (NSDictionary* s in list) {
    NSString* name = [s[@"name"] isKindOfClass:[NSString class]] ? s[@"name"] : nil;
    if (!name) continue;
    NSButton* b = [NSButton buttonWithTitle:name target:self action:@selector(suggestionClicked:)];
    b.bezelStyle = NSBezelStyleRecessed;
    b.controlSize = NSControlSizeSmall;
    b.image = [NSImage imageWithSystemSymbolName:@"person.crop.circle" accessibilityDescription:nil];
    b.imagePosition = NSImageLeading;
    b.tag = i++;
    if (b.tag == 0) b.toolTip = @"Tab";
    [_people addArrangedSubview:b];
  }
}

- (void)suggestionClicked:(NSButton*)b {
  if (b.tag >= 0 && b.tag < static_cast<NSInteger>(_suggestions.count)) [self acceptSuggestion:_suggestions[b.tag]];
}

- (void)acceptSuggestion:(NSDictionary*)s {
  NSString* completion = [s[@"completion"] isKindOfClass:[NSString class]] ? s[@"completion"] : nil;
  if (!completion) return;
  _field.stringValue = completion;
  [self showSuggestions:@[]];
  [self.view.window makeFirstResponder:_field];
  _field.currentEditor.selectedRange = NSMakeRange(completion.length, 0);
  [self searchNow:nil];
}

// ---- results ------------------------------------------------------------------------------

- (void)reloadResults {
  _scrubGenerator = nil;
  _scrubIndex = -1;
  [_grid reloadData];
}

- (NSInteger)collectionView:(NSCollectionView*)view numberOfItemsInSection:(NSInteger)section {
  (void)view;
  (void)section;
  return static_cast<NSInteger>(_rows.size());
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)view itemForRepresentedObjectAtIndexPath:(NSIndexPath*)path {
  MVTileItem* item = [view makeItemWithIdentifier:@"tile" forIndexPath:path];
  const std::size_t i = static_cast<std::size_t>(path.item);
  if (i >= _rows.size()) return item;
  const mv::nle::row& x = _rows[i];
  MVTileView* tile = item.tile;
  tile.index = path.item;
  tile.scrubber = self;
  tile.name.stringValue = ns(x.path).lastPathComponent;
  tile.toolTip = ns(x.path);
  NSString* badge = is_video(x) ? mmss(x.pts_ms) : @"photo";
  if (is_video(x) && x.duration_ms > 0) badge = [NSString stringWithFormat:@"%@ / %@", badge, mmss(x.duration_ms)];
  if (x.more > 0) badge = [NSString stringWithFormat:@"%@  +%u", badge, x.more];
  tile.badge.stringValue = [NSString stringWithFormat:@" %@ ", badge];
  tile.image.image = nil;
  [self loadTile:tile row:x index:path.item];
  return item;
}

- (void)loadTile:(MVTileView*)tile row:(const mv::nle::row&)x index:(NSInteger)index {
  if (x.thumb.empty()) return;
  NSString* key = ns(x.thumb);
  if (NSImage* hit = [_tiles objectForKey:key]) {
    tile.image.image = hit;
    return;
  }
  __weak MVTileView* weakTile = tile;
  NSCache* tiles = _tiles;
  [[[self agent] remoteObjectProxy] thumbnail:key
                                    withReply:^(NSData* jpeg) {
                                      NSImage* img = jpeg ? [[NSImage alloc] initWithData:jpeg] : nil;
                                      if (!img) return;
                                      dispatch_async(dispatch_get_main_queue(), ^{
                                        [tiles setObject:img forKey:key];
                                        MVTileView* t = weakTile;
                                        if (t && t.index == index) t.image.image = img;
                                      });
                                    }];
}

// Hover: frames across the clip, decoded here (read-only access to the file).
- (void)tile:(MVTileView*)tile scrubbedTo:(double)fraction {
  const NSInteger index = tile.index;
  if (index < 0 || static_cast<std::size_t>(index) >= _rows.size()) return;
  const mv::nle::row& x = _rows[static_cast<std::size_t>(index)];
  if (fraction < 0 || !is_video(x)) {
    if (fraction < 0) {
      _scrubIndex = -1;
      [_scrubGenerator cancelAllCGImageGeneration];
      NSImage* hit = x.thumb.empty() ? nil : [_tiles objectForKey:ns(x.thumb)];
      tile.image.image = hit;
    }
    return;
  }
  if (_scrubIndex != index || !_scrubGenerator) {
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns(x.path)] options:nil];
    _scrubGenerator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
    _scrubGenerator.appliesPreferredTrackTransform = YES;
    _scrubGenerator.maximumSize = CGSizeMake(320, 180);
    _scrubGenerator.requestedTimeToleranceBefore = CMTimeMake(1, 2);
    _scrubGenerator.requestedTimeToleranceAfter = CMTimeMake(1, 2);
    _scrubIndex = index;
  }
  const double clip_s = x.duration_ms > 0 ? x.duration_ms / 1000.0 : std::max(1.0, (x.pts_ms + 5000) / 1000.0);
  const CMTime at = CMTimeMakeWithSeconds(fraction * clip_s, 600);
  [_scrubGenerator cancelAllCGImageGeneration];
  __weak MVTileView* weakTile = tile;
  __weak MVSearchViewController* weak = self;
  [_scrubGenerator generateCGImagesAsynchronouslyForTimes:@[ [NSValue valueWithCMTime:at] ]
                                        completionHandler:^(CMTime requested, CGImageRef image, CMTime actual,
                                                            AVAssetImageGeneratorResult result, NSError* error) {
                                          (void)requested;
                                          (void)actual;
                                          (void)error;
                                          if (result != AVAssetImageGeneratorSucceeded || !image) return;
                                          NSImage* img = [[NSImage alloc] initWithCGImage:image size:NSZeroSize];
                                          dispatch_async(dispatch_get_main_queue(), ^{
                                            MVTileView* t = weakTile;
                                            MVSearchViewController* me = weak;
                                            if (t && me && t.index == index && me->_scrubIndex == index) {
                                              t.image.image = img;
                                            }
                                          });
                                        }];
}

// ---- selection and the preview ----------------------------------------------------------

- (void)collectionView:(NSCollectionView*)view didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)paths {
  (void)paths;
  [self previewSelection:view];
}

- (void)collectionView:(NSCollectionView*)view didDeselectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)paths {
  (void)paths;
  [self previewSelection:view];
}

- (void)previewSelection:(NSCollectionView*)view {
  NSIndexPath* last = nil;
  for (NSIndexPath* p in view.selectionIndexPaths) {
    if (!last || p.item > last.item) last = p;
  }
  [self showPreview:last ? last.item : -1];
}

- (void)selectIndex:(NSInteger)index {
  if (index < 0 || static_cast<std::size_t>(index) >= _rows.size()) return;
  NSIndexPath* p = [NSIndexPath indexPathForItem:index inSection:0];
  _grid.selectionIndexPaths = [NSSet setWithObject:p];
  [_grid scrollToItemsAtIndexPaths:[NSSet setWithObject:p] scrollPosition:NSCollectionViewScrollPositionNearestHorizontalEdge];
  [self showPreview:index];
}

- (void)showPreview:(NSInteger)index {
  if (index == _previewIndex) return;
  _previewIndex = index;
  _momentCursor = 0;
  [_player.player pause];
  if (index < 0 || static_cast<std::size_t>(index) >= _rows.size()) {
    _player.player = nil;
    _still.hidden = YES;
    _player.hidden = NO;
    _caption.stringValue = @"";
    return;
  }
  const mv::nle::row& x = _rows[static_cast<std::size_t>(index)];
  NSURL* url = [NSURL fileURLWithPath:ns(x.path)];
  if (is_video(x)) {
    _still.hidden = YES;
    _player.hidden = NO;
    AVPlayer* player = [AVPlayer playerWithURL:url];
    _player.player = player;
    [player seekToTime:CMTimeMakeWithSeconds(std::max<std::int64_t>(0, x.pts_ms) / 1000.0, 600)
        toleranceBefore:kCMTimeZero
         toleranceAfter:kCMTimeZero];
    // The moment in the clip N / Shift+N starts from.
    for (std::size_t k = 0; k < x.moments.size(); ++k) {
      if (x.moments[k].pts_ms == x.pts_ms) _momentCursor = k;
    }
  } else {
    _player.player = nil;
    _player.hidden = YES;
    _still.hidden = NO;
    _still.image = x.thumb.empty() ? nil : [_tiles objectForKey:ns(x.thumb)];
    __weak MVSearchViewController* weak = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSImage* img = [[NSImage alloc] initWithContentsOfURL:url];
      dispatch_async(dispatch_get_main_queue(), ^{
        MVSearchViewController* me = weak;
        if (img && me && me->_previewIndex == index) me->_still.image = img;
      });
    });
  }
  [self updateCaption];
}

- (void)updateCaption {
  if (_previewIndex < 0 || static_cast<std::size_t>(_previewIndex) >= _rows.size()) return;
  const mv::nle::row& x = _rows[static_cast<std::size_t>(_previewIndex)];
  NSString* name = ns(x.path).lastPathComponent;
  if (!is_video(x)) {
    _caption.stringValue = [NSString stringWithFormat:@"%@  ·  photo", name];
    return;
  }
  NSMutableString* s = [NSMutableString stringWithFormat:@"%@  ·  match at %@", name, mmss(x.pts_ms)];
  if (x.duration_ms > 0) [s appendFormat:@" of %@", mmss(x.duration_ms)];
  if (x.moments.size() > 1) [s appendFormat:@"  ·  %zu matches (N / Shift+N)", x.moments.size()];
  _caption.stringValue = s;
}

- (std::int64_t)currentPreviewMs {
  AVPlayer* p = _player.player;
  if (!p) return -1;
  return static_cast<std::int64_t>(CMTimeGetSeconds(p.currentTime) * 1000.0);
}

- (void)togglePlay {
  AVPlayer* p = _player.player;
  if (!p) return;
  if (p.rate != 0) {
    [p pause];
  } else {
    [p play];
  }
}

- (void)playFromMatch {
  if (_previewIndex < 0 || static_cast<std::size_t>(_previewIndex) >= _rows.size()) return;
  const mv::nle::row& x = _rows[static_cast<std::size_t>(_previewIndex)];
  AVPlayer* p = _player.player;
  if (!p) return;
  const handles& h = kHandles[_handles];
  const std::int64_t from = std::max<std::int64_t>(0, x.pts_ms - std::min<std::int64_t>(h.before_ms, 5000));
  [p seekToTime:CMTimeMakeWithSeconds(from / 1000.0, 600) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
  [p play];
}

- (void)stepMoment:(int)direction {
  if (_previewIndex < 0 || static_cast<std::size_t>(_previewIndex) >= _rows.size()) return;
  const mv::nle::row& x = _rows[static_cast<std::size_t>(_previewIndex)];
  if (x.moments.empty() || !_player.player) return;
  const std::size_t n = x.moments.size();
  _momentCursor = (_momentCursor + n + static_cast<std::size_t>(direction > 0 ? 1 : n - 1)) % n;
  [_player.player seekToTime:CMTimeMakeWithSeconds(x.moments[_momentCursor].pts_ms / 1000.0, 600)
             toleranceBefore:kCMTimeZero
              toleranceAfter:kCMTimeZero];
  _caption.stringValue = [NSString stringWithFormat:@"%@  ·  match %zu of %zu at %@", ns(x.path).lastPathComponent,
                                                    _momentCursor + 1, n, mmss(x.moments[_momentCursor].pts_ms)];
}

// ---- keys and the context menu --------------------------------------------------------------

- (BOOL)gridHandledKey:(NSEvent*)event {
  NSString* c = event.charactersIgnoringModifiers;
  const NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  if ([c isEqualToString:@" "]) {
    [self togglePlay];
    return YES;
  }
  if ([c isEqualToString:@"\r"]) {
    [self playFromMatch];
    return YES;
  }
  if ([c.lowercaseString isEqualToString:@"n"] && !(mods & NSEventModifierFlagCommand)) {
    [self stepMoment:(mods & NSEventModifierFlagShift) ? -1 : 1];
    return YES;
  }
  if ([c.lowercaseString isEqualToString:@"f"] && (mods & NSEventModifierFlagCommand)) {
    if (mods & NSEventModifierFlagShift) {
      [self findSimilar];
    } else {
      [self.view.window makeFirstResponder:_field];
    }
    return YES;
  }
  if (event.keyCode == 53) {  // Esc
    [self.view.window makeFirstResponder:_field];
    return YES;
  }
  return NO;
}

- (void)gridDoubleClicked {
  [self playFromMatch];
}

- (NSMenu*)gridMenuForIndex:(NSInteger)index {
  (void)index;
  NSMenu* menu = [[NSMenu alloc] init];
  [menu addItemWithTitle:@"Play from the match" action:@selector(menuPlay:) keyEquivalent:@""].target = self;
  [menu addItemWithTitle:@"Find similar" action:@selector(menuSimilar:) keyEquivalent:@""].target = self;
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItemWithTitle:@"Show in Finder" action:@selector(menuReveal:) keyEquivalent:@""].target = self;
  [menu addItemWithTitle:@"Open in MediaViewer" action:@selector(menuOpen:) keyEquivalent:@""].target = self;
  return menu;
}

- (void)menuPlay:(id)sender {
  (void)sender;
  [self playFromMatch];
}

- (void)menuSimilar:(id)sender {
  (void)sender;
  [self findSimilar];
}

- (NSArray<NSURL*>*)selectedURLs {
  NSMutableArray<NSURL*>* urls = [NSMutableArray array];
  for (NSIndexPath* p in _grid.selectionIndexPaths) {
    if (static_cast<std::size_t>(p.item) < _rows.size()) {
      [urls addObject:[NSURL fileURLWithPath:ns(_rows[static_cast<std::size_t>(p.item)].path)]];
    }
  }
  return urls;
}

- (void)menuReveal:(id)sender {
  (void)sender;
  [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:[self selectedURLs]];
}

- (void)menuOpen:(id)sender {
  (void)sender;
  NSURL* app = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@MV_FCP_APP_BUNDLE_ID];
  if (!app) return;
  [NSWorkspace.sharedWorkspace openURLs:[self selectedURLs]
                   withApplicationAtURL:app
                          configuration:[NSWorkspaceOpenConfiguration configuration]
                      completionHandler:nil];
}

// ---- the drag: FCPXML for the whole selection, a file URL per item ----------------------

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)view pasteboardWriterForItemAtIndexPath:(NSIndexPath*)path {
  const std::size_t i = static_cast<std::size_t>(path.item);
  if (i >= _rows.size()) return nil;
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  [item setString:[NSURL fileURLWithPath:ns(_rows[i].path)].absoluteString forType:NSPasteboardTypeFileURL];
  // One document for the whole drag, on its first item, made when a target asks.
  NSIndexPath* first = nil;
  for (NSIndexPath* p in view.selectionIndexPaths) {
    if (!first || p.item < first.item) first = p;
  }
  const bool isFirst = ![view.selectionIndexPaths containsObject:path] || [path isEqual:first];
  if (isFirst) [item setDataProvider:self forTypes:@[ kFCPXMLTypeVersioned, kFCPXMLType ]];
  return item;
}

- (BOOL)collectionView:(NSCollectionView*)view
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)paths
                   withEvent:(NSEvent*)event {
  (void)view;
  (void)paths;
  (void)event;
  return YES;
}

- (void)collectionView:(NSCollectionView*)view
       draggingSession:(NSDraggingSession*)session
      willBeginAtPoint:(NSPoint)point
  forItemsAtIndexPaths:(NSSet<NSIndexPath*>*)paths {
  (void)view;
  (void)session;
  (void)point;
  _dragging.clear();
  NSArray<NSIndexPath*>* sorted = [paths.allObjects sortedArrayUsingSelector:@selector(compare:)];
  for (NSIndexPath* p in sorted) {
    if (static_cast<std::size_t>(p.item) < _rows.size()) _dragging.push_back(_rows[static_cast<std::size_t>(p.item)]);
  }
}

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
  (void)pasteboard;
  mv::nle::fcpxml_options o;
  o.event_name = "MediaViewer: " + _query;
  o.keyword = _keyword ? "MV: " + _query : std::string();
  o.before_ms = kHandles[_handles].before_ms;
  o.after_ms = kHandles[_handles].after_ms;
  const std::string doc = mv::nle::fcpxml(_dragging, o);
  [item setData:[NSData dataWithBytes:doc.data() length:doc.size()] forType:type];
}

@end

// ---- entry ---------------------------------------------------------------------------

// Foundation's app-extension entry point (what `-e _NSExtensionMain` named).
extern "C" int NSExtensionMain(int argc, char* argv[]);

namespace {

// Final Cut Pro's ProExtension.framework declares the workflow extension point:
// every extension's context is its ProExtensionRemoteContext and its principal
// class is ProExtensionRequestHandling, which makes our view controller.
// ExtensionFoundation looks the context class up on FCP's first connection and
// traps if it is missing. ProExtensionHost.framework is the extension's view of
// FCP's objects (the open library, for the panel's scope); without it the panel
// still searches, over every indexed folder. Loaded from the Final Cut Pro
// installed here, so they always match the host and nothing of Apple's is
// redistributed; Apple signs them, hence
// com.apple.security.cs.disable-library-validation (plan/12 2026-09-28).
bool load_pro_extension(os_log_t log) {
  NSMutableArray<NSURL*>* apps = [NSMutableArray array];
  for (NSString* bundle in @[ @"com.apple.FinalCut", @"com.apple.FinalCutTrial" ]) {
    if (NSURL* url = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:bundle]) [apps addObject:url];
  }
  [apps addObject:[NSURL fileURLWithPath:@"/Applications/Final Cut Pro.app"]];
  for (NSURL* app in apps) {
    NSString* frameworks = [app.path stringByAppendingPathComponent:@"Contents/Frameworks"];
    NSString* extension = [frameworks stringByAppendingPathComponent:@"ProExtension.framework/ProExtension"];
    if (dlopen(extension.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)) {
      os_log(log, "ProExtension loaded from Final Cut Pro");
      NSString* host = [frameworks stringByAppendingPathComponent:@"ProExtensionHost.framework/ProExtensionHost"];
      if (!dlopen(host.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)) {
        os_log_error(log, "ProExtensionHost did not load: %{public}s", dlerror());
      }
      return true;
    }
    os_log_error(log, "ProExtension did not load: %{public}s", dlerror());
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  @autoreleasepool {
    os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.fcp", "extension");
    if (!load_pro_extension(log) || !NSClassFromString(@"ProExtensionRemoteContext")) {
      // Nothing to host us with: FCP moved or dropped the framework. Exiting
      // is kinder than ExtensionFoundation's trap, and says why in the log.
      os_log_fault(log, "Final Cut Pro's ProExtension.framework is not available; not starting");
      return 1;
    }
  }
  return NSExtensionMain(argc, argv);
}
