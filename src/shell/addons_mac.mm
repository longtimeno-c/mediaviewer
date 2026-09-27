// SPDX-License-Identifier: GPL-2.0-or-later
// The Mac host's add-ons (plan/18 "Add-ons"; the Windows twin is
// abi/addon_abi.cpp + IslandHost.Addons.cs). Owns the add-on store, the
// loaded add-ons and their chromes:
//
//   Import  (Milestone G)  Import.bundle, principal class MVImportChrome, which
//                          drives an NSWindow hosting the SwiftUI Import view.
//   AI      (Milestone H)  AI.bundle, principal class MVAIChrome: the search
//                          panel and the Settings -> Local search management
//                          view (plan/17). Pieces of its family ("ai-faces")
//                          carry no code; the AI pack reaches their verified
//                          folders through the host table's piece_dir. Apple
//                          silicon only: ONNX Runtime ships no x86_64 macOS
//                          build, so an Intel Mac never offers Local search.
//
// The app runs with the hardened runtime and library validation, so an
// add-on's dylibs and bundle load only if signed by the same Team ID; the
// store re-verifies the signed manifest and every file before each load.
// Downloads happen in Swift (AddonsView.swift, LocalSearchView.swift); this
// file verifies, installs, removes, loads, and bridges events to the add-on
// chromes on the main queue.
//
// Compatibility: an installed Import 1.0.0 bundle was built against the
// MVAddonChrome selectors and the MvAddonHostMac selectors below, and the
// base Swift calls the mv_addons_* functions. None of those change; the AI
// pack's additions are new optional selectors and the mv_addon2_* functions.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "shell/addons_mac.h"

#include <mediaviewer/mediaviewer_ai.h>
#include <mediaviewer/mediaviewer_import.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/media.h"
#include "addon/store.h"
#include "core/json.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "meta/meta.h"
#include "mv_chrome_bridge.h"
#include "shell/commands.h"
#include "shell/media_kind.h"
#include "shell/present_busy.h"

// The selectors the Import.bundle principal class answers (MVImportChrome in
// src.swift/ImportChrome). Declared here only so the calls type-check; the
// bundle is reached by message send, never by linking. Frozen: Import 1.0.0.
@protocol MVAddonChrome <NSObject>
- (void)attachWithTable:(NSValue*)table host:(id)host;
- (void)openWithSource:(NSString*)source marks:(NSArray<NSString*>*)marks;
- (void)importNow:(NSString*)pathsJson;
- (void)deliverEvent:(uint32_t)kind status:(uint32_t)status identifier:(uint64_t)ident payload:(int64_t)payload;
- (void)shutdown;
@end

// Milestone H: what AI.bundle's principal class (MVAIChrome in
// src.swift/AIChrome) answers beyond attach / deliver / shutdown. Every one is
// optional and checked with -respondsToSelector: before it is sent, so a
// later chrome may add more and an older one may lack some.
@protocol MVAIChrome <NSObject>
- (void)attachWithTable:(NSValue*)table host:(id)host;
- (void)deliverEvent:(uint32_t)kind status:(uint32_t)status identifier:(uint64_t)ident payload:(int64_t)payload;
- (void)shutdown;
@optional
// search_open / search_similar / search_next_match / search_prev_match.
- (BOOL)runCommand:(NSString*)name;
// A directory opened in the viewer (the chrome calls note_folder_opened).
- (void)folderChanged:(NSString*)dir;
// The canvas item changed ("" none): scrub markers for a clip in the search.
- (void)itemChanged:(NSString*)path;
// Settings -> Local search: the management view, owned (and kept) by the chrome.
- (NSView*)settingsView;
@end

@interface MvAddonHostMac : NSObject
@end

