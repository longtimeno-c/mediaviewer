// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN's interface on the Mac (vpn.h): a Network Extension packet tunnel.
//
// The provider (src/addons/vpn/mac_tunnel, a system extension inside
// MediaViewer.app, `<app id>.DriftwireTunnel`) is a thin pump: it sets the
// interface's address, routes and DNS from what this file gives it, and
// passes packets both ways over a loopback TCP connection to the add-on,
// which proves itself with a token made here. It does nothing unless the
// add-on starts it. Everything else (the stack, the flows, the relay) is the
// add-on's (vpn.cpp), so the code still lives in the add-on
// (docs/design/12, 2026-11-12: the owner asked for slice 5; the extension is
// the one part the OS makes live in the app's bundle).
//
// It needs the Network Extension entitlement (packet-tunnel-provider,
// system extension) on the signing team's Developer ID profile, which only
// Apple grants: owed. A build without the extension in its bundle says so
// (unavailable()), and the proxy carries on.
//
// Every call here runs on the engine's control thread (never the UI's) and
// waits for the system's answers with a timeout.
#import <Foundation/Foundation.h>
#import <NetworkExtension/NetworkExtension.h>
#import <Security/Security.h>
#import <SystemExtensions/SystemExtensions.h>

#include <sodium.h>

#include <chrono>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "wire/net.h"
#include "addons/vpn/vpn.h"

// The system extension's activation, answered on a queue of its own.
@interface MVTunnelActivation : NSObject <OSSystemExtensionRequestDelegate>
@property(nonatomic) dispatch_semaphore_t done;
@property(atomic) BOOL finished;
@property(atomic) BOOL ok;
@property(atomic) BOOL needsApproval;
@property(atomic) BOOL needsReboot;
@end

@implementation MVTunnelActivation
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest*)request
                  actionForReplacingExtension:(OSSystemExtensionProperties*)existing
                                withExtension:(OSSystemExtensionProperties*)ext {
  return OSSystemExtensionReplacementActionReplace;
}
- (void)requestNeedsUserApproval:(OSSystemExtensionRequest*)request {
  self.needsApproval = YES;  // System Settings asks; the result comes later
}
- (void)request:(OSSystemExtensionRequest*)request didFinishWithResult:(OSSystemExtensionRequestResult)result {
  // Replacing a running extension may only finish at the next restart.
  self.needsReboot = result == OSSystemExtensionRequestWillCompleteAfterReboot;
  self.ok = !self.needsReboot;
  self.finished = YES;
  dispatch_semaphore_signal(self.done);
}
- (void)request:(OSSystemExtensionRequest*)request didFailWithError:(NSError*)error {
  self.ok = NO;
  self.finished = YES;
  dispatch_semaphore_signal(self.done);
}
@end

namespace mv::driftwire::vpn {

namespace {

constexpr int kActivateMs = 120000;  // the person approves in System Settings
constexpr int kSystemMs = 15000;     // the preferences, the tunnel's start
constexpr int kProviderMs = 20000;   // the provider connects back
constexpr int kStopMs = 5000;        // the tunnel's session goes down

NSString* extension_id() {
  NSString* app = NSBundle.mainBundle.bundleIdentifier ?: @"io.github.longtimeno-c.mediaviewer";
  return [app stringByAppendingString:@".DriftwireTunnel"];
}

// Whether the extension at `path` is signed with the packet tunnel's Network
// Extension entitlement (its signature read, nothing run).
bool signed_for_packet_tunnel(NSString* path) {
  SecStaticCodeRef code = nullptr;
  if (SecStaticCodeCreateWithPath((__bridge CFURLRef)[NSURL fileURLWithPath:path], kSecCSDefaultFlags, &code) !=
      errSecSuccess) {
    return false;
  }
  CFDictionaryRef info = nullptr;
  const OSStatus s = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info);
  CFRelease(code);
  if (s != errSecSuccess || !info) return false;
  NSDictionary* d = CFBridgingRelease(info);
  NSDictionary* ent = d[(__bridge NSString*)kSecCodeInfoEntitlementsDict];
  NSArray* ne = [ent[@"com.apple.developer.networking.networkextension"] isKindOfClass:NSArray.class]
                    ? ent[@"com.apple.developer.networking.networkextension"]
                    : nil;
  return [ne containsObject:@"packet-tunnel-provider-systemextension"];
}

// Waits up to `ms` for `s`, in short steps, giving up at once when
// `cancelled` (quitting: the control thread must not hold the app open for
// minutes while System Settings waits for the person).
bool wait(dispatch_semaphore_t s, int ms, const std::atomic<bool>& cancelled) {
  constexpr int kStepMs = 100;
  for (int left = ms; left > 0 && !cancelled.load(); left -= kStepMs) {
    const int step = left < kStepMs ? left : kStepMs;
    if (dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(step) * NSEC_PER_MSEC)) == 0) {
      return true;
    }
  }
  return false;
}

