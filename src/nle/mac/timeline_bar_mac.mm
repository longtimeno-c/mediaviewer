// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/29 "The panel". Final Cut Pro has no API to edit the open
// timeline, so the round trip is FCPXML both ways: the editor drags the
// project from FCP's browser onto this bar (FCP puts the whole timeline on
// the pasteboard as FCPXML), and a new version goes back as an FCPXML file
// FCP opens, or a drag into its browser. It is imported as a new project
// beside the original, never over it: the version has its own name and no
// uid (nle/timeline.h write_version).
//
//   +---------------------------------------------------------------+
//   | Timeline: "Interview" · dragged 2 min ago · changed since     |
//   | Folders: broll, Selects   ⇢ "Interview (Claude 1)"  [Folders▾] [New Version] |
//   +---------------------------------------------------------------+
//
// Nothing here reads or writes the editor's media or library: the dragged
// FCPXML and the versions live in the app group's container (Timeline/,
// nle/bridge_state.h), where the timeline agent reads them.
#import "nle/mac/timeline_bar_mac.h"

#include <os/log.h>

#include <chrono>
#include <string>
#include <vector>

#include "nle/bridge_state.h"
#include "nle/timeline.h"

namespace {

NSString* const kFCPXMLPrefix = @"com.apple.finalcutpro.xml";

os_log_t bar_log() {
  static os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.fcp", "timeline");
  return log;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s ? std::string(s.UTF8String ?: "") : std::string(); }

// Every FCPXML pasteboard type FCP may use, newest first.
NSArray<NSString*>* fcpxml_types() {
  NSMutableArray* out = [NSMutableArray array];
  for (int minor = 20; minor >= 5; --minor) [out addObject:[NSString stringWithFormat:@"%@.v1-%d", kFCPXMLPrefix, minor]];
  [out addObject:kFCPXMLPrefix];
  return out;
}

// "1.13" -> "com.apple.finalcutpro.xml.v1-13"
NSString* type_for_version(const std::string& v) {
  const std::size_t dot = v.find('.');
  if (dot == std::string::npos) return kFCPXMLPrefix;
  return [NSString stringWithFormat:@"%@.v%s-%s", kFCPXMLPrefix, v.substr(0, dot).c_str(), v.substr(dot + 1).c_str()];
}

}  // namespace

// The last version written: drag it into an FCP event to import it there.
@interface MVVersionChip : NSTextField
@property(nonatomic, weak) MVTimelineBar* bar;
@end

@interface MVTimelineBar ()
- (void)beginVersionDrag:(NSEvent*)event from:(NSView*)view;
@end

@implementation MVVersionChip
- (void)mouseDown:(NSEvent*)event {
  (void)event;  // the drag starts on the first move
}
- (void)mouseDragged:(NSEvent*)event {
  [self.bar beginVersionDrag:event from:self];
}
- (void)resetCursorRects {
  [self addCursorRect:self.bounds cursor:NSCursor.openHandCursor];
}
@end

@implementation MVTimelineBar {
  NSString* _group;
  NSURL* _dir;  // <container>/Timeline
  mv::nle::bridge_state _state;
  NSTextField* _title;
  NSTextField* _detail;
  MVVersionChip* _chip;
  NSPopUpButton* _folders;
  NSButton* _version;
  BOOL _dropping;
  NSTimer* _clock;
  dispatch_queue_t _work;
  std::int64_t _lastPlayheadWrite;
  std::string _versionDocument;  // the last version's FCPXML, for a drag
  NSURL* _versionURL;
}

- (instancetype)initWithAppGroup:(NSString*)group {
  if ((self = [super initWithFrame:NSZeroRect])) {
    _group = [group copy];
    _work = dispatch_queue_create("mediaviewer.fcp.timeline",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                          QOS_CLASS_USER_INITIATED, 0));
    NSURL* container = [NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:group];
    if (!container) {
      // An unsigned development build has no group container; its own one does.
      container = [NSURL fileURLWithPath:NSHomeDirectory() isDirectory:YES];
    }
    _dir = [container URLByAppendingPathComponent:ns(std::string(mv::nle::kBridgeFolder)) isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:[_dir URLByAppendingPathComponent:@"versions"]
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    [self loadState];
    [self build];
    [self registerForDraggedTypes:[fcpxml_types() arrayByAddingObject:NSPasteboardTypeFileURL]];
    [self refresh];
  }
  return self;
}