namespace {

// One loaded add-on with a chrome.
struct addon_slot {
  std::unique_ptr<mv::addon::loaded_addon> addon;
  id chrome = nil;
  NSBundle* bundle = nil;
  const void* table = nullptr;
  bool loading = false;
  std::string error;       // why the last load failed ("" none); no path, ever
  std::uint64_t load_seq = 0;
};

struct mac_addons {
  std::unique_ptr<mv::addon::store> store;
  std::unique_ptr<mv::addon::loaded_addon> import;
  id<MVAddonChrome> chrome = nil;
  NSBundle* bundle = nil;
  addon_slot ai;
  MvAddonHostMac* host = nil;
  MvAddonsOpenPathFn open_path = nullptr;
  void* open_path_ctx = nullptr;
  std::string status;       // command-bar line while a job runs
  bool hint_pending = false;
  id mount_observer = nil;
  std::mutex thumbs_mutex;  // the lazy open below; lookups are the store's own
  mv::image::thumb_store thumbs;
  std::string folder;       // the directory the viewer last opened
};

mac_addons& state() {
  static mac_addons s;
  return s;
}

// The Mac AI pack is arm64 only (plan/17, 2026-09-26): ONNX Runtime has no
// x86_64 macOS build. On Intel, Local search does not exist at all.
constexpr bool ai_supported() {
#if defined(__arm64__) || defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

bool is_ai_family(const std::string& id) { return id == "ai" || id == "ai-faces"; }

std::string hint_marker() {
  auto dir = mv::io::addons_dir();
  return dir ? mv::io::join_path(*dir, ".import-hint-dismissed") : std::string();
}

bool ensure_store() {
  mac_addons& s = state();
  if (s.store) return true;
  auto root = mv::io::addons_dir();
  if (!root) return false;
  auto key = mv::addon::pinned_public_key();
  s.store = std::make_unique<mv::addon::store>(*root, std::vector<std::uint8_t>(key.begin(), key.end()),
                                               MV_ADDON_HOST_API);
  s.store->startup_cleanup();
  return true;
}

bool capture_info(const std::string& path, mv_addon_capture& out) {
  auto m = mv::meta::read(path);
  if (!m) return false;
  if (m->s.date_taken_key != 0) {
    out.taken_unix = m->s.date_taken_key;
    out.has_date = 1;
  }
  const std::size_t n = std::min(m->s.camera.size(), sizeof(out.camera_utf8) - 1);
  std::memcpy(out.camera_utf8, m->s.camera.data(), n);
  out.camera_utf8[n] = '\0';
  return true;
}

mv::result<std::string> thumbnail(const std::string& path) {
  mac_addons& s = state();
  {
    // Two add-ons' workers can reach here at once (Import's plan, the AI
    // pack's result tiles): the one-time open is serialised.
    std::lock_guard<std::mutex> lock(s.thumbs_mutex);
    if (!s.thumbs.is_open()) {
      MV_TRY(std::string dir, mv::io::thumb_cache_dir());
      MV_TRY_VOID(s.thumbs.open(dir));
    }
  }
  MV_TRY(mv::io::file_stat st, mv::io::stat_path(path));
  const mv::image::thumb_key key{path, st.mtime_unix, st.size};
  MV_TRY(std::string hit, s.thumbs.lookup(key));
  if (!hit.empty()) return hit;
  MV_TRY(auto bytes, mv::io::read_all(path));
  MV_TRY(auto jpeg, mv::image::make_thumb_jpeg(bytes));
  return s.thumbs.store(key, jpeg);
}

bool import_installed_ok() {
  if (!ensure_store()) return false;
  auto found = state().store->find("import");
  return found && found->state == mv::addon::install_state::ok;
}

bool ai_installed_ok() {
  if (!ai_supported() || !ensure_store()) return false;
  auto found = state().store->find("ai");
  return found && found->state == mv::addon::install_state::ok;
}

void unload_import() {
  mac_addons& s = state();
  if (s.chrome) {
    @try {
      [s.chrome shutdown];
    } @catch (NSException*) {
    }
  }
  s.chrome = nil;
  s.import.reset();  // shutdown, then the dylib goes
  // NSBundle -unload is not safe for Swift code; the bundle stays mapped
  // until quit, inert (plan/18: removal completes at next start).
  s.bundle = nil;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::import, false);
}

bool load_import() {
  mac_addons& s = state();
  if (s.chrome) return true;
  if (!ensure_store()) return false;
  mv::addon::host_services svc;
  svc.capture = &capture_info;
  svc.thumbnail = &thumbnail;
  svc.should_yield = [] { return mv::shell::g_present_busy.load(std::memory_order_relaxed); };
  svc.post = [](const mv_addon_event& e) {
    const mv_addon_event copy = e;
    dispatch_async(dispatch_get_main_queue(), ^{
      mac_addons& st = state();
      if (st.chrome) {
        [st.chrome deliverEvent:copy.kind status:copy.status identifier:copy.id payload:copy.payload];
      }
    });
  };
  if (auto lib = mv::io::default_library_dir()) svc.default_library = *lib;
  auto loaded = mv::addon::loaded_addon::load(*s.store, "import", std::move(svc));
  if (!loaded) return false;
  const auto* table = (*loaded)->query(MV_IMPORT_INTERFACE);
  if (!table) return false;

  const auto& info = (*loaded)->info();
  const std::string bundle_path = mv::io::join_path(info.dir, mv::io::native_relative(info.m.chrome));
  NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle_path.c_str()]];
  NSError* error = nil;
  if (!bundle || ![bundle loadAndReturnError:&error]) return false;
  Class principal = bundle.principalClass;
  if (!principal) return false;
  id<MVAddonChrome> chrome = [[principal alloc] init];
  if (![chrome respondsToSelector:@selector(attachWithTable:host:)]) return false;
  if (!s.host) s.host = [[MvAddonHostMac alloc] init];
  [chrome attachWithTable:[NSValue valueWithPointer:table] host:s.host];
  s.import = std::move(*loaded);
  s.bundle = bundle;
  s.chrome = chrome;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::import, true);
  return true;
}

