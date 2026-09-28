// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "abi/addon_media.h"
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
#include "shell/write_guard.h"

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
// Quit (2026-09-27): -shutdown without its bounded wait for table calls in
// flight. YES when none was in flight, so the pack may stop; NO leaves the
// pack running for the exit (a read inside it must not see it freed). A chrome
// without it gets -shutdown.
- (BOOL)shutdownForQuit;
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
  // Made once, on whichever thread asks first (MvAddonsStart's utility block
  // and Swift's detached state reads race at launch), and never replaced. The
  // store keeps no state of its own beyond its root and key, so its const
  // calls are safe from any thread once it exists.
  std::mutex store_mutex;
  std::unique_ptr<mv::addon::store> store;
  // install / remove move and delete add-on folders: one at a time.
  std::mutex store_writes;
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

// Never destroyed: at quit a Swift state read may still be hashing through the
// store, and a pack left running may still ask for a thumbnail, while the
// process exits.
mac_addons& state() {
  static mac_addons* const s = new mac_addons;
  return *s;
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

bool is_ai_family(const std::string& id) {
  return id == "ai" || id == "ai-faces" || id == "ai-audio";
}

std::string hint_marker() {
  auto dir = mv::io::addons_dir();
  return dir ? mv::io::join_path(*dir, ".import-hint-dismissed") : std::string();
}

bool ensure_store() {
  mac_addons& s = state();
  std::lock_guard<std::mutex> lock(s.store_mutex);
  if (s.store) return true;
  auto root = mv::io::addons_dir();
  if (!root) return false;
  auto key = mv::addon::pinned_public_key();
  s.store = std::make_unique<mv::addon::store>(*root, std::vector<std::uint8_t>(key.begin(), key.end()),
                                               MV_ADDON_HOST_API);
  s.store->startup_cleanup();  // under the lock: nobody holds the store before it has run
  return true;
}

// Where an AI pack is loaded, shut down and removed, in the order asked: a
// removal queued behind an unload runs only once the pack's workers have
// stopped and its dylib is gone, and a new load never overlaps the old pack's
// teardown. Utility QoS, never the main thread (rule 1).
dispatch_queue_t addon_queue() {
  static dispatch_queue_t q = dispatch_queue_create(
      "MediaViewer.addons",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  return q;
}

// A loaded_addon's destructor calls the pack's shutdown, which joins its
// workers, then unloads the dylib: off the main thread. The chrome, if any,
// must already have been shut down.
// Set by MvAddonsQuit (main thread). A load that lands after it is stopped
// the quit's way, not queued behind whatever addon_queue() still holds.
std::atomic<bool> g_quitting{false};

void retire(std::unique_ptr<mv::addon::loaded_addon> addon) {
  if (!addon) return;
  if (g_quitting.load(std::memory_order_relaxed)) {
    mv::addon::stop_for_exit(std::move(addon));
    return;
  }
  mv::addon::loaded_addon* raw = addon.release();
  dispatch_async(addon_queue(), ^{
    std::unique_ptr<mv::addon::loaded_addon> owned(raw);
  });
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
  // Stills only: a clip's poster needs the player, and reading a whole clip
  // to find that out would cost gigabytes (Windows' twin says the same).
  if (mv::shell::is_video_name(path)) return mv::err(mv::status::unsupported_format);
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
// its JPEG-512 cache (src/abi/addon_media.h), the same set abi/addon_abi.cpp's
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
  svc.open_audio = &mv::addon::media::open_audio;
  // piece_dir is left empty: loaded_addon::load serves the family's own
  // verified pieces from the store.
  return svc;
}

// [main-thread] The chrome shuts down here, first: it stops taking table calls
// and waits for those in flight, so nothing reaches the table after this. Only
// then is the pack retired on addon_queue() (its workers stop, indexing
// resumes next load, then the dylib goes) -- joining them here would hold the
// main thread.
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
  retire(std::move(s.ai.addon));
  s.ai.bundle = nil;   // mapped until quit, inert (NSBundle -unload is unsafe for Swift)
  s.ai.loading = false;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::ai, false);
  // Nothing may point into the gone table's clip: drop the markers.
  mv_chrome_set_scrub_markers(nullptr, nullptr, 0, -1);
}

