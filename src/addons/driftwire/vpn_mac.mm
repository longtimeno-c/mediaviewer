// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN's interface on the Mac (vpn.h): a Network Extension packet tunnel.
//
// The provider (src/addons/driftwire/mac_tunnel, a system extension inside
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
#include <cstring>
#include <mutex>

#include "addons/driftwire/net.h"
#include "addons/driftwire/vpn.h"

// The system extension's activation, answered on a queue of its own.
@interface MVTunnelActivation : NSObject <OSSystemExtensionRequestDelegate>
@property(nonatomic) dispatch_semaphore_t done;
@property(nonatomic) BOOL ok;
@property(nonatomic) BOOL needsApproval;
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
  self.ok = YES;
  dispatch_semaphore_signal(self.done);
}
- (void)request:(OSSystemExtensionRequest*)request didFailWithError:(NSError*)error {
  self.ok = NO;
  dispatch_semaphore_signal(self.done);
}
@end

namespace mv::driftwire::vpn {

namespace {

constexpr int kActivateMs = 120000;  // the person approves in System Settings
constexpr int kSystemMs = 15000;     // the preferences, the tunnel's start
constexpr int kProviderMs = 20000;   // the provider connects back

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

bool wait(dispatch_semaphore_t s, int ms) {
  return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ms) * NSEC_PER_MSEC)) == 0;
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

  result<std::unique_ptr<device>> start(const settings& s) override {
    std::lock_guard lock(m_);
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

    NSMutableArray* exclude = [NSMutableArray array];
    for (const auto& a : s.exclude) [exclude addObject:@(net::address_text(a).c_str())];
    NSMutableArray* nets = [NSMutableArray array];
    for (const auto& n : s.exclude_nets) {
      [nets addObject:@{@"base" : @(net::address_text(n.base).c_str()), @"prefix" : @(n.prefix)}];
    }
    NSDictionary* config = @{
      @"port" : @((*listener)->port()),
      @"token" : @(token.c_str()),
      @"address4" : @(s.address4.c_str()),
      @"prefix4" : @(s.prefix4),
      @"dns4" : @(s.dns4.c_str()),
      @"mtu" : @(s.mtu),
      @"exclude" : exclude,
      @"excludeNets" : nets,
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
    if (!wait(loaded, kSystemMs) || failed) return err(status::io);
    if (!manager) manager = [[NETunnelProviderManager alloc] init];
    NETunnelProviderProtocol* proto = [[NETunnelProviderProtocol alloc] init];
    proto.providerBundleIdentifier = extension_id();
    proto.serverAddress = s.relay_name.empty() ? @"Driftwire" : @(s.relay_name.c_str());
    proto.providerConfiguration = config;
    proto.includeAllNetworks = s.kill_switch;  // Block when the tunnel is down
    proto.excludeLocalNetworks = YES;
    manager.protocolConfiguration = proto;
    manager.localizedDescription = @"Driftwire";
    manager.enabled = YES;
    dispatch_semaphore_t saved = dispatch_semaphore_create(0);
    [manager saveToPreferencesWithCompletionHandler:^(NSError* e) {
      failed = e;
      dispatch_semaphore_signal(saved);
    }];
    if (!wait(saved, kActivateMs)) return err(status::timeout);
    if (failed) return err(failed.code == NEVPNErrorConfigurationReadWriteFailed ? status::permission_denied : status::io);
    dispatch_semaphore_t reloaded = dispatch_semaphore_create(0);
    [manager loadFromPreferencesWithCompletionHandler:^(NSError* e) {
      failed = e;
      dispatch_semaphore_signal(reloaded);
    }];
    if (!wait(reloaded, kSystemMs) || failed) return err(status::io);
    NSError* started = nil;
    if (![manager.connection startVPNTunnelAndReturnError:&started]) return err(status::io);
    manager_ = manager;

    // The provider connects back with the token, then the packets flow.
    auto c = (*listener)->accept(kProviderMs);
    if (!c) {
      stop_locked();
      return err(c.error());
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
  status activate() {
    MVTunnelActivation* d = [[MVTunnelActivation alloc] init];
    d.done = dispatch_semaphore_create(0);
    OSSystemExtensionRequest* r =
        [OSSystemExtensionRequest activationRequestForExtension:extension_id()
                                                          queue:dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)];
    r.delegate = d;
    [OSSystemExtensionManager.sharedManager submitRequest:r];
    if (!wait(d.done, kActivateMs)) return d.needsApproval ? status::permission_denied : status::timeout;
    return d.ok ? status::ok : status::permission_denied;
  }

  void stop_locked() {
    if (manager_) [manager_.connection stopVPNTunnel];
    manager_ = nil;
  }

  std::mutex m_;
  bool activated_ = false;                // under m_
  NETunnelProviderManager* manager_ = nil;  // under m_
};

}  // namespace

std::unique_ptr<os_port> make_os_port() { return std::make_unique<mac_port>(); }

}  // namespace mv::driftwire::vpn