// ---- the AI pack (Milestone H) ------------------------------------------------

// The host table v2 services the AI pack needs: the viewer's own decoders and
// its JPEG-512 cache (src/addon/media.h), the same set abi/addon_abi.cpp's
// mv_addon_load installs on Windows.
mv::addon::host_services ai_services() {
  mv::addon::host_services svc;
  svc.capture = &capture_info;
  svc.thumbnail = &thumbnail;
  // Indexing waits while the present loop is busy (plan/17 "Yield policy":
  // playback, panning, a slideshow, a transition).
  svc.should_yield = [] { return mv::shell::g_present_busy.load(std::memory_order_relaxed); };
  svc.post = [](const mv_addon_event& e) {
    const mv_addon_event copy = e;
    dispatch_async(dispatch_get_main_queue(), ^{
      id chrome = state().ai.chrome;
      if (chrome) {
        [(id<MVAIChrome>)chrome deliverEvent:copy.kind status:copy.status identifier:copy.id
                                     payload:copy.payload];
      }
    });
  };
  if (auto lib = mv::io::default_library_dir()) svc.default_library = *lib;
  svc.still_rgb = &mv::addon::media::decode_still;
  svc.open_sampler = &mv::addon::media::open_sampler;
  svc.video_frame = &mv::addon::media::video_frame;
  svc.moment_thumbnail = &mv::addon::media::moment_thumbnail;
  // piece_dir is left empty: loaded_addon::load serves the family's own
  // verified pieces from the store.
  return svc;
}

void unload_ai() {
  mac_addons& s = state();
  ++s.ai.load_seq;  // a load in flight lands on nothing
  if (s.ai.chrome) {
    @try {
      [(id<MVAIChrome>)s.ai.chrome shutdown];
    } @catch (NSException*) {
    }
  }
  s.ai.chrome = nil;
  s.ai.table = nullptr;
  s.ai.addon.reset();  // the pack stops its workers (indexing resumes next load), then the dylib goes
  s.ai.bundle = nil;   // mapped until quit, inert (NSBundle -unload is unsafe for Swift)
  s.ai.loading = false;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::ai, false);
  // Nothing may point into the gone table's clip: drop the markers.
  mv_chrome_set_scrub_markers(nullptr, nullptr, 0, -1);
}

