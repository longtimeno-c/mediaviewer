// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Final Cut Pro search on / off (docs/design/23). Since 2026-10-19 (#485,
// docs/design/12 "Final Cut Pro search is an add-on") the pieces are the Final
// Cut Pro add-on's, arm64 only (cmake/darwin-fcp.cmake):
//
//   <add-on>/MediaViewer Search.app                  a background-only container around
//                                                    MediaViewerSearch.appex, the workflow
//                                                    extension (macOS discovers one only
//                                                    inside an app LaunchServices knows)
//   <add-on>/libmv_fcp.dylib                         the search agent, hosted by
//                                                    `MediaViewer --search-agent`
//   Contents/Library/LaunchAgents/<service>.plist    (in the app) the agent's
//                                                    launchd job: this app's own
//                                                    executable, --search-agent
//
// Nothing of it exists until the owner installs the add-on from Settings >
// Local search (offered once the Local search pack is installed: the pack is
// the bulk, and the agent hosts it read-only), and nothing runs until they turn
// it on there. Off, the agent is not registered with launchd (nothing runs,
// nothing is listed in Login Items) and the extension is not registered with
// PlugInKit, so Final Cut Pro lists nothing: docs/design/18's "absent means
// absent". On, SMAppService registers the agent (launchd starts it when the
// panel asks and it exits when idle), LaunchServices registers the container
// and pluginkit the extension, elected "use". The user never opens the
// container (the owner's call, 2026-09-28: no second app to install or open).
//
// Nothing here runs on the main thread except opening System Settings:
// the store's verify, SMAppService and pluginkit are I/O, IPC and a child process.
#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
#import <ServiceManagement/ServiceManagement.h>

#include "shell/fcp_mac.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/manifest.h"
#include "addon/store.h"
#include "io/paths.h"

// The names are compiled into the app by cmake/darwin-fcp.cmake. The lab
// (mediaviewer_lab) has none, and reports Final Cut Pro as absent.
#if defined(MV_FCP_AGENT_PLIST) && defined(MV_FCP_EXTENSION_ID) && defined(MV_FCP_ADDON_EXTENSION)
#define MV_FCP_NAMED 1
#else
#define MV_FCP_NAMED 0
#endif

namespace {

enum fcp_state : std::int32_t {
  kAbsent = 0,        // not in this build, or not this Mac (the pack is arm64 only)
  kOff = 1,
  kOn = 2,
  kApproval = 3,      // on, but macOS waits for Login Items approval
  kNotInstalled = 4,  // the Final Cut Pro add-on is not installed (Settings offers it)
};

#if MV_FCP_NAMED
// UserDefaults: the bundled extension of an app before 2026-10-19 was elected
// "ignore" once; the add-on's extension starts unregistered instead.
NSString* const kHiddenKey = @"MVFinalCutExtensionHidden";

constexpr bool this_mac_runs_it() {
#if defined(__arm64__) || defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

bool job_present() {
  NSString* agent = [NSBundle.mainBundle.bundlePath
      stringByAppendingPathComponent:@"Contents/Library/LaunchAgents/" MV_FCP_AGENT_PLIST];
  return [NSFileManager.defaultManager fileExistsAtPath:agent];
}

// The installed add-on's version folder, verified (the store hashes its
// files: a library and an extension, ~1 MB). nullopt when it is not installed. [worker]
std::optional<std::string> installed_dir() {
  auto root = mv::io::addons_dir();
  if (!root) return std::nullopt;
  const auto key = mv::addon::pinned_public_key();
  const mv::addon::store s(*root, std::vector<std::uint8_t>(key.begin(), key.end()), MV_ADDON_HOST_API);
  auto found = s.find("fcp");
  if (!found || found->state != mv::addon::install_state::ok) return std::nullopt;
  if (![NSFileManager.defaultManager fileExistsAtPath:@((found->dir + "/" MV_FCP_ADDON_EXTENSION).c_str())]) {
    return std::nullopt;
  }
  return found->dir;
}

SMAppService* agent_service() { return [SMAppService agentServiceWithPlistName:@MV_FCP_AGENT_PLIST]; }

bool run(NSString* tool, NSArray<NSString*>* args) {
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:tool];
  task.arguments = args;
  task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
  task.standardError = NSFileHandle.fileHandleWithNullDevice;
  NSError* error = nil;
  if (![task launchAndReturnError:&error]) return false;
  [task waitUntilExit];
  return task.terminationStatus == 0;
}

// pluginkit is the supported way to add an extension to PlugInKit and to
// elect it in or out. [worker]
bool pluginkit(NSArray<NSString*>* args) { return run(@"/usr/bin/pluginkit", args); }

// Registers the add-on's container with LaunchServices and its extension
// with PlugInKit (idempotent; a newer version's folder replaces the older
// one's) and elects it in; or elects it out and unregisters both. [worker]
void extension(bool use, const std::string& dir) {
  NSString* container = @((dir + "/" MV_FCP_ADDON_CONTAINER).c_str());
  NSString* appex = @((dir + "/" MV_FCP_ADDON_EXTENSION).c_str());
  if (use) {
    (void)LSRegisterURL((__bridge CFURLRef)[NSURL fileURLWithPath:container], true);
    (void)pluginkit(@[ @"-a", appex ]);
    (void)pluginkit(@[ @"-e", @"use", @"-i", @MV_FCP_EXTENSION_ID ]);
  } else {
    (void)pluginkit(@[ @"-e", @"ignore", @"-i", @MV_FCP_EXTENSION_ID ]);
    if (dir.empty()) return;
    (void)pluginkit(@[ @"-r", appex ]);
    // No public call unregisters a bundle; lsregister is the system's own tool.
    (void)run(@"/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
              @[ @"-u", container ]);
  }
}

std::int32_t state_of(SMAppService* agent) {
  switch (agent.status) {
    case SMAppServiceStatusEnabled: return kOn;
    case SMAppServiceStatusRequiresApproval: return kApproval;
    default: return kOff;
  }
}

bool registered(SMAppService* agent) {
  const SMAppServiceStatus s = agent.status;
  return s == SMAppServiceStatusEnabled || s == SMAppServiceStatusRequiresApproval;
}
#endif

}  // namespace

