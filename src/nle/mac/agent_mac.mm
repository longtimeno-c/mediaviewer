// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Local search agent (plan/23 Phase 1): a launchd agent, started on
// demand when Final Cut Pro's workflow extension looks up its Mach service,
// as MediaViewer.app's own executable with `--search-agent` (main_mac.mm
// calls MvSearchAgentMain before anything of the viewer starts; the app
// already carries the add-on store, the verifier and SQLite, so the agent adds
// only this file and mv_nle to it),
// that hosts the installed AI pack through its read-only door and answers
// searches over XPC. Not sandboxed (it reads the app's index and the viewer's
// thumbnail cache, as the app does); hardened runtime and library validation,
// so only a pack signed by the same team loads into it.
//
//   - Peers: only code signed by this agent's own team (its signature says
//     which); an unsigned development agent refuses everyone unless built
//     with MV_FCP_DEV_ALLOW_UNSIGNED.
//   - Idle: no process sits resident. The agent exits kIdleSeconds after its
//     last connection closes (or after launch, if nobody connects).
//   - Privacy (rule 6): nothing here logs a query, a path or a result; the
//     log carries status names only. No network.
//   - Cost to the viewer (plan/23 "Performance"): the pack loads its text
//     towers only; searches run at user-initiated QoS, one at a time.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <os/log.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/manifest.h"
#include "addon/store.h"
#include "nle/search_session.h"
#include "nle/search_wire.h"
#include "io/paths.h"
#include "nle/mac/agent_mac.h"
#import "nle/mac/agent_protocol.h"

namespace {

constexpr int64_t kIdleSeconds = 50;  // the verify line: gone within 60 s of the last client
constexpr int kSearchTimeoutMs = 30000;

os_log_t agent_log() {
  static os_log_t log = os_log_create("io.github.longtimeno-c.mediaviewer.fcp", "agent");
  return log;
}

// This process's own Team ID, from its signature; nil when unsigned or ad hoc.
NSString* own_team() {
  SecCodeRef self = nullptr;
  if (SecCodeCopySelf(kSecCSDefaultFlags, &self) != errSecSuccess || !self) return nil;
  SecStaticCodeRef stat = nullptr;
  NSString* team = nil;
  if (SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &stat) == errSecSuccess && stat) {
    CFDictionaryRef info = nullptr;
    if (SecCodeCopySigningInformation(stat, kSecCSSigningInformation, &info) == errSecSuccess && info) {
      id t = ((__bridge NSDictionary*)info)[(__bridge NSString*)kSecCodeInfoTeamIdentifier];
      if ([t isKindOfClass:[NSString class]] && [t length] > 0) team = [t copy];
      CFRelease(info);
    }
    CFRelease(stat);
  }
  CFRelease(self);
  return team;
}

NSData* data_of(const std::vector<std::uint8_t>& v) { return [NSData dataWithBytes:v.data() length:v.size()]; }

NSData* failure(std::uint64_t correlation, mv::status code) {
  mv::nle::reply r;
  r.correlation_id = correlation;
  r.code = code;
  return data_of(mv::nle::encode(r));
}

}  // namespace

@interface MVAgent : NSObject <NSXPCListenerDelegate, MVSearchAgent>
@end

@implementation MVAgent {
  dispatch_queue_t _work;  // searches and the pack's load, one at a time
  std::unique_ptr<mv::addon::store> _store;
  std::unique_ptr<mv::nle::search_session> _session;
  mv::status _loadError;
  std::string _thumbs;  // the viewer's cache folder, with a trailing '/'
  NSString* _team;
  std::atomic<int> _connections;
  std::atomic<std::uint64_t> _idleGeneration;
}

- (instancetype)init {
  if ((self = [super init])) {
    _work = dispatch_queue_create("mediaviewer.fcp.search",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                          QOS_CLASS_USER_INITIATED, 0));
    _loadError = mv::status::ok;
    _team = own_team();
    _connections = 0;
    _idleGeneration = 0;
    if (auto thumbs = mv::io::thumb_cache_dir()) _thumbs = *thumbs + "/";
    // The pack loads now, while the editor types the first query: launchd
    // started this agent because the panel opened.
    dispatch_async(_work, ^{
      [self loadPack];
    });
    [self armIdle];
  }
  return self;
}

// [work queue]
- (void)loadPack {
  if (_session || _loadError != mv::status::ok) return;
  auto root = mv::io::addons_dir();
  if (!root || _thumbs.empty()) {
    _loadError = mv::status::io;
    return;
  }
  const auto key = mv::addon::pinned_public_key();
  _store = std::make_unique<mv::addon::store>(*root, std::vector<std::uint8_t>(key.begin(), key.end()),
                                              MV_ADDON_HOST_API);
  auto opened = mv::nle::search_session::open(*_store, _thumbs.substr(0, _thumbs.size() - 1));
  if (!opened) {
    // unsupported_format: the installed pack predates the reader.
    _loadError = opened.error();
    os_log_error(agent_log(), "Local search did not load: %{public}s", mv::status_name(opened.error()));
    return;
  }
  _session = std::move(*opened);
}