// On the main thread, once the native side loaded on a worker: the bundle,
// its principal class, attach.
void attach_ai(std::unique_ptr<mv::addon::loaded_addon> loaded) {
  mac_addons& s = state();
  const void* table = loaded->query(MV_AI_INTERFACE);
  if (!table) {
    s.ai.error = "interface";
    return;
  }
  const auto& info = loaded->info();
  const std::string bundle_path = mv::io::join_path(info.dir, mv::io::native_relative(info.m.chrome));
  NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle_path.c_str()]];
  NSError* error = nil;
  if (!bundle || ![bundle loadAndReturnError:&error]) {
    // Library validation refuses a bundle signed by another team here.
    s.ai.error = "bundle";
    return;
  }
  Class principal = bundle.principalClass;
  id chrome = principal ? [[principal alloc] init] : nil;
  if (!chrome || ![chrome respondsToSelector:@selector(attachWithTable:host:)] ||
      ![chrome respondsToSelector:@selector(deliverEvent:status:identifier:payload:)]) {
    s.ai.error = "chrome";
    return;
  }
  if (!s.host) s.host = [[MvAddonHostMac alloc] init];
  [(id<MVAIChrome>)chrome attachWithTable:[NSValue valueWithPointer:table] host:s.host];
  s.ai.addon = std::move(loaded);
  s.ai.table = table;
  s.ai.bundle = bundle;
  s.ai.chrome = chrome;
  s.ai.error.clear();
  mv::shell::set_addon_commands_available(mv::shell::addon_family::ai, true);
  // The folder already on screen counts as opened.
  if (!s.folder.empty() && [chrome respondsToSelector:@selector(folderChanged:)]) {
    [(id<MVAIChrome>)chrome folderChanged:[NSString stringWithUTF8String:s.folder.c_str()]];
  }
}

// Verifying hashes every file of a pack that can be gigabytes of models, and
// the pack starts its workers: all of it on a utility queue (rule 1). Only the
// NSBundle load and attach come back to the main thread.
bool start_ai_load() {
  mac_addons& s = state();
  if (!ai_supported()) return false;
  if (s.ai.chrome || s.ai.loading) return true;
  if (!ensure_store()) return false;
  s.ai.loading = true;
  s.ai.error.clear();
  const std::uint64_t seq = ++s.ai.load_seq;
  mv::addon::store* store = s.store.get();  // lives until exit
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    auto loaded = mv::addon::loaded_addon::load(*store, "ai", ai_services());
    const std::string why = loaded ? std::string()
                                   : std::string(mv::status_name(loaded.error()));
    // A unique_ptr cannot ride in a block; hand it over as a raw pointer the
    // main thread takes back.
    mv::addon::loaded_addon* raw = loaded ? loaded->release() : nullptr;
    dispatch_async(dispatch_get_main_queue(), ^{
      std::unique_ptr<mv::addon::loaded_addon> owned(raw);
      mac_addons& st = state();
      if (seq != st.ai.load_seq) return;  // removed or unloaded meanwhile: owned shuts it down
      st.ai.loading = false;
      if (!owned) {
        st.ai.error = why.empty() ? "load" : why;
        return;
      }
      attach_ai(std::move(owned));
    });
  });
  return true;
}

id<MVAIChrome> ai_chrome() { return (id<MVAIChrome>)state().ai.chrome; }

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

std::string read_bridge(int32_t (*fn)(char*, int32_t)) {
  const int32_t need = fn(nullptr, 0);
  if (need <= 0) return {};
  std::string out(static_cast<std::size_t>(need) + 1, '\0');
  fn(out.data(), static_cast<int32_t>(out.size()));
  out.resize(static_cast<std::size_t>(need));
  return out;
}

}  // namespace

@implementation MvAddonHostMac
- (void)openInViewer:(NSString*)path {
  mac_addons& s = state();
  if (s.open_path && path.UTF8String) s.open_path(s.open_path_ctx, path.UTF8String);
}
- (void)setStatus:(NSString*)text {
  state().status = text ? std::string(text.UTF8String ?: "") : std::string();
}
- (void)notifyTitle:(NSString*)title body:(NSString*)body {
  // UNUserNotificationCenter needs an authorization prompt; the summary in the
  // Import window is the notification until the user allows them.
  (void)title;
  (void)body;
  NSBeep();
}

// ---- Milestone H: what the AI chrome asks of the viewer ----------------------
// Called by message send (-performSelector:withObject:) from Swift, so every
// argument and result is an object. [main-thread]

