// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "MediaViewer Search": the Final Cut Pro workflow extension (plan/23 Phase 0
// spike, with the live search of Phase 1 behind it). FCP shows this view
// controller in a floating window from its Extensions button; the process is
// sandboxed and reads nothing of the user's: every search and every tile comes
// from the search agent over XPC (agent_protocol.h).
//
// Phase 0 asked FCP, empirically, whether it takes an extension with no Apple
// SDK framework. It does not (2026-09-28): its extension point names
// ProExtension.framework's classes as the context and principal class, and
// the process traps without them. So main() below loads that framework from
// the installed Final Cut Pro before the extension starts; we ship nothing of
// Apple's. Our view controller is ProExtensionPrincipalViewControllerClass.
// Phase 0's other question: does a drag of FCPXML + file URLs import? So: a
// search field, a list of results, and a drag source. The first row, "Test
// drag", is the spike's hard-coded document (~/Movies/test: two clips with
// ranges and a photo, keyword "MV: test"). Phase 2 replaces this with the
// SwiftUI grid.
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <os/log.h>

#include <pwd.h>
#include <unistd.h>

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

std::string home_dir() {
  // The sandbox's NSHomeDirectory is the container; the spike's files are the user's.
  const passwd* pw = getpwuid(getuid());
  return pw && pw->pw_dir ? pw->pw_dir : "";
}

NSString* mmss(std::int64_t ms) {
  if (ms < 0) return @"photo";
  const long long s = ms / 1000;
  return [NSString stringWithFormat:@"%lld:%02lld", s / 60, s % 60];
}

}  // namespace

@interface MVSearchViewController : NSViewController <NSTableViewDataSource, NSTableViewDelegate,
                                                      NSSearchFieldDelegate, NSPasteboardItemDataProvider>
@end

@implementation MVSearchViewController {
  NSSearchField* _field;
  NSTableView* _table;
  NSTextField* _status;
  NSXPCConnection* _agent;
  std::vector<mv::nle::row> _rows;  // row 0 is the spike's test drag
  std::vector<mv::nle::row> _dragging;
  std::string _query;
  std::uint64_t _generation;
  NSCache<NSString*, NSImage*>* _tiles;
}

- (void)loadView {
  NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 420, 560)];
  root.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];  // FCP's look
  _field = [[NSSearchField alloc] initWithFrame:NSZeroRect];
  _field.placeholderString = @"Search your footage";
  _field.delegate = self;
  _field.sendsSearchStringImmediately = NO;
  _field.target = self;
  _field.action = @selector(search:);
  _status = [NSTextField labelWithString:@""];
  _status.textColor = NSColor.secondaryLabelColor;
  _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
  NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"row"];
  [_table addTableColumn:col];
  _table.headerView = nil;
  _table.rowHeight = 64;
  _table.allowsMultipleSelection = YES;
  _table.dataSource = self;
  _table.delegate = self;
  [_table setDraggingSourceOperationMask:NSDragOperationCopy forLocal:NO];
  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  scroll.documentView = _table;
  scroll.hasVerticalScroller = YES;
  for (NSView* v in @[ _field, _status, scroll ]) {
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:v];
  }
  [NSLayoutConstraint activateConstraints:@[
    [_field.topAnchor constraintEqualToAnchor:root.topAnchor constant:12],
    [_field.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:12],
    [_field.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-12],
    [_status.topAnchor constraintEqualToAnchor:_field.bottomAnchor constant:6],
    [_status.leadingAnchor constraintEqualToAnchor:_field.leadingAnchor],
    [_status.trailingAnchor constraintEqualToAnchor:_field.trailingAnchor],
    [scroll.topAnchor constraintEqualToAnchor:_status.bottomAnchor constant:6],
    [scroll.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
  ]];
  self.view = root;
  _tiles = [[NSCache alloc] init];
  _generation = 0;
  [self resetRows];
}