- (void)build {
  self.wantsLayer = YES;
  self.layer.cornerRadius = 6;
  self.layer.borderWidth = 1;
  self.layer.borderColor = NSColor.separatorColor.CGColor;

  _title = [NSTextField labelWithString:@""];
  _title.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
  _title.lineBreakMode = NSLineBreakByTruncatingTail;
  _detail = [NSTextField labelWithString:@""];
  _detail.font = [NSFont systemFontOfSize:11];
  _detail.textColor = NSColor.secondaryLabelColor;
  _detail.lineBreakMode = NSLineBreakByTruncatingTail;
  _chip = [[MVVersionChip alloc] initWithFrame:NSZeroRect];
  _chip.bar = self;
  _chip.editable = NO;
  _chip.selectable = NO;
  _chip.bordered = NO;
  _chip.drawsBackground = NO;
  _chip.font = [NSFont systemFontOfSize:11];
  _chip.textColor = NSColor.linkColor;
  _chip.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _chip.toolTip = @"The last version written. Drag it into a Final Cut Pro event to import it there.";

  _folders = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
  _folders.bezelStyle = NSBezelStyleTexturedRounded;
  _folders.toolTip = @"Folders the timeline agent may use, besides the timeline's own media.";
  _version = [NSButton buttonWithTitle:@"New Version" target:self action:@selector(newVersion:)];
  _version.bezelStyle = NSBezelStyleTexturedRounded;
  _version.toolTip = @"Write this project again as a new project beside it, and import it into Final Cut Pro.";

  for (NSView* v in @[ _title, _detail, _chip, _folders, _version ]) {
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:v];
  }
  [_title setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                   forOrientation:NSLayoutConstraintOrientationHorizontal];
  [_detail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
  [_chip setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 1
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];
  [NSLayoutConstraint activateConstraints:@[
    [_title.topAnchor constraintEqualToAnchor:self.topAnchor constant:6],
    [_title.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
    [_title.trailingAnchor constraintLessThanOrEqualToAnchor:_folders.leadingAnchor constant:-8],
    [_detail.topAnchor constraintEqualToAnchor:_title.bottomAnchor constant:2],
    [_detail.leadingAnchor constraintEqualToAnchor:_title.leadingAnchor],
    [_detail.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-6],
    [_chip.firstBaselineAnchor constraintEqualToAnchor:_detail.firstBaselineAnchor],
    [_chip.leadingAnchor constraintEqualToAnchor:_detail.trailingAnchor constant:8],
    [_chip.trailingAnchor constraintLessThanOrEqualToAnchor:_folders.leadingAnchor constant:-8],
    [_version.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    [_version.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8],
    [_folders.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    [_folders.trailingAnchor constraintEqualToAnchor:_version.leadingAnchor constant:-6],
  ]];
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  [_clock invalidate];
  _clock = nil;
  if (self.window) {
    // "dragged 2 min ago" stays true while the panel is open; nothing ticks
    // while it is closed.
    _clock = [NSTimer scheduledTimerWithTimeInterval:30
                                              target:self
                                            selector:@selector(tick:)
                                            userInfo:nil
                                             repeats:YES];
    _clock.tolerance = 5;
  }
}

- (void)tick:(NSTimer*)t {
  (void)t;
  [self refresh];
}

// ---- state --------------------------------------------------------------------------------

- (NSURL*)stateURL {
  return [_dir URLByAppendingPathComponent:ns(std::string(mv::nle::kBridgeState))];
}
- (NSURL*)projectURL {
  return [_dir URLByAppendingPathComponent:ns(std::string(mv::nle::kBridgeProject))];
}

- (void)loadState {
  NSData* d = [NSData dataWithContentsOfURL:[self stateURL]];
  if (!d) return;
  const std::string text(static_cast<const char*>(d.bytes), d.length);
  if (auto s = mv::nle::bridge_state_from_json(text)) _state = std::move(*s);
}

- (void)saveState {
  const std::string text = mv::nle::to_json(_state);
  NSData* d = [NSData dataWithBytes:text.data() length:text.size()];
  NSError* e = nil;
  if (![d writeToURL:[self stateURL] options:NSDataWritingAtomic error:&e]) {
    os_log_error(bar_log(), "state not saved");
  }
}

- (void)refresh {
  const mv::nle::bridge_state& s = _state;
  if (s.project.empty()) {
    _title.stringValue = @"Drag a project here from the Final Cut Pro browser to work on its timeline.";
    _title.textColor = NSColor.secondaryLabelColor;
  } else {
    NSMutableString* t = [NSMutableString stringWithFormat:@"Timeline: “%@” · %@", ns(s.project),
                                                           ns(mv::nle::dragged_ago(s.dragged_ms, now_ms()))];
    if (s.changed_ms > 0) [t appendString:@" · changed in Final Cut Pro since: drag it again"];
    if (!s.active_project.empty() && s.active_project != s.project) {
      [t appendFormat:@" · Final Cut Pro has “%@” open", ns(s.active_project)];
    }
    _title.stringValue = t;
    _title.textColor = s.changed_ms > 0 ? NSColor.systemOrangeColor : NSColor.labelColor;
  }
  NSMutableArray* names = [NSMutableArray array];
  for (const std::string& f : s.folders) [names addObject:ns(f).lastPathComponent];
  _detail.stringValue = names.count ? [NSString stringWithFormat:@"Folders: %@", [names componentsJoinedByString:@", "]]
                                    : @"No folder given: only the timeline's own media";
  _chip.stringValue = s.versions.empty() ? @"" : [NSString stringWithFormat:@"⇢ “%@”", ns(s.versions.back().name)];
  _version.enabled = !s.project.empty();
  [self rebuildFolderMenu];
}

// ---- folders ------------------------------------------------------------------------------

- (void)rebuildFolderMenu {
  NSMenu* m = [[NSMenu alloc] init];
  [m addItemWithTitle:@"Folders" action:nil keyEquivalent:@""];  // a pull-down's title item
  for (std::size_t i = 0; i < _state.folders.size(); ++i) {
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"Remove “%@”", ns(_state.folders[i]).lastPathComponent]
               action:@selector(removeFolder:)
        keyEquivalent:@""];
    item.target = self;
    item.tag = static_cast<NSInteger>(i);
    item.toolTip = ns(_state.folders[i]);
    [m addItem:item];
  }
  if (!_state.folders.empty()) [m addItem:[NSMenuItem separatorItem]];
  NSMenuItem* add = [[NSMenuItem alloc] initWithTitle:@"Add Folder…" action:@selector(addFolder:) keyEquivalent:@""];
  add.target = self;
  [m addItem:add];
  _folders.menu = m;
}

- (void)addFolder:(id)sender {
  (void)sender;
  NSOpenPanel* p = [NSOpenPanel openPanel];
  p.canChooseDirectories = YES;
  p.canChooseFiles = NO;
  p.allowsMultipleSelection = YES;
  p.prompt = @"Give Folder";
  p.message = @"The timeline agent may search and use the media in these folders, besides the timeline's own.";
  [p beginSheetModalForWindow:self.window
            completionHandler:^(NSModalResponse r) {
              if (r != NSModalResponseOK) return;
              for (NSURL* u in p.URLs) {
                const std::string path = str(u.path);
                if (path.empty() || path == "/") continue;  // never the whole disk
                bool have = false;
                for (const std::string& f : self->_state.folders) have = have || f == path;
                if (!have) self->_state.folders.push_back(path);
              }
              [self saveState];
              [self refresh];
            }];
}

- (void)removeFolder:(NSMenuItem*)item {
  const auto i = static_cast<std::size_t>(item.tag);
  if (i < _state.folders.size()) _state.folders.erase(_state.folders.begin() + static_cast<std::ptrdiff_t>(i));
  [self saveState];
  [self refresh];
}

// ---- Final Cut Pro ------------------------------------------------------------------------

- (void)finalCutChanged {
  NSString* (^active)(void) = self.activeProject;
  if (!active) return;
  dispatch_async(_work, ^{
    NSString* name = active();
    dispatch_async(dispatch_get_main_queue(), ^{
      const std::string n = str(name);
      bool dirty = n != self->_state.active_project;
      self->_state.active_project = n;
      // FCP reports a change as the drag ends too: only later ones count.
      if (!n.empty() && n == self->_state.project && self->_state.dragged_ms > 0 &&
          now_ms() - self->_state.dragged_ms > 3000 && self->_state.changed_ms == 0) {
        self->_state.changed_ms = now_ms();
        dirty = true;
      }
      if (dirty) {
        [self saveState];
        [self refresh];
      }
    });
  });
}

- (void)playheadChanged {
  double (^playhead)(void) = self.playhead;
  if (!playhead || _state.project.empty()) return;
  // At most once a second: the agent reads it when asked, not as it moves.
  if (now_ms() - _lastPlayheadWrite < 1000) return;
  _lastPlayheadWrite = now_ms();
  dispatch_async(_work, ^{
    const double at = playhead();
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_state.playhead_s = at;
      self->_state.playhead_ms_at = now_ms();
      [self saveState];
    });
  });
}