// {"title": String, "paths": [String], "moments": [Number] (ms, -1 none),
//  "select": Number, "gallery": Bool}. Enter / Cmd+Enter in the search panel.
- (NSNumber*)openList:(NSDictionary*)request {
  NSArray* paths = [request[@"paths"] isKindOfClass:[NSArray class]] ? request[@"paths"] : nil;
  if (paths.count == 0) return @NO;
  NSArray* moments = [request[@"moments"] isKindOfClass:[NSArray class]] ? request[@"moments"] : nil;
  NSString* title = [request[@"title"] isKindOfClass:[NSString class]] ? request[@"title"] : @"";
  const int32_t select = [request[@"select"] respondsToSelector:@selector(intValue)]
                             ? [request[@"select"] intValue]
                             : 0;
  const bool gallery = [request[@"gallery"] respondsToSelector:@selector(boolValue)] &&
                       [request[@"gallery"] boolValue];
  std::vector<std::string> keep;
  std::vector<const char*> ptrs;
  std::vector<int64_t> ms;
  keep.reserve(paths.count);
  for (NSUInteger i = 0; i < paths.count; ++i) {
    NSString* p = [paths[i] isKindOfClass:[NSString class]] ? paths[i] : @"";
    keep.emplace_back(p.UTF8String ?: "");
    NSNumber* m = i < moments.count && [moments[i] isKindOfClass:[NSNumber class]] ? moments[i] : nil;
    ms.push_back(m ? m.longLongValue : -1);
  }
  for (const std::string& k : keep) ptrs.push_back(k.c_str());
  const bool ok = mv_chrome_open_list(title.UTF8String ?: "", ptrs.data(), ms.data(),
                                      static_cast<int32_t>(ptrs.size()), select, gallery);
  return ok ? @YES : @NO;
}

- (void)closeList {
  mv_chrome_close_list();
}

// What find-similar and N / Shift+N need: the item on the canvas, the folder
// (or result list) around it, and the clip's transport.
- (NSDictionary*)viewerState {
  const std::string path = read_bridge(&mv_chrome_current_item_path);
  const std::string folder = read_bridge(&mv_chrome_current_folder);
  const std::string title = read_bridge(&mv_chrome_list_title);
  int64_t position = 0, duration = 0;
  bool playing = false, muted = false;
  int32_t rate = 100;
  float volume = 1.0f;
  const bool clip_live = mv_chrome_video_status(&position, &duration, &playing, &rate, &muted, &volume);
  return @{
    @"path" : ns(path),
    @"folder" : ns(folder),
    @"list_open" : @(mv_chrome_list_open()),
    @"list_title" : ns(title),
    @"video" : @(mv::shell::is_video_name(path)),
    @"clip_live" : @(clip_live),
    @"position_ms" : @(position),
    @"duration_ms" : @(duration),
    @"playing" : @(playing),
  };
}

// An exact seek on the clip on screen; it stays paused if it was paused.
- (void)seekTo:(NSNumber*)ms {
  if (![ms respondsToSelector:@selector(longLongValue)]) return;
  mv_chrome_video_seek(ms.longLongValue, true);
}

// {"path": String, "ms": [Number], "current": Number}; an empty "ms" clears.
- (void)setMarkers:(NSDictionary*)request {
  NSString* path = [request[@"path"] isKindOfClass:[NSString class]] ? request[@"path"] : @"";
  NSArray* list = [request[@"ms"] isKindOfClass:[NSArray class]] ? request[@"ms"] : @[];
  const int32_t current = [request[@"current"] respondsToSelector:@selector(intValue)]
                              ? [request[@"current"] intValue]
                              : -1;
  std::vector<int64_t> ms;
  ms.reserve(list.count);
  for (id v in list) {
    if ([v isKindOfClass:[NSNumber class]]) ms.push_back([v longLongValue]);
  }
  mv_chrome_set_scrub_markers(path.UTF8String ?: "", ms.data(), static_cast<int32_t>(ms.size()),
                              current);
}

// The main window, for centring the search panel over it.
- (NSWindow*)viewerWindow {
  NSWindow* main = NSApp.mainWindow;
  if (main) return main;
  for (NSWindow* w in NSApp.windows) {
    if (w.isVisible && [w.contentView isKindOfClass:[NSView class]] && !w.isSheet &&
        ![w isKindOfClass:[NSPanel class]]) {
      return w;
    }
  }
  return nil;
}
@end

