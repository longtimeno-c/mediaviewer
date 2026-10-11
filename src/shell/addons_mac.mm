// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac host's add-ons (docs/design/18 "Add-ons"; the Windows twin is
// abi/addon_abi.cpp + IslandHost.Addons.cs). Owns the add-on store, the
// loaded add-ons and their chromes:
//
//   Import  (Milestone G)  Import.bundle, principal class MVImportChrome, which
//                          drives an NSWindow hosting the SwiftUI Import view.
//   AI      (Milestone H)  AI.bundle, principal class MVAIChrome: the search
//                          panel and the Settings -> Local search management
//                          view (docs/design/17). Pieces of its family ("ai-faces")
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
#include "addon/open_json.h"
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
@optional
// docs/design/25 (2026-10-03): a command the add-on's manifest contributed, by its
// own id ("open", "import_now"), with the payload its row asked for as JSON.
// Checked with -respondsToSelector:; an Import 1.0.0 bundle has only the two
// selectors above and is driven through them.
- (BOOL)runCommand:(NSString*)name payload:(NSString*)json;
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

// main_mac.mm: the command table changed; rebuild the router and let
// Settings and `?` re-read it. Main thread.
void MvAppCommandsChanged();

namespace {

void refresh_contributed_commands();

// One loaded add-on with a chrome.
struct addon_slot {
  std::unique_ptr<mv::addon::loaded_addon> addon;
  id chrome = nil;
  NSBundle* bundle = nil;
  const void* table = nullptr;
  bool loading = false;
  std::string error;       // why the last load failed ("" none); no path, ever
  std::string reason;      // the same, in words a person can act on (load_reason); no path
  std::uint64_t load_seq = 0;
  // A later window's reader (MvAddonsStartReader) is loaded lazily, on the
  // first ⌘F or when Settings opens. Once tried and not loaded (nothing
  // indexed yet, not installed, a failure) it is tried again at most every
  // kReaderRetry. `empty`: the last try found no index.db.
  bool tried = false;
  bool empty = false;
  bool absent = false;  // the last try found Local search not installed
  std::chrono::steady_clock::time_point tried_at{};
  // ⌘F started this load: the panel opens once it attaches (or file search
  // and the reason, if it does not).
  bool open_when_attached = false;
};

// An add-on that will not load or start, said in the window (the command bar's
// alert, AddonsView.swift AddonAlertItem) with why (owner, 2026-10-07: "show a
// user facing alert on any failures and why"). One at a time, the latest. The
// text never holds a path (rule 6).
struct addon_alert {
  std::uint64_t seq = 0;  // moves on every change, so the Swift poll reads it only then
  std::string addon;      // "ai" / "import"; "" none
  std::string title;
  std::string body;
  bool warn = true;       // false: a note (a later window's reader with nothing indexed)
  bool open = false;      // the person just asked (⌘F): open its popover at once
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
  // Another window's process holds Add-ons/.host.lock (main_mac.mm
  // MvClaimAddonHost): this one loads the AI pack through its read-only door
  // only, indexes nothing, loads no Import, installs or removes nothing, and
  // never runs the store's startup_cleanup (the host may be installing into
  // .staging). Set once, before the chrome exists.
  bool reader = false;
  std::string import_error;   // Import's last load failure, as ai.error
  std::string import_reason;  // and in words, as ai.reason
  addon_alert alert;          // [main-thread]
};

// Never destroyed: at quit a Swift state read may still be hashing through the
// store, and a pack left running may still ask for a thumbnail, while the
// process exits.
mac_addons& state() {
  static mac_addons* const s = new mac_addons;
  return *s;
}

// The Mac AI pack is arm64 only (docs/design/17, 2026-09-26): ONNX Runtime has no
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
  // Under the lock: nobody holds the store before it has run. Never in a later
  // window: the host window owns the store's removals and staging.
  if (!s.reader) s.store->startup_cleanup();
  return true;
}

// ---- why an add-on did not start, in words (the Windows twin is
// IslandHost.AddonAlert.cs AddonLoadReason; both say the same) ----------------