// ---- the drop -----------------------------------------------------------------------------

// The FCPXML on the pasteboard: FCP's own data, or a .fcpxml / .fcpxmld file.
- (NSData*)fcpxmlFrom:(NSPasteboard*)pb {
  for (NSPasteboardItem* item in pb.pasteboardItems) {
    for (NSString* type in fcpxml_types()) {
      if ([item.types containsObject:type]) {
        NSData* d = [item dataForType:type];
        if (d.length) return d;
      }
    }
  }
  NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ NSURL.class ]
                                            options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  for (NSURL* u in urls) {
    NSString* ext = u.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"fcpxml"]) return [NSData dataWithContentsOfURL:u];
    if ([ext isEqualToString:@"fcpxmld"]) return [NSData dataWithContentsOfURL:[u URLByAppendingPathComponent:@"Info.fcpxml"]];
  }
  return nil;
}

- (BOOL)accepts:(id<NSDraggingInfo>)info {
  NSPasteboard* pb = info.draggingPasteboard;
  if ([pb availableTypeFromArray:fcpxml_types()]) return YES;
  NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ NSURL.class ]
                                            options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  for (NSURL* u in urls) {
    NSString* ext = u.pathExtension.lowercaseString;
    if ([ext isEqualToString:@"fcpxml"] || [ext isEqualToString:@"fcpxmld"]) return YES;
  }
  return NO;
}