void MvAddonsStart(MvAddonsOpenPathFn open_path, void* ctx) {
  mac_addons& s = state();
  s.open_path = open_path;
  s.open_path_ctx = ctx;
  // Verifying hashes every file: not on the main thread (rule 1).
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    const bool ok = import_installed_ok();
    const bool ai = ai_installed_ok();
    dispatch_async(dispatch_get_main_queue(), ^{
      if (ok) (void)load_import();
      if (ai) (void)start_ai_load();
    });
  });
  // The one-time hint: a removable volume mounts while Import is absent.
  s.mount_observer = [[[NSWorkspace sharedWorkspace] notificationCenter]
      addObserverForName:NSWorkspaceDidMountNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                mac_addons& st = state();
                if (st.chrome) return;  // Import itself watches volumes
                NSURL* url = note.userInfo[NSWorkspaceVolumeURLKey];
                NSNumber* removable = nil;
                [url getResourceValue:&removable forKey:NSURLVolumeIsRemovableKey error:nil];
                NSNumber* ejectable = nil;
                [url getResourceValue:&ejectable forKey:NSURLVolumeIsEjectableKey error:nil];
                if (!removable.boolValue && !ejectable.boolValue) return;
                const std::string marker = hint_marker();
                if (!marker.empty() && mv::io::stat_path(marker)) return;
                st.hint_pending = true;
              }];
}

void MvAddonsOpenImport(const std::vector<std::string>& marks) {
  mac_addons& s = state();
  if (!s.chrome) return;
  NSMutableArray<NSString*>* list = [NSMutableArray arrayWithCapacity:marks.size()];
  for (const std::string& m : marks) [list addObject:[NSString stringWithUTF8String:m.c_str()]];
  [s.chrome openWithSource:@"" marks:list];
}

void MvAddonsImportNow(const std::vector<std::string>& paths) {
  mac_addons& s = state();
  if (!s.chrome || paths.empty()) return;
  mv::json::writer w;
  w.begin_array();
  for (const std::string& p : paths) w.string(p);
  w.end_array();
  [s.chrome importNow:[NSString stringWithUTF8String:w.str().c_str()]];
}

void MvAddonsFolderOpened(const std::string& dir) {
  mac_addons& s = state();
  s.folder = dir;
  id<MVAIChrome> chrome = ai_chrome();
  if (chrome && [chrome respondsToSelector:@selector(folderChanged:)]) {
    [chrome folderChanged:ns(dir)];
  }
}

void MvAddonsItemChanged(const std::string& path) {
  id<MVAIChrome> chrome = ai_chrome();
  if (chrome && [chrome respondsToSelector:@selector(itemChanged:)]) [chrome itemChanged:ns(path)];
}

bool MvAddonsRunCommand(const char* name) {
  id<MVAIChrome> chrome = ai_chrome();
  if (!name || !chrome || ![chrome respondsToSelector:@selector(runCommand:)]) return false;
  @try {
    return [chrome runCommand:[NSString stringWithUTF8String:name]] == YES;
  } @catch (NSException*) {
    return false;
  }
}

// ---- the Swift Settings bridge (mv_chrome_bridge.h) --------------------------