constexpr const char* kAiTitle = "Local search couldn't start";
constexpr const char* kImportTitle = "Import couldn't start";
constexpr const char* kReaderEmptyTitle = "Nothing indexed for Local search yet";
constexpr const char* kReaderEmptyText =
    "Local search has nothing indexed yet. Index a folder from the first MediaViewer window you opened; "
    "search then works here too.";
constexpr auto kReaderRetry = std::chrono::seconds(10);

// `section`: where in Settings it is managed ("Local search", "Add-ons").
// `detail`: shared_library::last_error() for status::io, captured on the
// thread that loaded (it is thread-local); path-free by construction.
std::string load_reason(mv::status st, const std::string& detail, const char* section) {
  const std::string where = std::string("Settings \u2192 ") + section;
  switch (st) {
    case mv::status::unsupported_format:
      return "It needs an update to work with this MediaViewer. Update it in " + where + ".";
    case mv::status::corrupt:
      return "Its files changed since it was installed, so it was not loaded. Remove it and install it again in " +
             where + ".";
    case mv::status::io:
      if (!detail.empty()) return detail;
      break;
    case mv::status::out_of_memory:
      return "There is not enough memory to start it. Close other apps, then restart MediaViewer.";
    case mv::status::permission_denied:
      return "macOS denied access to its files. Security software may be blocking it.";
    default:
      break;
  }
  return std::string("Something went wrong (") + mv::status_name(st) + ").";
}

// Its native side loaded but its chrome (the bundle, its principal class, its
// interface) did not: `category` is which, never an NSError text (a path).
std::string chrome_reason(const char* category, const char* section) {
  return std::string("Its window could not start (") + category +
         "). Restart MediaViewer; if it happens again, remove it and install it again in Settings \u2192 " + section +
         ".";
}

void raise_alert(const char* addon, std::string title, std::string body, bool warn, bool open) {
  addon_alert& a = state().alert;
  ++a.seq;
  a.addon = addon;
  a.title = std::move(title);
  a.body = std::move(body);
  a.warn = warn;
  a.open = open;
}

// It loaded after all: its alert goes.
void clear_alert(const char* addon) {
  addon_alert& a = state().alert;
  if (a.addon != addon) return;
  const std::uint64_t seq = a.seq + 1;
  a = addon_alert{};
  a.seq = seq;
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
  // until quit, inert (docs/design/18: removal completes at next start).
  s.bundle = nil;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::import, false);
  refresh_contributed_commands();
}

// docs/design/25: the rows the loaded add-ons' manifests contribute, into the
// command table (shell/commands.h set_addon_commands). An add-on with rows of
// its own supersedes the rows the table has built in for it; one without
// (an older Import) keeps them. Then the router and Settings re-read the
// table (main_mac.mm MvAppCommandsChanged).
void refresh_contributed_commands() {
  std::vector<mv::shell::addon_command_row> rows;
  const auto add = [&rows](const mv::addon::loaded_addon* addon) {
    if (!addon) return;
    const mv::addon::manifest& m = addon->info().m;
    for (const mv::addon::manifest_command& c : m.commands) {
      mv::shell::addon_command_row r;
      r.addon = m.id;
      r.id = c.id;
      r.name = c.name;
      if (!c.mac.empty() && !mv::shell::parse_key_label(c.mac, r.k, r.mods)) {
        r.k = mv::shell::key::none;  // listed, Settings may give it a key
        r.mods = 0;
      }
      r.modes = mv::shell::parse_modes(c.modes);
      r.payload = c.payload;
      rows.push_back(std::move(r));
    }
  };
  const mac_addons& s = state();
  add(s.import.get());
  mv::shell::set_addon_commands(rows);
  MvAppCommandsChanged();
}

bool load_import_attempt();

// Every failure says why: Settings' line (mv_addon2_load_reason) and the
// command bar's alert.
bool load_import() {
  mac_addons& s = state();
  if (s.chrome) return true;
  if (s.reader) return false;  // a later window loads no Import
  if (load_import_attempt()) {
    s.import_error.clear();
    s.import_reason.clear();
    clear_alert("import");
    return true;
  }
  if (!s.import_reason.empty()) raise_alert("import", kImportTitle, s.import_reason, true, false);
  return false;
}