// ---- bridge (mv_chrome_bridge.h) ------------------------------------------------------

// [worker] The store's verify of a small add-on, and an XPC round trip to the service manager.
extern "C" std::int32_t mv_fcp_state(void) {
#if MV_FCP_NAMED
  if (!this_mac_runs_it() || !job_present()) return kAbsent;
  if (!installed_dir()) return kNotInstalled;
  return state_of(agent_service());
#else
  return kAbsent;
#endif
}

// [worker] Turns the agent and the extension on or off together; returns the
// state after (kApproval: on, once the owner allows it in Login Items;
// kNotInstalled: on was asked without the add-on).
extern "C" std::int32_t mv_fcp_set_enabled(bool on) {
#if MV_FCP_NAMED
  if (!this_mac_runs_it() || !job_present()) return kAbsent;
  const auto dir = installed_dir();
  SMAppService* agent = agent_service();
  NSError* error = nil;
  if (on) {
    if (!dir) return kNotInstalled;
    // requiresApproval can come back as an error the first time; the status
    // below is what counts.
    if (agent.status != SMAppServiceStatusEnabled) (void)[agent registerAndReturnError:&error];
    extension(true, *dir);
  } else {
    if (agent.status != SMAppServiceStatusNotRegistered) (void)[agent unregisterAndReturnError:&error];
    extension(false, dir.value_or(std::string()));
    if (!dir) return kNotInstalled;
  }
  return state_of(agent);
#else
  (void)on;
  return kAbsent;
#endif
}

// [main-thread]
extern "C" void mv_fcp_open_login_items(void) { [SMAppService openSystemSettingsLoginItems]; }

// ---- launch --------------------------------------------------------------------------

void MvFcpStart() {
#if MV_FCP_NAMED
  // Well after first pixel, at background priority (rule 1; "the view comes
  // first"). Off and never installed: one XPC call. On: the add-on's verify,
  // then the registrations again (idempotent).
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                 dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
                   if (!this_mac_runs_it() || !job_present()) return;
                   SMAppService* agent = agent_service();
                   if (!registered(agent)) return;
                   const auto dir = installed_dir();
                   NSError* error = nil;
                   if (!dir) {
                     // The add-on was removed while on: nothing left to start.
                     (void)[agent unregisterAndReturnError:&error];
                     (void)pluginkit(@[ @"-e", @"ignore", @"-i", @MV_FCP_EXTENSION_ID ]);
                     return;
                   }
                   // On: picks up a launchd job an update changed, and the
                   // folder of the add-on's newest version.
                   (void)[agent registerAndReturnError:&error];
                   extension(true, *dir);
                   [NSUserDefaults.standardUserDefaults removeObjectForKey:kHiddenKey];
                 });
#endif
}