namespace {
int32_t copy_out(const std::string& text, char* buf, int32_t size) {
  if (buf && size > 0) {
    const std::size_t n = std::min<std::size_t>(text.size(), static_cast<std::size_t>(size - 1));
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int32_t>(text.size());
}

const char* state_name(mv::addon::install_state st) {
  return st == mv::addon::install_state::ok             ? "ok"
         : st == mv::addon::install_state::needs_update ? "needs_update"
                                                        : "invalid";
}
}  // namespace

extern "C" int32_t mv_addons_state_json(char* buf, int32_t size) {
  // [worker-thread]: hashes every installed file.
  mv::json::writer w;
  w.begin_object();
  bool installed = false;
  if (ensure_store()) {
    if (auto found = state().store->find("import")) {
      installed = true;
      w.key("version").string(found->version);
      w.key("size").integer(static_cast<std::int64_t>(found->size));
      w.key("state").string(state_name(found->state));
    }
  }
  w.key("installed").boolean(installed);
  w.end_object();
  return copy_out(w.str(), buf, size);
}

extern "C" int32_t mv_addons_check_manifest(const uint8_t* manifest, int32_t manifest_len,
                                            const uint8_t* sig, int32_t sig_len, char* buf,
                                            int32_t size) {
  const auto d = mv::addon::check_manifest(
      std::span<const std::uint8_t>(manifest, manifest ? static_cast<std::size_t>(manifest_len) : 0),
      std::span<const std::uint8_t>(sig, sig ? static_cast<std::size_t>(sig_len) : 0),
      mv::addon::pinned_public_key(), MV_ADDON_HOST_API);
  mv::json::writer w;
  w.begin_object();
  w.key("ok").boolean(d.trusted());
  w.key("why").string(mv::addon::rejection_name(d.why));
  if (d.trusted()) {
    w.key("version").string(d.m.version);
    // Milestone H (additive; Import's Swift ignores them): which add-on or
    // piece this is, and what it will use on disk ("uses ~N GB", the 3 GB rule).
    w.key("id").string(d.m.id);
    w.key("part_of").string(d.m.part_of);
    w.key("installed_size").integer(static_cast<std::int64_t>(d.m.installed_size));
    w.key("archive").begin_object();
    w.key("path").string(d.m.archive.path);
    w.key("sha256").string(d.m.archive.sha256);
    w.key("size").integer(static_cast<std::int64_t>(d.m.archive.size));
    w.end_object();
  }
  w.end_object();
  return copy_out(w.str(), buf, size);
}

extern "C" int32_t mv_addons_sha256(const char* path, char* buf, int32_t size) {
  return copy_out(path ? mv::addon::sha256_file(path) : std::string(), buf, size);
}

extern "C" int32_t mv_addons_make_staging(char* buf, int32_t size) {
  if (!ensure_store()) return copy_out({}, buf, size);
  auto dir = state().store->make_staging();
  return copy_out(dir ? *dir : std::string(), buf, size);
}

extern "C" bool mv_addons_install(const char* staged_dir) {
  if (!staged_dir || !ensure_store()) return false;
  return state().store->install(staged_dir).has_value();
}

extern "C" bool mv_addons_load(void) { return load_import(); }

extern "C" bool mv_addons_remove(bool keep_data) {
  unload_import();
  return ensure_store() && state().store->remove("import", keep_data).has_value();
}

extern "C" bool mv_addons_loaded(void) { return state().chrome != nil; }

extern "C" void mv_addons_open_import(void) { MvAddonsOpenImport({}); }

extern "C" int32_t mv_addons_status(char* buf, int32_t size) { return copy_out(state().status, buf, size); }

extern "C" bool mv_addons_hint_pending(void) { return state().hint_pending; }

extern "C" void mv_addons_hint_done(bool never_again) {
  mac_addons& s = state();
  s.hint_pending = false;
  if (!never_again) return;
  const std::string marker = hint_marker();
  if (marker.empty()) return;
  mv::io::file_writer w;
  if (auto made = w.create_new(marker); made) (void)w.close();
}

// ---- Milestone H: id-parameterised management (mv_chrome_bridge.h) ------------

extern "C" bool mv_addon2_supported(const char* id) {
  if (!id) return false;
  const std::string s(id);
  if (s == "import") return true;
  if (s == "ai-cuda") return false;  // a Windows NVIDIA piece; never offered here
  return is_ai_family(s) && ai_supported();
}

extern "C" int32_t mv_addon2_state_json(const char* id, char* buf, int32_t size) {
  // [worker-thread]: hashes every installed file of that add-on.
  mv::json::writer w;
  w.begin_object();
  const std::string want = id ? id : "";
  const bool supported = mv_addon2_supported(want.c_str());
  w.key("id").string(want);
  w.key("supported").boolean(supported);
  bool installed = false;
  if (supported && ensure_store()) {
    if (auto found = state().store->find(want)) {
      installed = true;
      w.key("version").string(found->version);
      w.key("size").integer(static_cast<std::int64_t>(found->size));
      w.key("state").string(state_name(found->state));
      w.key("why").string(mv::addon::rejection_name(found->why));
    }
  }
  w.key("installed").boolean(installed);
  w.end_object();
  return copy_out(w.str(), buf, size);
}

// [main-thread] Cheap: no file is read.
extern "C" bool mv_addon2_loaded(const char* id) {
  if (!id) return false;
  const std::string s(id);
  if (s == "import") return state().chrome != nil;
  if (s == "ai") return state().ai.chrome != nil;
  return false;
}

extern "C" bool mv_addon2_loading(const char* id) {
  return id && std::string(id) == "ai" && state().ai.loading;
}

// Why the last load failed: "" none, else a status_name ("CORRUPT",
// "UNSUPPORTED_FORMAT" = needs an update) or "bundle" / "chrome" / "interface".
extern "C" int32_t mv_addon2_load_error(const char* id, char* buf, int32_t size) {
  const bool ai = id && std::string(id) == "ai";
  return copy_out(ai ? state().ai.error : std::string(), buf, size);
}

// [main-thread] Import loads at once (as mv_addons_load); the AI pack is
// verified and started on a worker and attaches later: poll mv_addon2_loaded /
// mv_addon2_loading. A piece is not loaded by itself.
extern "C" bool mv_addon2_load(const char* id) {
  if (!id) return false;
  const std::string s(id);
  if (s == "import") return load_import();
  if (s == "ai") return start_ai_load();
  return false;
}

// [main-thread] A piece of the loaded AI pack was installed or removed: the
// pack re-reads its pieces (mv.ai.1 set_setting "reload"); People is picked up
// at once, nothing restarts. With the pack not loaded, this loads it.
extern "C" bool mv_addon2_reload(const char* id) {
  if (!id || std::string(id) != "ai") return false;
  mac_addons& s = state();
  if (s.ai.chrome) {
    const auto* api = static_cast<const mv_ai_api*>(s.ai.table);
    return api && api->set_setting && api->set_setting(api->ctx, "reload", "1") == MV_OK;
  }
  if (s.ai.loading) return true;  // the load in flight reads the pieces as they are now
  return ai_installed_ok() && start_ai_load();
}

// [main-thread] Unloads first. Removing Core removes its pieces too (a piece is
// nothing without it); keep_data = true keeps the search index. Removing a
// piece of a loaded pack tells the pack to re-read its pieces ("reload").
extern "C" bool mv_addon2_remove(const char* id, bool keep_data) {
  if (!id || !ensure_store()) return false;
  const std::string s(id);
  mv::addon::store& store = *state().store;
  if (s == "import") return mv_addons_remove(keep_data);
  if (s == "ai") {
    unload_ai();
    bool ok = store.remove("ai", keep_data).has_value();
    if (auto faces = store.find("ai-faces"); faces) ok = store.remove("ai-faces", false).has_value() && ok;
    return ok;
  }
  if (s == "ai-faces") {
    const bool ok = store.remove(s, keep_data).has_value();
    if (state().ai.chrome) (void)mv_addon2_reload("ai");
    return ok;
  }
  return false;
}

// [worker-thread] A family's installed bytes and its ceiling (plan/17: 3 GB
// for "ai"); the Swift side refuses a piece that would not fit before it
// downloads anything, and install refuses it again.
extern "C" bool mv_addon2_family_usage(const char* family, uint64_t* used, uint64_t* ceiling) {
  if (!family || !used || !ceiling || !ensure_store()) return false;
  const auto room = state().store->family_usage(family);
  *used = room.used;
  *ceiling = room.ceiling;
  return true;
}

// [main-thread] The chrome's management view for Settings, or NULL. The
// chrome keeps it alive; the caller does not release it.
extern "C" void* mv_addon2_settings_view(const char* id) {
  if (!id || std::string(id) != "ai") return nullptr;
  id<MVAIChrome> chrome = ai_chrome();
  if (!chrome || ![chrome respondsToSelector:@selector(settingsView)]) return nullptr;
  NSView* view = [chrome settingsView];
  return (__bridge void*)view;
}

// [main-thread] Runs an add-on command as its key would (the command bar's
// indexing pill opens the search panel).
extern "C" bool mv_addon2_run_command(const char* name) { return MvAddonsRunCommand(name); }

// [main-thread][no-block] The command bar's indexing pill: mv.ai.1 status,
// read directly (it never blocks).
extern "C" bool mv_addon2_ai_status(mv_chrome_ai_status* out) {
  if (!out) return false;
  std::memset(out, 0, sizeof(*out));
  const auto* api = static_cast<const mv_ai_api*>(state().ai.table);
  if (!api || !api->status) return false;
  mv_ai_status st{};
  st.struct_size = sizeof(st);
  if (api->status(api->ctx, &st) != MV_OK) return false;
  out->state = static_cast<int32_t>(st.state);
  out->yield_reason = static_cast<int32_t>(st.yield_reason);
  out->backend = static_cast<int32_t>(st.backend);
  out->provider_fault = static_cast<int32_t>(st.provider_fault);
  out->flags = st.flags;
  out->assets_total = st.assets_total;
  out->assets_done = st.assets_done;
  out->frames_indexed = st.frames_indexed;
  out->eta_low_seconds = st.eta_low_seconds;
  out->eta_high_seconds = st.eta_high_seconds;
  return true;
}