- (void)setDropping:(BOOL)on {
  _dropping = on;
  self.layer.borderColor = (on ? NSColor.controlAccentColor : NSColor.separatorColor).CGColor;
  self.layer.borderWidth = on ? 2 : 1;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  if ([info draggingSource] == self || ![self accepts:info]) return NSDragOperationNone;
  [self setDropping:YES];
  return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
  (void)info;
  [self setDropping:NO];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  [self setDropping:NO];
  NSData* data = [self fcpxmlFrom:info.draggingPasteboard];
  if (!data.length) return NO;
  NSURL* projectURL = [self projectURL];
  dispatch_async(_work, ^{
    std::string text(static_cast<const char*>(data.bytes), data.length);
    mv::nle::timeline t;
    std::string error;
    const bool ok = t.load(text, &error);
    std::string name, event, library, version;
    std::size_t count = 0;
    if (ok) {
      name = t.project().name;
      event = t.project().event_name;
      library = t.project().library;
      version = t.version();
      count = t.items().size();
      [data writeToURL:projectURL options:NSDataWritingAtomic error:nil];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ok) {
        os_log(bar_log(), "drop refused");
        self->_title.stringValue = [NSString stringWithFormat:@"Not a project: %@", ns(error)];
        self->_title.textColor = NSColor.systemRedColor;
        return;
      }
      os_log(bar_log(), "project held, %zu items", count);
      mv::nle::bridge_state& s = self->_state;
      s.project = name;
      s.event = event;
      s.library = library;
      s.fcpxml_version = version;
      s.dragged_ms = now_ms();
      s.changed_ms = 0;
      [self saveState];
      [self refresh];
    });
  });
  return YES;
}

