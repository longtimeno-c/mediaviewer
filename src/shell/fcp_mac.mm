// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Final Cut Pro search on / off (docs/design/23; docs/design/12 2026-09-28, amended the same
// day). MediaViewer.app carries the pieces, arm64 only (cmake/darwin-fcp.cmake):
//
//   Contents/PlugIns/MediaViewerSearch.appex         the workflow extension
//   Contents/Library/LaunchAgents/<service>.plist    the Local search agent's
//                                                    launchd job: this app's own
//                                                    executable, --search-agent
//
// Both are dormant until the owner turns Final Cut Pro on in Settings > Local
// search, which is offered once the Local search pack is installed: the
// pack is the bulk, downloaded like any add-on, and the agent hosts it
// read-only. Off, the agent is not registered with launchd (nothing runs,
// nothing is listed in Login Items) and the extension is hidden from Final Cut
// Pro by a PlugInKit "ignore" election, so docs/design/18's "absent means absent"
// holds for everyone who has not asked for it. On, SMAppService registers the
// agent (launchd starts it when the panel asks and it exits when idle) and
// the election becomes "use".
//
// Nothing here runs on the main thread except opening System Settings:
// SMAppService and pluginkit are IPC and a child process.
#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

#include "shell/fcp_mac.h"

#include <cstdint>

// The names are compiled into the app by cmake/darwin-fcp.cmake. The lab
// (mediaviewer_lab) has none, and reports Final Cut Pro as absent.
#if defined(MV_FCP_AGENT_PLIST) && defined(MV_FCP_EXTENSION_ID)
#define MV_FCP_NAMED 1
#else
#define MV_FCP_NAMED 0
#endif

namespace {

enum fcp_state : std::int32_t {
  kAbsent = 0,    // not in this build, or not this Mac (the pack is arm64 only)
  kOff = 1,
  kOn = 2,
  kApproval = 3,  // on, but macOS waits for Login Items approval
};

#if MV_FCP_NAMED
// UserDefaults: the extension has been hidden once (a fresh install lists
// every extension it carries until told otherwise).
NSString* const kHiddenKey = @"MVFinalCutExtensionHidden";

// The pieces in this bundle, whatever the Mac (an Intel Mac running the
// universal app has them too, and hides the extension like everyone else).
bool pieces_present() {
  NSBundle* b = NSBundle.mainBundle;
  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* appex = [b.builtInPlugInsPath stringByAppendingPathComponent:@"MediaViewerSearch.appex"];
  NSString* agent = [b.bundlePath stringByAppendingPathComponent:@"Contents/Library/LaunchAgents/" MV_FCP_AGENT_PLIST];
  return [fm fileExistsAtPath:appex] && [fm fileExistsAtPath:agent];
}

constexpr bool this_mac_runs_it() {
#if defined(__arm64__) || defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

SMAppService* agent_service() { return [SMAppService agentServiceWithPlistName:@MV_FCP_AGENT_PLIST]; }

// pluginkit is the supported way to elect an extension in or out. [worker]
bool elect(bool use) {
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/pluginkit"];
  task.arguments = @[ @"-e", use ? @"use" : @"ignore", @"-i", @MV_FCP_EXTENSION_ID ];
  task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
  task.standardError = NSFileHandle.fileHandleWithNullDevice;
  NSError* error = nil;
  if (![task launchAndReturnError:&error]) return false;
  [task waitUntilExit];
  return task.terminationStatus == 0;
}

std::int32_t state_of(SMAppService* agent) {
  switch (agent.status) {
    case SMAppServiceStatusEnabled: return kOn;
    case SMAppServiceStatusRequiresApproval: return kApproval;
    default: return kOff;
  }
}
#endif

}  // namespace

// ---- bridge (mv_chrome_bridge.h) ------------------------------------------------------

// [worker] An XPC round trip to the service manager.
extern "C" std::int32_t mv_fcp_state(void) {
#if MV_FCP_NAMED
  if (!this_mac_runs_it() || !pieces_present()) return kAbsent;
  return state_of(agent_service());
#else
  return kAbsent;
#endif
}

// [worker] Turns the agent and the extension on or off together; returns the
// state after (kApproval: on, once the owner allows it in Login Items).
extern "C" std::int32_t mv_fcp_set_enabled(bool on) {
#if MV_FCP_NAMED
  if (!this_mac_runs_it() || !pieces_present()) return kAbsent;
  SMAppService* agent = agent_service();
  NSError* error = nil;
  if (on) {
    // requiresApproval can come back as an error the first time; the status
    // below is what counts.
    if (agent.status != SMAppServiceStatusEnabled) (void)[agent registerAndReturnError:&error];
    (void)elect(true);
  } else {
    if (agent.status != SMAppServiceStatusNotRegistered) (void)[agent unregisterAndReturnError:&error];
    (void)elect(false);
  }
  [NSUserDefaults.standardUserDefaults setBool:YES forKey:kHiddenKey];
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
  // first"). A stat of two paths, then at most one XPC call and one pluginkit.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                 dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
                   if (!pieces_present()) return;
                   NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
                   if (!this_mac_runs_it()) {
                     if (![d boolForKey:kHiddenKey] && elect(false)) [d setBool:YES forKey:kHiddenKey];
                     return;
                   }
                   SMAppService* agent = agent_service();
                   const SMAppServiceStatus status = agent.status;
                   if (status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval) {
                     // On: idempotent, and picks up a launchd job an update changed.
                     NSError* error = nil;
                     (void)[agent registerAndReturnError:&error];
                     return;
                   }
                   if (![d boolForKey:kHiddenKey] && elect(false)) [d setBool:YES forKey:kHiddenKey];
                 });
#endif
}