- (void)viewDidAppear {
  [super viewDidAppear];
  [self.view.window makeFirstResponder:_field];
}

- (void)resetRows {
  _rows.clear();
  // The spike's hard-coded drag (plan/23 Phase 0 verify).
  const std::string dir = home_dir() + "/Movies/test/";
  mv::nle::row a;
  a.path = dir + "clip1.mov";
  a.kind = 2;
  a.pts_ms = 4000;
  mv::nle::row b;
  b.path = dir + "clip2.mov";
  b.kind = 2;
  b.pts_ms = 10000;
  mv::nle::row c;
  c.path = dir + "photo.jpg";
  c.kind = 1;
  _rows = {a, b, c};
  _query = "test";
  _status.stringValue = @"Test drag: ~/Movies/test (clip1.mov, clip2.mov, photo.jpg)";
  [_table reloadData];
}

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

- (void)search:(id)sender {
  (void)sender;
  NSString* text = [_field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (text.length == 0) {
    [self resetRows];
    return;
  }
  const std::uint64_t generation = ++_generation;
  mv::nle::request r;
  r.correlation_id = generation;
  const auto req = mv::nle::encode(r);
  _status.stringValue = @"Searching…";
  const std::string query = text.UTF8String;
  __weak MVSearchViewController* weak = self;
  id<MVSearchAgent> proxy = [[self agent] remoteObjectProxyWithErrorHandler:^(NSError* e) {
    (void)e;
    dispatch_async(dispatch_get_main_queue(), ^{
      MVSearchViewController* me = weak;
      if (me && me->_generation == generation) {
        me->_status.stringValue = @"Local search is not available. In MediaViewer, open Settings > Local search and turn on "
                                    @"Final Cut Pro.";
      }
    });
  }];
  [proxy run:[NSData dataWithBytes:req.data() length:req.size()]
          text:text
      scopeDir:@""
     withReply:^(NSData* reply) {
       auto decoded = mv::nle::decode(
           std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(reply.bytes), reply.length));
       // A block copies what it captures: hand the rows over in a shared box.
       auto rep = decoded ? std::make_shared<mv::nle::reply>(std::move(*decoded)) : nullptr;
       dispatch_async(dispatch_get_main_queue(), ^{
         MVSearchViewController* me = weak;
         if (!me || me->_generation != generation) return;  // a newer query owns the list
         if (!rep) {
           me->_status.stringValue = @"Local search sent an answer this panel cannot read.";
           return;
         }
         if (rep->code == mv::status::unsupported_format) {
           me->_status.stringValue = @"Local search needs an update.";
         } else if (rep->code == mv::status::not_found) {
           me->_status.stringValue = @"Nothing is indexed yet. Open MediaViewer to index your footage.";
         } else if (rep->code != mv::status::ok) {
           me->_status.stringValue = @"Local search is still loading. Try again in a moment.";
         } else {
           me->_status.stringValue =
               rep->rows.empty() ? @"Nothing found"
                                 : [NSString stringWithFormat:@"%zu results. Drag into an event or the timeline.",
                                                              rep->rows.size()];
         }
         me->_rows = std::move(rep->rows);
         me->_query = query;
         [me->_table reloadData];
       });
     }];
}