bool load_import_attempt() {
  mac_addons& s = state();
  s.import_error.clear();
  s.import_reason.clear();
  if (!ensure_store()) return false;
  const auto fail_chrome = [&s](const char* category) {
    s.import_error = category;
    s.import_reason = chrome_reason(category, "Add-ons");
    return false;
  };
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
  // Find duplicates (PR 54): the Trash, never a permanent delete. Called on
  // the add-on's I/O thread. A volume with no Trash (a network share, some
  // removable drives) answers NSFeatureUnsupportedError and keeps the file.
  svc.recycle = [](const std::string& path) -> mv::result<bool> {
    @autoreleasepool {
      NSString* ns = [NSString stringWithUTF8String:path.c_str()];
      if (!ns) return mv::err(mv::status::invalid_arg);
      NSURL* url = [NSURL fileURLWithPath:ns];
      NSError* error = nil;
      if ([[[NSFileManager alloc] init] trashItemAtURL:url resultingItemURL:nil error:&error]) {
        return true;
      }
      if (error && [error.domain isEqualToString:NSCocoaErrorDomain] &&
          error.code == NSFeatureUnsupportedError) {
        return false;
      }
      return mv::err(mv::status::io);
    }
  };
  auto loaded = mv::addon::loaded_addon::load(*s.store, "import", std::move(svc));
  if (!loaded) {
    s.import_error = mv::status_name(loaded.error());
    s.import_reason = load_reason(loaded.error(), mv::addon::shared_library::last_error(), "Add-ons");
    return false;
  }
  const auto* table = (*loaded)->query(MV_IMPORT_INTERFACE);
  if (!table) return fail_chrome("interface");

  const auto& info = (*loaded)->info();
  const std::string bundle_path = mv::io::join_path(info.dir, mv::io::native_relative(info.m.chrome));
  NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle_path.c_str()]];
  NSError* error = nil;
  if (!bundle || ![bundle loadAndReturnError:&error]) return fail_chrome("bundle");
  Class principal = bundle.principalClass;
  if (!principal) return fail_chrome("chrome");
  id<MVAddonChrome> chrome = [[principal alloc] init];
  if (![chrome respondsToSelector:@selector(attachWithTable:host:)]) return fail_chrome("chrome");
  if (!s.host) s.host = [[MvAddonHostMac alloc] init];
  [chrome attachWithTable:[NSValue valueWithPointer:table] host:s.host];
  s.import = std::move(*loaded);
  s.bundle = bundle;
  s.chrome = chrome;
  mv::shell::set_addon_commands_available(mv::shell::addon_family::import, true);
  refresh_contributed_commands();
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
  // Indexing waits while the present loop is busy (docs/design/17 "Yield policy":
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
  svc.thumbnail_jpeg = &mv::addon::media::thumbnail_jpeg;
  svc.store_thumbnail_jpeg = &mv::addon::media::store_thumbnail_jpeg;
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
bool attach_ai(std::unique_ptr<mv::addon::loaded_addon>& loaded) {
  mac_addons& s = state();
  const void* table = loaded->query(MV_AI_INTERFACE);
  if (!table) {
    s.ai.error = "interface";
    return false;
  }
  const auto& info = loaded->info();
  const std::string bundle_path = mv::io::join_path(info.dir, mv::io::native_relative(info.m.chrome));
  NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle_path.c_str()]];
  NSError* error = nil;
  if (!bundle || ![bundle loadAndReturnError:&error]) {
    // Library validation refuses a bundle signed by another team here.
    s.ai.error = "bundle";
    return false;
  }
  Class principal = bundle.principalClass;
  id chrome = principal ? [[principal alloc] init] : nil;
  if (!chrome || ![chrome respondsToSelector:@selector(attachWithTable:host:)] ||
      ![chrome respondsToSelector:@selector(deliverEvent:status:identifier:payload:)]) {
    s.ai.error = "chrome";
    return false;
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
  return true;
}