std::string hex(const std::uint8_t* b, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(kHex[b[i] >> 4]);
    out.push_back(kHex[b[i] & 15]);
  }
  return out;
}

class mac_port final : public os_port {
 public:
  std::string unavailable() override {
    NSString* path = [NSString stringWithFormat:@"%@/Contents/Library/SystemExtensions/%@.systemextension",
                                                NSBundle.mainBundle.bundlePath, extension_id()];
    if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
      return "All traffic (VPN) needs MediaViewer's network extension, which this build does not carry yet (it "
             "waits for Apple's Network Extension entitlement). The proxy works without it.";
    }
    // A build that carries it but not signed with Apple's entitlement (a
    // developer's ad hoc assemble): macOS would refuse to load it.
    if (!signed_for_packet_tunnel(path)) {
      return "All traffic (VPN) needs MediaViewer's network extension signed with Apple's Network Extension "
             "entitlement, and this build's is not. The proxy works without it.";
    }
    return {};
  }

  // Block when the tunnel is down would be includeAllNetworks, which holds
  // back every connection but the provider's own: the add-on's link to the
  // relay leaves from the app, not the provider, so with it the Mac would be
  // cut off while "connected". Not offered until that is tried on a Mac.
  std::string block_unavailable() override {
    return "Block when the tunnel is down isn't offered on the Mac yet.";
  }

  void cancel() override { cancelled_.store(true); }

  result<std::unique_ptr<device>> start(const settings& s) override {
    std::lock_guard lock(m_);
    if (cancelled_.load()) return err(status::cancelled);
    if (!activated_) {
      if (const status a = activate(); a != status::ok) return err(a);
      activated_ = true;
    }
    // Where the provider connects back, and the token it proves itself with.
    auto listener = net::listen(true);
    if (!listener) return err(listener.error());
    std::uint8_t raw[16];
    randombytes_buf(raw, sizeof raw);
    const std::string token = hex(raw, sizeof raw);

    // The routes, both families (plan_routes): the provider sets these as they are.
    const route_plan plan = plan_routes(s, route_style::default_route);
    const auto routes = [](const std::vector<net::subnet>& all) {
      NSMutableArray* out = [NSMutableArray array];
      for (const auto& n : all) [out addObject:@{@"base" : @(net::address_text(n.base).c_str()), @"prefix" : @(n.prefix)}];
      return out;
    };
    NSDictionary* config = @{
      @"port" : @((*listener)->port()),
      @"token" : @(token.c_str()),
      @"address4" : @(s.address4.c_str()),
      @"prefix4" : @(s.prefix4),
      @"dns4" : @(s.dns4.c_str()),
      @"address6" : @(s.address6.c_str()),
      @"prefix6" : @(s.prefix6),
      @"mtu" : @(s.mtu),
      @"include4" : routes(plan.include4),
      @"include6" : routes(plan.include6),
      @"exclude4" : routes(plan.exclude4),
      @"exclude6" : routes(plan.exclude6),
    };

    // The tunnel's configuration in the system's VPN list (the first time,
    // the system asks the person to allow it).
    __block NETunnelProviderManager* manager = nil;
    __block NSError* failed = nil;
    dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
    [NETunnelProviderManager loadAllFromPreferencesWithCompletionHandler:^(NSArray* all, NSError* e) {
      for (NETunnelProviderManager* m in all) {
        if ([((NETunnelProviderProtocol*)m.protocolConfiguration).providerBundleIdentifier
                isEqualToString:extension_id()]) {
          manager = m;
        }
      }
      failed = e;
      dispatch_semaphore_signal(loaded);
    }];
    if (!wait(loaded, kSystemMs, cancelled_) || failed) return err(status::io);
    if (!manager) manager = [[NETunnelProviderManager alloc] init];
    NETunnelProviderProtocol* proto = [[NETunnelProviderProtocol alloc] init];
    proto.providerBundleIdentifier = extension_id();
    proto.serverAddress = s.relay_name.empty() ? @"MediaViewer VPN" : @(s.relay_name.c_str());
    proto.providerConfiguration = config;
    proto.includeAllNetworks = NO;  // block_unavailable(): never the kill switch here yet
    (void)s.kill_switch;
    proto.excludeLocalNetworks = YES;
    manager.protocolConfiguration = proto;
    manager.localizedDescription = @"MediaViewer VPN";
    manager.enabled = YES;
    dispatch_semaphore_t saved = dispatch_semaphore_create(0);
    [manager saveToPreferencesWithCompletionHandler:^(NSError* e) {
      failed = e;
      dispatch_semaphore_signal(saved);
    }];
    if (!wait(saved, kActivateMs, cancelled_)) {
      // Its answer may still come (the person allows it late, or after quit):
      // disabled now, so it never lands enabled with nobody listening.
      manager_ = manager;
      stop_locked();
      return err(cancelled_.load() ? status::cancelled : status::timeout);
    }
    if (failed) return err(failed.code == NEVPNErrorConfigurationReadWriteFailed ? status::permission_denied : status::io);
    // Saved and enabled: from here every way out takes it down and disables it,
    // so a dead configuration is never left for System Settings to connect.
    manager_ = manager;
    dispatch_semaphore_t reloaded = dispatch_semaphore_create(0);
    [manager loadFromPreferencesWithCompletionHandler:^(NSError* e) {
      failed = e;
      dispatch_semaphore_signal(reloaded);
    }];
    if (!wait(reloaded, kSystemMs, cancelled_) || failed) {
      stop_locked();
      return err(status::io);
    }
    NSError* started = nil;
    if (![manager.connection startVPNTunnelAndReturnError:&started]) {
      stop_locked();
      return err(status::io);
    }

    // The provider connects back with the token, then the packets flow.
    decltype((*listener)->accept(0)) c = err(status::timeout);
    for (int left = kProviderMs; left > 0 && !cancelled_.load(); left -= 250) {
      c = (*listener)->accept(250);
      if (c || c.error() != status::timeout) break;
    }
    if (!c) {
      stop_locked();
      return err(cancelled_.load() ? status::cancelled : c.error());
    }
    std::string said(token.size() + 1, '\0');
    if (!read_exact(**c, std::span(reinterpret_cast<std::uint8_t*>(said.data()), said.size()), kSystemMs) ||
        said.back() != '\n' || sodium_memcmp(said.data(), token.data(), token.size()) != 0) {
      stop_locked();
      return err(status::permission_denied);
    }
    return framed_device(std::move(*c));
  }

  void stop() override {
    std::lock_guard lock(m_);
    stop_locked();
  }

 private:
  // The system extension, approved (or not) by the person. A request whose
  // answer did not come in time stays: approving it a moment later is still
  // seen by the next start, which waits on it instead of asking again.
  status activate() {
    MVTunnelActivation* d = pending_;
    if (!d) {
      d = [[MVTunnelActivation alloc] init];
      d.done = dispatch_semaphore_create(0);
      OSSystemExtensionRequest* r =
          [OSSystemExtensionRequest activationRequestForExtension:extension_id()
                                                            queue:dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)];
      r.delegate = d;
      pending_ = d;
      [OSSystemExtensionManager.sharedManager submitRequest:r];
    }
    if (!d.finished && !wait(d.done, kActivateMs, cancelled_)) {
      if (cancelled_.load()) return status::cancelled;
      return d.needsApproval ? status::permission_denied : status::timeout;
    }
    pending_ = nil;  // answered: a later start asks afresh if it has to
    if (d.needsReboot) return status::busy;  // finishes at the next restart
    return d.ok ? status::ok : status::permission_denied;
  }

  // Takes the tunnel down, waits (briefly) until it is, and disables the saved
  // configuration: a restart then never races the old session, and nothing
  // stays enabled for System Settings to connect to a closed port.
  void stop_locked() {
    NETunnelProviderManager* m = manager_;
    manager_ = nil;
    if (!m) return;
    [m.connection stopVPNTunnel];
    // Network Extension answers on the main thread: waiting there (the
    // add-on's shutdown is the UI thread's) would only run to the timeout,
    // and quitting waits for nothing. The requests go either way.
    const bool can_wait = !NSThread.isMainThread && !cancelled_.load();
    for (int left = can_wait ? kStopMs : 0; left > 0; left -= 100) {
      const NEVPNStatus st = m.connection.status;
      if (st == NEVPNStatusDisconnected || st == NEVPNStatusInvalid) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    m.enabled = NO;
    dispatch_semaphore_t saved = dispatch_semaphore_create(0);
    [m saveToPreferencesWithCompletionHandler:^(NSError*) {
      dispatch_semaphore_signal(saved);
    }];
    if (can_wait) (void)wait(saved, kSystemMs, cancelled_);
  }

  std::mutex m_;
  std::atomic<bool> cancelled_{false};
  bool activated_ = false;                   // under m_
  MVTunnelActivation* pending_ = nil;        // under m_: an activation still waiting for its answer
  NETunnelProviderManager* manager_ = nil;   // under m_
};

}  // namespace

std::unique_ptr<os_port> make_os_port() { return std::make_unique<mac_port>(); }

}  // namespace mv::driftwire::vpn