// ---- a new version ------------------------------------------------------------------------

- (void)newVersion:(id)sender {
  (void)sender;
  NSURL* projectURL = [self projectURL];
  NSURL* versions = [_dir URLByAppendingPathComponent:@"versions" isDirectory:YES];
  std::vector<std::string> taken;
  for (const mv::nle::bridge_version& v : _state.versions) taken.push_back(v.name);
  const std::string base = _state.project;
  _version.enabled = NO;
  dispatch_async(_work, ^{
    NSData* data = [NSData dataWithContentsOfURL:projectURL];
    mv::nle::timeline t;
    std::string error = "the dragged project is gone: drag it again";
    bool ok = data && t.load(std::string(static_cast<const char*>(data.bytes), data.length), &error, base);
    std::string name, document;
    NSURL* out = nil;
    if (ok) {
      std::vector<std::string> all = taken;
      all.insert(all.end(), t.project_names().begin(), t.project_names().end());
      name = mv::nle::next_version_name(t.project().name, all);
      document = t.write_version(name);
      out = [versions URLByAppendingPathComponent:ns(mv::nle::version_file_name(name))];
      ok = [[NSData dataWithBytes:document.data() length:document.size()] writeToURL:out
                                                                             options:NSDataWritingAtomic
                                                                               error:nil];
      if (!ok) error = "the version could not be written";
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_version.enabled = YES;
      if (!ok) {
        self->_title.stringValue = ns(error);
        self->_title.textColor = NSColor.systemRedColor;
        return;
      }
      self->_state.versions.push_back({name, str(out.lastPathComponent), now_ms(), base});
      self->_versionDocument = document;
      self->_versionURL = out;
      [self saveState];
      [self refresh];
      [self sendToFinalCut:out];
    });
  });
}

// Final Cut Pro imports an FCPXML file it is asked to open, into the library
// the document names (the editor's own): a new project beside the original.
- (void)sendToFinalCut:(NSURL*)file {
  NSURL* fcp = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.FinalCut"]
                   ?: [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.FinalCutTrial"];
  if (!fcp) return;
  NSWorkspaceOpenConfiguration* c = [NSWorkspaceOpenConfiguration configuration];
  c.activates = YES;
  [NSWorkspace.sharedWorkspace openURLs:@[ file ]
                   withApplicationAtURL:fcp
                          configuration:c
                      completionHandler:^(NSRunningApplication* app, NSError* e) {
                        (void)app;
                        if (e) os_log_error(bar_log(), "version not handed to Final Cut Pro");
                      }];
}

- (void)beginVersionDrag:(NSEvent*)event from:(NSView*)view {
  if (_state.versions.empty()) return;
  if (!_versionURL) {
    _versionURL = [[_dir URLByAppendingPathComponent:@"versions" isDirectory:YES]
        URLByAppendingPathComponent:ns(_state.versions.back().file)];
    NSData* d = [NSData dataWithContentsOfURL:_versionURL];
    if (!d) return;
    _versionDocument.assign(static_cast<const char*>(d.bytes), d.length);
  }
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  [item setDataProvider:self forTypes:@[ type_for_version(_state.fcpxml_version), kFCPXMLPrefix ]];
  [item setString:_versionURL.absoluteString forType:NSPasteboardTypeFileURL];
  NSDraggingItem* drag = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
  [drag setDraggingFrame:view.bounds contents:nil];
  [view beginDraggingSessionWithItems:@[ drag ] event:event source:self];
}

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
  (void)pasteboard;
  [item setData:[NSData dataWithBytes:_versionDocument.data() length:_versionDocument.size()] forType:type];
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  (void)session;
  return context == NSDraggingContextOutsideApplication ? NSDragOperationCopy : NSDragOperationNone;
}

@end