// ---- the list ----------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  (void)tableView;
  return static_cast<NSInteger>(_rows.size());
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  (void)column;
  NSTableCellView* cell = [tableView makeViewWithIdentifier:@"cell" owner:self];
  if (!cell) {
    cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 400, 64)];
    cell.identifier = @"cell";
    NSImageView* image = [[NSImageView alloc] initWithFrame:NSMakeRect(8, 4, 96, 56)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    NSTextField* label = [NSTextField labelWithString:@""];
    label.frame = NSMakeRect(112, 20, 280, 24);
    label.autoresizingMask = NSViewWidthSizable;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [cell addSubview:image];
    [cell addSubview:label];
    cell.imageView = image;
    cell.textField = label;
  }
  const mv::nle::row& x = _rows[static_cast<std::size_t>(row)];
  NSString* name = [[NSString stringWithUTF8String:x.path.c_str()] lastPathComponent];
  NSString* more = x.more > 0 ? [NSString stringWithFormat:@"  +%u in this clip", x.more] : @"";
  cell.textField.stringValue = [NSString stringWithFormat:@"%@  %@%@", name, mmss(x.pts_ms), more];
  cell.imageView.image = nil;
  if (!x.thumb.empty()) {
    NSString* key = [NSString stringWithUTF8String:x.thumb.c_str()];
    if (NSImage* hit = [_tiles objectForKey:key]) {
      cell.imageView.image = hit;
    } else {
      __weak NSTableCellView* weakCell = cell;
      NSCache* tiles = _tiles;
      [[[self agent] remoteObjectProxy] thumbnail:key
                                        withReply:^(NSData* jpeg) {
                                          NSImage* img = jpeg ? [[NSImage alloc] initWithData:jpeg] : nil;
                                          if (!img) return;
                                          dispatch_async(dispatch_get_main_queue(), ^{
                                            [tiles setObject:img forKey:key];
                                            weakCell.imageView.image = img;
                                          });
                                        }];
    }
  }
  return cell;
}

// ---- the drag: FCPXML for the whole selection, a file URL per item ----------------------

- (id<NSPasteboardWriting>)tableView:(NSTableView*)tableView pasteboardWriterForRow:(NSInteger)row {
  const mv::nle::row& x = _rows[static_cast<std::size_t>(row)];
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:x.path.c_str()]];
  [item setString:url.absoluteString forType:NSPasteboardTypeFileURL];
  // One document for the whole drag, on its first item (the lowest selected
  // row, or the unselected row being dragged), made when a target asks.
  NSIndexSet* selected = tableView.selectedRowIndexes;
  const bool first = ![selected containsIndex:static_cast<NSUInteger>(row)] ||
                     static_cast<NSUInteger>(row) == selected.firstIndex;
  if (first) [item setDataProvider:self forTypes:@[ kFCPXMLTypeVersioned, kFCPXMLType ]];
  return item;
}

- (void)tableView:(NSTableView*)tableView
    draggingSession:(NSDraggingSession*)session
    willBeginAtPoint:(NSPoint)point
      forRowIndexes:(NSIndexSet*)rowIndexes {
  (void)tableView;
  (void)session;
  (void)point;
  _dragging.clear();
  [rowIndexes enumerateIndexesUsingBlock:^(NSUInteger i, BOOL*) {
    if (i < self->_rows.size()) self->_dragging.push_back(self->_rows[i]);
  }];
}

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
  (void)pasteboard;
  mv::nle::fcpxml_options o;
  o.event_name = "MediaViewer: " + _query;
  o.keyword = "MV: " + _query;
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
// traps if it is missing. Loaded from the Final Cut Pro installed here, so it
// always matches the host and nothing of Apple's is redistributed; Apple signs
// it, hence com.apple.security.cs.disable-library-validation (plan/12
// 2026-09-28). Read-only: the sandbox may read and map /Applications.
bool load_pro_extension(os_log_t log) {
  NSMutableArray<NSURL*>* apps = [NSMutableArray array];
  for (NSString* bundle in @[ @"com.apple.FinalCut", @"com.apple.FinalCutTrial" ]) {
    if (NSURL* url = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:bundle]) [apps addObject:url];
  }
  [apps addObject:[NSURL fileURLWithPath:@"/Applications/Final Cut Pro.app"]];
  for (NSURL* app in apps) {
    NSString* framework =
        [app.path stringByAppendingPathComponent:@"Contents/Frameworks/ProExtension.framework/ProExtension"];
    if (dlopen(framework.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)) {
      os_log(log, "ProExtension loaded from Final Cut Pro");
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