// On the main thread, once the native side loaded on a worker: the bundle,
// its principal class, attach. Takes `loaded` only on success; on failure the
// caller retires it (no chrome was attached to it).
void attach_ai(std::unique_ptr<mv::addon::loaded_addon>& loaded) {
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
  dispatch_async(addon_queue(), ^{
    auto loaded = mv::addon::loaded_addon::load(*store, "ai", ai_services());
    const std::string why = loaded ? std::string()
                                   : std::string(mv::status_name(loaded.error()));
    // A unique_ptr cannot ride in a block; hand it over as a raw pointer the
    // main thread takes back.
    mv::addon::loaded_addon* raw = loaded ? loaded->release() : nullptr;
    dispatch_async(dispatch_get_main_queue(), ^{
      std::unique_ptr<mv::addon::loaded_addon> owned(raw);
      mac_addons& st = state();
      if (seq != st.ai.load_seq) {
        retire(std::move(owned));  // removed or unloaded meanwhile
        return;
      }
      st.ai.loading = false;
      if (!owned) {
        st.ai.error = why.empty() ? "load" : why;
        return;
      }
      attach_ai(owned);
      retire(std::move(owned));  // attach failed: never on the main thread
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
  // Photos library files (issue #72): viewed in place or as previews, never
  // written (shell/write_guard.h). A list without them clears the set.
  std::vector<std::string> read_only;
  if (NSArray* ro = [request[@"readOnly"] isKindOfClass:[NSArray class]] ? request[@"readOnly"] : nil) {
    for (id p in ro) {
      if ([p isKindOfClass:[NSString class]]) read_only.emplace_back([(NSString*)p UTF8String] ?: "");
    }
  }
  mv::shell::set_read_only_paths(read_only);
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

// An exact seek on the clip on screen; it stays paused if it was paused. While
// the selected clip is still opening (N pressed twice quickly) the live clip
// is the previous one: the seek is dropped, and the new clip opens on its
// own moment.
- (void)seekTo:(NSNumber*)ms {
  if (![ms respondsToSelector:@selector(longLongValue)]) return;
  (void)MvViewerSeekShownClip(ms.longLongValue);
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

namespace {

std::chrono::steady_clock::time_point g_quit_at;
// Set when MvAddonsWaitStopped's time ran out with add-on work still going.
std::atomic<bool> g_exit_fast{false};

// Registered by MvAddonsQuit, so it runs before every static destructor that
// was registered earlier (all of them: the add-ons were loaded before quit).
// A pack still stopping, a pack left running, or a verify / removal still on
// the add-on queue may be using what those destructors free.
void exit_guard() {
  if (!g_exit_fast.load(std::memory_order_acquire) && !mv::addon::running_at_exit()) return;
  std::fflush(nullptr);
  _exit(0);
}

}  // namespace

void MvAddonsQuit() {
  mac_addons& s = state();
  g_quitting.store(true, std::memory_order_relaxed);
  g_quit_at = std::chrono::steady_clock::now();
  static bool guarded = false;
  if (!guarded) guarded = std::atexit(&exit_guard) == 0;
  if (s.chrome) {
    @try {
      [s.chrome shutdown];  // Import's chrome waits on nothing
    } @catch (NSException*) {
    }
  }
  s.chrome = nil;
  mv::addon::stop_for_exit(std::move(s.import));

  // The AI pack: its chrome closes the table first. A read still inside the
  // pack (Settings' roots or people, a result thumbnail) would be freed under
  // it by a stop, so then the pack is left running for the exit instead.
  ++s.ai.load_seq;  // a load in flight lands on nothing (retire, the quit's way)
  bool idle = true;
  if (s.ai.chrome) {
    id<MVAIChrome> chrome = (id<MVAIChrome>)s.ai.chrome;
    @try {
      if ([chrome respondsToSelector:@selector(shutdownForQuit)]) {
        idle = [chrome shutdownForQuit] == YES;
      } else {
        [chrome shutdown];  // an older chrome: its own bounded wait
      }
    } @catch (NSException*) {
      idle = false;
    }
  }
  s.ai.chrome = nil;
  s.ai.table = nullptr;
  s.ai.bundle = nil;
  s.ai.loading = false;
  if (idle) {
    mv::addon::stop_for_exit(std::move(s.ai.addon));
  } else {
    mv::addon::abandon_for_exit(std::move(s.ai.addon));
  }
}

void MvAddonsWaitStopped(double seconds) {
  const auto deadline =
      g_quit_at + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
  // addon_queue() is serial: the marker runs once everything queued before it
  // (a verify hashing the pack, a removal deleting it) has finished.
  dispatch_semaphore_t drained = dispatch_semaphore_create(0);
  dispatch_async(addon_queue(), ^{
    dispatch_semaphore_signal(drained);
  });
  const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now());
  const bool queue_idle =
      dispatch_semaphore_wait(drained, dispatch_time(DISPATCH_TIME_NOW, std::max<std::int64_t>(0, left.count()))) == 0;
  const bool stopped = mv::addon::wait_stopped_for_exit(deadline);
  if (!queue_idle || !stopped) g_exit_fast.store(true, std::memory_order_release);
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
  std::lock_guard<std::mutex> lock(state().store_writes);
  return state().store->install(staged_dir).has_value();
}

extern "C" bool mv_addons_load(void) { return load_import(); }

extern "C" bool mv_addons_remove(bool keep_data) {
  unload_import();
  // Not under store_writes: this runs on the main thread, and an install in
  // flight holds that lock while it hashes a download.
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
// at once, nothing restarts. With the pack not loaded, this loads it if Core
// verifies -- checked on addon_queue() (it hashes Core), so true means only
// that the check was queued.
extern "C" bool mv_addon2_reload(const char* id) {
  if (!id || std::string(id) != "ai") return false;
  mac_addons& s = state();
  if (s.ai.chrome) {
    const auto* api = static_cast<const mv_ai_api*>(s.ai.table);
    return api && api->set_setting && api->set_setting(api->ctx, "reload", "1") == MV_OK;
  }
  if (s.ai.loading) return true;  // the load in flight reads the pieces as they are now
  if (!ai_supported()) return false;
  dispatch_async(addon_queue(), ^{
    if (!ai_installed_ok()) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      (void)start_ai_load();
    });
  });
  return true;
}

// [main-thread] Unloads first. Removing Core removes its pieces too (a piece is
// nothing without it); keep_data = true keeps the search index. Removing a
// piece of a loaded pack tells the pack to re-read its pieces ("reload").
// For the AI family nothing here waits: the chrome shuts down on the main
// thread, and the pack's shutdown (joining its workers) and the deletion (up to
// 3 GB) run in that order on addon_queue(). True means the removal was queued;
// the Settings view sees it land through mv_addon2_state_json, which it polls.
// Import's removal is still synchronous (mv_addons_remove).
extern "C" bool mv_addon2_remove(const char* id, bool keep_data) {
  if (!id) return false;
  const std::string s(id);
  if (s == "import") return mv_addons_remove(keep_data);
  if (s != "ai" && s != "ai-faces" && s != "ai-audio") return false;
  if (s == "ai") unload_ai();  // queues the pack's shutdown ahead of the removal below
  dispatch_async(addon_queue(), ^{
    if (!ensure_store()) return;
    mac_addons& st = state();
    std::lock_guard<std::mutex> lock(st.store_writes);
    if (s == "ai") {
      (void)st.store->remove("ai", keep_data);
      // A piece that is not installed answers invalid_arg: remove() matches on
      // the manifests' ids without hashing anything, so no find() first.
      for (const char* piece : {"ai-faces", "ai-audio"}) (void)st.store->remove(piece, false);
      return;
    }
    (void)st.store->remove(s, keep_data);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (state().ai.chrome) (void)mv_addon2_reload("ai");
    });
  });
  return true;
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
extern "C" void* mv_addon2_settings_view(const char* addon_id) {
  if (!addon_id || std::string(addon_id) != "ai") return nullptr;
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
  // Photos library assets only iCloud has are handled too (issue #72): there
  // is nothing on this Mac to read. Zero from an older pack.
  out->assets_done = st.assets_done + st.assets_unavailable;
  out->frames_indexed = st.frames_indexed;
  out->eta_low_seconds = st.eta_low_seconds;
  out->eta_high_seconds = st.eta_high_seconds;
  // Zero from a pack older than the audio fields (it fills only its struct_size).
  out->sound_total = st.sound_total;
  out->sound_done = st.sound_done;
  out->speech_total = st.speech_total;
  out->speech_done = st.speech_done;
  return true;
}