// Exits kIdleSeconds from now unless a connection arrives meanwhile.
- (void)armIdle {
  const std::uint64_t generation = ++_idleGeneration;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kIdleSeconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    if (self->_idleGeneration.load() != generation || self->_connections.load() > 0) return;
    os_log(agent_log(), "idle: exiting");
    // The pack's threads may be mid-search: no static destructors (as the app's quit).
    _exit(0);
  });
}

- (BOOL)listener:(NSXPCListener*)listener shouldAcceptNewConnection:(NSXPCConnection*)connection {
  (void)listener;
  if (_team) {
    // Checked by the XPC runtime on every message, not once at connect.
    NSString* req = [NSString
        stringWithFormat:@"anchor apple generic and certificate leaf[subject.OU] = \"%@\"", _team];
    [connection setCodeSigningRequirement:req];
  } else {
#if !defined(MV_FCP_DEV_ALLOW_UNSIGNED)
    os_log_error(agent_log(), "unsigned agent: refusing connections");
    return NO;
#endif
  }
  connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MVSearchAgent)];
  connection.exportedObject = self;
  ++_connections;
  ++_idleGeneration;  // a pending exit is cancelled
  __weak MVAgent* weak = self;
  connection.invalidationHandler = ^{
    MVAgent* agent = weak;
    if (!agent) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (--agent->_connections == 0) [agent armIdle];
    });
  };
  [connection resume];
  return YES;
}

- (void)run:(NSData*)request
        text:(NSString*)text
    scopeDir:(NSString*)scopeDir
   withReply:(void (^)(NSData*))reply {
  const auto* bytes = static_cast<const std::uint8_t*>(request.bytes);
  auto req = mv::nle::decode_request(std::span<const std::uint8_t>(bytes, request.length));
  if (!req) {
    reply(failure(0, req.error()));
    return;
  }
  const mv::nle::request r = *req;
  const std::string q = text ? std::string(text.UTF8String) : std::string();
  const std::string scope = scopeDir ? std::string(scopeDir.UTF8String) : std::string();
  dispatch_async(_work, ^{
    [self loadPack];
    if (!self->_session) {
      reply(failure(r.correlation_id, self->_loadError == mv::status::ok ? mv::status::internal : self->_loadError));
      return;
    }
    const mv::nle::reply out = self->_session->run(r, q, scope, kSearchTimeoutMs);
    if (out.code != mv::status::ok) {
      os_log(agent_log(), "search %llu: %{public}s", static_cast<unsigned long long>(r.correlation_id),
             mv::status_name(out.code));
    }
    reply(data_of(mv::nle::encode(out)));
  });
}

- (void)rootsWithReply:(void (^)(NSString*))reply {
  dispatch_async(_work, ^{
    [self loadPack];
    auto json = self->_session ? self->_session->roots_json(kSearchTimeoutMs) : mv::result<std::string>(mv::err(mv::status::not_found));
    reply(json ? [NSString stringWithUTF8String:json->c_str()] : nil);
  });
}

- (void)suggest:(NSString*)text withReply:(void (^)(NSString*))reply {
  const std::string q = text ? std::string(text.UTF8String) : std::string();
  dispatch_async(_work, ^{
    [self loadPack];
    auto json = self->_session ? self->_session->suggest_json(q, 2000) : mv::result<std::string>(mv::err(mv::status::not_found));
    reply(json ? [NSString stringWithUTF8String:json->c_str()] : nil);
  });
}

- (void)thumbnail:(NSString*)path withReply:(void (^)(NSData*))reply {
  // Only a file in the viewer's own cache, by its resolved path: this is not
  // a way to read anything else on the disk.
  NSString* resolved = [path stringByResolvingSymlinksInPath];
  const std::string p = resolved ? std::string(resolved.UTF8String) : std::string();
  NSString* cache = [[NSString stringWithUTF8String:_thumbs.c_str()] stringByResolvingSymlinksInPath];
  const std::string root = cache ? std::string(cache.UTF8String) + "/" : std::string();
  if (root.size() <= 1 || p.rfind(root, 0) != 0 || p.find("/..") != std::string::npos ||
      p.size() < 5 || p.compare(p.size() - 4, 4, ".jpg") != 0) {
    reply(nil);
    return;
  }
  dispatch_async(_work, ^{
    reply([NSData dataWithContentsOfFile:resolved options:NSDataReadingMappedIfSafe error:nil]);
  });
}

@end

int MvSearchAgentMain() {
  @autoreleasepool {
    MVAgent* agent = [[MVAgent alloc] init];
    NSXPCListener* listener = [[NSXPCListener alloc] initWithMachServiceName:@MV_FCP_MACH_SERVICE];
    listener.delegate = agent;
    [listener resume];
    [[NSRunLoop mainRunLoop] run];
  }
  return 0;
}