// Verifying hashes every file of a pack that can be gigabytes of models, and
// the pack starts its workers: all of it on a utility queue (rule 1). Only the
// NSBundle load and attach come back to the main thread.
// In a later window (s.reader) the pack is loaded through its read-only door
// (MV_AI_READER_ENTRY_SYMBOL): the same mv.ai.1 table over index.db opened
// read-only, text towers only, no scans, catching up with the first window's
// commits every few seconds. status::not_found from it: nothing indexed yet.
bool start_ai_load() {
  mac_addons& s = state();
  if (!ai_supported()) return false;
  if (s.ai.chrome || s.ai.loading) return true;
  if (!ensure_store()) return false;
  s.ai.loading = true;
  s.ai.error.clear();
  s.ai.reason.clear();
  const std::uint64_t seq = ++s.ai.load_seq;
  mv::addon::store* store = s.store.get();  // lives until exit
  const bool reader = s.reader;
  dispatch_async(addon_queue(), ^{
    auto loaded = mv::addon::loaded_addon::load(*store, "ai", ai_services(),
                                                reader ? MV_AI_READER_ENTRY_SYMBOL : MV_ADDON_ENTRY_SYMBOL);
    const mv::status st = loaded ? mv::status::ok : loaded.error();
    // Thread-local: read here, on the thread that loaded.
    const std::string detail = loaded ? std::string() : mv::addon::shared_library::last_error();
    // A unique_ptr cannot ride in a block; hand it over as a raw pointer the
    // main thread takes back.
    mv::addon::loaded_addon* raw = loaded ? loaded->release() : nullptr;
    dispatch_async(dispatch_get_main_queue(), ^{
      std::unique_ptr<mv::addon::loaded_addon> owned(raw);
      mac_addons& sm = state();
      if (seq != sm.ai.load_seq) {
        retire(std::move(owned));  // removed or unloaded meanwhile
        return;
      }
      sm.ai.loading = false;
      const bool asked = sm.ai.open_when_attached;
      sm.ai.open_when_attached = false;
      sm.ai.tried_at = std::chrono::steady_clock::now();
      sm.ai.empty = false;
      sm.ai.absent = false;
      if (!owned) {
        if (reader && st == mv::status::invalid_arg) {
          // Not installed: ⌘F is what it is without Local search, silently.
          sm.ai.absent = true;
          if (asked) mv_chrome_file_search();
          return;
        }
        sm.ai.error = mv::status_name(st);
        if (reader && st == mv::status::not_found) {
          // Not a failure: the first window has indexed nothing yet.
          sm.ai.empty = true;
          sm.ai.reason = kReaderEmptyText;
          if (asked) {
            mv_chrome_file_search();  // as without Local search, and why
            raise_alert("ai", kReaderEmptyTitle, kReaderEmptyText, false, true);
          }
          return;
        }
        sm.ai.reason = load_reason(st, detail, "Local search");
        raise_alert("ai", kAiTitle, sm.ai.reason, true, asked);
        return;
      }
      if (attach_ai(owned)) {
        sm.ai.reason.clear();
        clear_alert("ai");
        if (asked) (void)MvAddonsRunCommand("search_open");
        return;
      }
      sm.ai.reason = chrome_reason(sm.ai.error.c_str(), "Local search");
      raise_alert("ai", kAiTitle, sm.ai.reason, true, asked);
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

void MvAddonsStartReader(MvAddonsOpenPathFn open_path, void* ctx) {
  mac_addons& s = state();
  s.reader = true;  // before anything makes the store: no startup_cleanup here
  s.open_path = open_path;
  s.open_path_ctx = ctx;
  // Nothing more at launch: reading the store hashes every installed file
  // (over a gigabyte with the AI pack) and the reader holds a text model, so
  // nothing may compete with this window's first pixel, and a window that
  // never searches costs nothing. The reader starts on the first ⌘F
  // (MvAddonsReaderSearch) or when Settings opens (mv_addon2_reader_start).
  // No card hint either: Import is not loaded or installed from this window.
}

namespace {
// Starts the reader's load if it is due: never tried, or tried and not loaded
// at least kReaderRetry ago. True when a load is now on its way.
bool reader_begin() {
  mac_addons& s = state();
  if (!s.reader || !ai_supported() || s.ai.chrome) return false;
  if (s.ai.loading) return true;
  if (s.ai.tried && std::chrono::steady_clock::now() - s.ai.tried_at < kReaderRetry) return false;
  s.ai.tried = true;
  s.ai.tried_at = std::chrono::steady_clock::now();
  return start_ai_load();
}
}  // namespace

bool MvAddonsReaderSearch() {
  mac_addons& s = state();
  if (!s.reader || s.ai.chrome) return false;
  if (reader_begin()) {
    s.ai.open_when_attached = true;  // the panel once it attaches, else file search
    return true;
  }
  // Too soon to look again: file search, with the note in the bar if that is why.
  if (s.ai.empty && s.alert.addon != "ai") raise_alert("ai", kReaderEmptyTitle, kReaderEmptyText, false, false);
  return false;
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

bool MvAddonsRunContributedCommand(const std::string& addon, const std::string& command,
                                   const std::string& payload_json) {
  mac_addons& s = state();
  if (addon != "import" || !s.chrome) return false;
  NSString* name = [NSString stringWithUTF8String:command.c_str()];
  NSString* json = [NSString stringWithUTF8String:payload_json.c_str()];
  if (!name || !json) return false;
  @try {
    if ([s.chrome respondsToSelector:@selector(runCommand:payload:)]) {
      return [(id)s.chrome runCommand:name payload:json] == YES;
    }
    // Import 1.0.0: the frozen selectors, fed what its manifest-less rows got.
    if (command == "open") {
      NSArray* marks = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
                                                       options:0
                                                         error:nil];
      NSMutableArray<NSString*>* list = [NSMutableArray array];
      for (NSObject* item in [marks isKindOfClass:[NSArray class]] ? marks : @[]) {
        if ([item isKindOfClass:[NSString class]]) [list addObject:(NSString*)item];
      }
      [s.chrome openWithSource:@"" marks:list];
      return true;
    }
    if (command == "import_now") {
      if ([json isEqualToString:@"[]"]) return false;
      [s.chrome importNow:json];
      return true;
    }
  } @catch (NSException*) {
    return false;
  }
  return false;
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
      // docs/design/25: Settings' line and the card hint, from the manifest.
      w.key("description").string(found->m.description);
      w.key("hint_on").string(found->m.hint_on);
      w.key("hint_text").string(found->m.hint_text);
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
    // docs/design/25: what the channel's add-on says about itself.
    w.key("name").string(d.m.name);
    w.key("description").string(d.m.description);
    w.key("hint_on").string(d.m.hint_on);
    w.key("hint_text").string(d.m.hint_text);
  }
  w.end_object();
  return copy_out(w.str(), buf, size);
}

// ---- open add-ons (docs/design/25) ---------------------------------------------------
// Data only: nothing below loads code, so none of it touches the loaded
// add-ons above. One writer at a time; reads need no lock (the store keeps no
// state beyond its root).

namespace {
std::mutex& open_writes() {
  static std::mutex* const m = new std::mutex;
  return *m;
}

mv::result<mv::addon::open_store> open_store_once() {
  auto s = mv::addon::default_open_store();
  if (s) {
    static std::once_flag cleaned;
    std::call_once(cleaned, [&] { s->startup_cleanup(); });
  }
  return s;
}
}  // namespace

extern "C" int32_t mv_open_addons_inspect(const char* package, char* buf, int32_t size) {
  auto s = open_store_once();
  if (!package || !s) return copy_out({}, buf, size);
  return copy_out(mv::addon::open_inspect_json(*s, package), buf, size);
}

extern "C" int32_t mv_open_addons_install(const char* package, const char* approved_sha256,
                                          char* buf, int32_t size) {
  auto s = open_store_once();
  if (!package || !approved_sha256 || !s) return copy_out({}, buf, size);
  // Without room for the answer the install is not attempted: the caller
  // could not learn what happened.
  if (!buf || size < 1024) return 1024;
  std::lock_guard<std::mutex> lock(open_writes());
  return copy_out(mv::addon::open_install_json(*s, package, approved_sha256), buf, size);
}

extern "C" int32_t mv_open_addons_list(char* buf, int32_t size) {
  auto s = open_store_once();
  return copy_out(s ? mv::addon::open_list_json(*s) : std::string("[]"), buf, size);
}

extern "C" bool mv_open_addons_remove(const char* folder) {
  auto s = open_store_once();
  if (!folder || !s) return false;
  std::lock_guard<std::mutex> lock(open_writes());
  return s->remove(folder).has_value();
}

extern "C" int32_t mv_open_addons_theme(const char* addon_id, const char* theme_id, char* buf,
                                        int32_t size) {
  auto s = open_store_once();
  if (!addon_id || !theme_id || !s) return copy_out({}, buf, size);
  auto theme = s->theme_json(addon_id, theme_id);
  return copy_out(theme ? *theme : std::string(), buf, size);
}

extern "C" int32_t mv_addons_sha256(const char* path, char* buf, int32_t size) {
  return copy_out(path ? mv::addon::sha256_file(path) : std::string(), buf, size);
}

extern "C" int32_t mv_addons_make_staging(char* buf, int32_t size) {
  if (state().reader || !ensure_store()) return copy_out({}, buf, size);
  auto dir = state().store->make_staging();
  return copy_out(dir ? *dir : std::string(), buf, size);
}

extern "C" bool mv_addons_install(const char* staged_dir) {
  if (!staged_dir || state().reader || !ensure_store()) return false;
  std::lock_guard<std::mutex> lock(state().store_writes);
  return state().store->install(staged_dir).has_value();
}

extern "C" bool mv_addons_load(void) { return load_import(); }

extern "C" bool mv_addons_remove(bool keep_data) {
  if (state().reader) return false;  // the first window manages add-ons
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
// "UNSUPPORTED_FORMAT" = needs an update, "NOT_FOUND" = a later window's
// reader with nothing indexed yet) or "bundle" / "chrome" / "interface".
extern "C" int32_t mv_addon2_load_error(const char* id, char* buf, int32_t size) {
  const std::string want = id ? id : "";
  const mac_addons& s = state();
  return copy_out(want == "ai" ? s.ai.error : want == "import" ? s.import_error : std::string(), buf, size);
}

// The same, in words a person can act on ("" none); never a path.
extern "C" int32_t mv_addon2_load_reason(const char* id, char* buf, int32_t size) {
  const std::string want = id ? id : "";
  const mac_addons& s = state();
  return copy_out(want == "ai" ? s.ai.reason : want == "import" ? s.import_reason : std::string(), buf, size);
}

// Another window's process hosts the add-ons: this one searches only.
extern "C" bool mv_addon2_elsewhere(void) { return state().reader; }

// Settings opened in a later window: start its reader if it is due (see
// reader_begin), so Local search's view can show. False when nothing started.
extern "C" bool mv_addon2_reader_start(void) { return reader_begin(); }

// The path bar's search icon in a later window: what ⌘F does (MvAddonsReaderSearch).
extern "C" bool mv_addon2_reader_search(void) { return MvAddonsReaderSearch(); }

// A later window's reader found Local search not installed at its last try.
extern "C" bool mv_addon2_reader_absent(void) { return state().ai.absent; }

extern "C" uint64_t mv_addon2_alert_seq(void) { return state().alert.seq; }

extern "C" int32_t mv_addon2_alert_json(char* buf, int32_t size) {
  const addon_alert& a = state().alert;
  mv::json::writer w;
  w.begin_object();
  w.key("seq").integer(static_cast<std::int64_t>(a.seq));
  w.key("addon").string(a.addon);
  w.key("title").string(a.title);
  w.key("body").string(a.body);
  w.key("warn").boolean(a.warn);
  w.key("open").boolean(a.open);
  w.end_object();
  return copy_out(w.str(), buf, size);
}

extern "C" void mv_addon2_alert_dismiss(uint64_t seq) {
  addon_alert& a = state().alert;
  if (a.seq != seq || a.addon.empty()) return;  // a newer one came meanwhile
  a = addon_alert{};
  a.seq = seq + 1;
}

// [main-thread] Import loads at once (as mv_addons_load); the AI pack is
// verified and started on a worker and attaches later: poll mv_addon2_loaded /
// mv_addon2_loading. A piece is not loaded by itself.
extern "C" bool mv_addon2_load(const char* id) {
  if (!id) return false;
  const std::string s(id);
  if (s == "import") return load_import();
  if (s == "ai") return state().reader ? reader_begin() : start_ai_load();
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
  if (!ai_supported() || s.reader) return false;  // a later window loads its reader on demand
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
  if (state().reader) return false;  // the first window manages add-ons
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

// [worker-thread] A family's installed bytes and its ceiling (docs/design/17: 3 GB
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
