// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac host's add-ons (plan/18 "Add-ons"; the Windows twin is
// abi/addon_abi.cpp + IslandHost.Addons.cs). Owns the add-on store, the
// loaded Import add-on, and its chrome: Import.bundle, loaded with NSBundle,
// whose principal class drives an NSWindow hosting the SwiftUI Import view.
//
// The app runs with the hardened runtime and library validation, so the
// add-on's dylib and bundle load only if signed by the same Team ID; the
// store re-verifies the signed manifest and every file before each load.
// Downloads happen in Swift (AddonsView.swift); this file verifies, installs,
// removes, loads, and bridges events to the add-on chrome on the main queue.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "shell/addons_mac.h"

#include <mediaviewer/mediaviewer_import.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "core/json.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "meta/meta.h"
#include "mv_chrome_bridge.h"
#include "shell/commands.h"
#include "shell/present_busy.h"

// The selectors the Import.bundle principal class answers (MVImportChrome in
// src.swift/ImportChrome). Declared here only so the calls type-check; the
// bundle is reached by message send, never by linking.
@protocol MVAddonChrome <NSObject>
- (void)attachWithTable:(NSValue*)table host:(id)host;
- (void)openWithSource:(NSString*)source marks:(NSArray<NSString*>*)marks;
- (void)importNow:(NSString*)pathsJson;
- (void)deliverEvent:(uint32_t)kind status:(uint32_t)status identifier:(uint64_t)ident payload:(int64_t)payload;
- (void)shutdown;
@end

@interface MvAddonHostMac : NSObject
@end

namespace {

struct mac_addons {
  std::unique_ptr<mv::addon::store> store;
  std::unique_ptr<mv::addon::loaded_addon> import;
  id<MVAddonChrome> chrome = nil;
  NSBundle* bundle = nil;
  MvAddonHostMac* host = nil;
  MvAddonsOpenPathFn open_path = nullptr;
  void* open_path_ctx = nullptr;
  std::string status;       // command-bar line while a job runs
  bool hint_pending = false;
  id mount_observer = nil;
  mv::image::thumb_store thumbs;
};

mac_addons& state() {
  static mac_addons s;
  return s;
}

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
  if (!s.thumbs.is_open()) {
    MV_TRY(std::string dir, mv::io::thumb_cache_dir());
    MV_TRY_VOID(s.thumbs.open(dir));
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
  mv::shell::set_addon_commands_available(false);
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
  mv::shell::set_addon_commands_available(true);
  return true;
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
@end

void MvAddonsStart(MvAddonsOpenPathFn open_path, void* ctx) {
  mac_addons& s = state();
  s.open_path = open_path;
  s.open_path_ctx = ctx;
  // Verifying hashes every file: not on the main thread (rule 1).
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    const bool ok = import_installed_ok();
    dispatch_async(dispatch_get_main_queue(), ^{
      if (ok) (void)load_import();
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
      w.key("state").string(found->state == mv::addon::install_state::ok       ? "ok"
                            : found->state == mv::addon::install_state::needs_update ? "needs_update"
                                                                                     : "invalid");
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
