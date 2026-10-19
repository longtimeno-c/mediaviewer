// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Final Cut Pro search agent (docs/design/23), in the Final Cut Pro add-on since
// 2026-10-19 (issue #485): a launchd agent, started on demand when the add-on's
// workflow extension looks up its Mach service, as MediaViewer's own
// executable with `--search-agent` (shell/fcp_host_mac.mm), which loads this
// add-on and the AI pack's read-only reader and calls run_agent with its
// Local search services. Not sandboxed (it reads the app's index and the
// viewer's thumbnail cache, as the app does); hardened runtime and library
// validation, so only code signed by the same team loads into it.
//
//   - Peers: only code signed by this agent's own team (its signature says
//     which); an unsigned development agent refuses everyone unless the host
//     says it may accept unsigned peers (a dev-key build).
//   - Idle: no process sits resident. The agent exits kIdleSeconds after its
//     last connection closes (or after launch, if nobody connects).
//   - Privacy (rule 6): nothing here logs a query, a path or a result; the
//     log carries status names only. No network.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <os/log.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_fcp.h>

#include "nle/search_wire.h"
#import "nle/mac/agent_protocol.h"

namespace {

constexpr int64_t kIdleSeconds = 50;  // the verify line: gone within 60 s of the last client

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

void MV_CALL to_data(void* user, const void* bytes, uint64_t size) {
  *static_cast<NSData* __strong*>(user) = [NSData dataWithBytes:bytes length:size];
}

}  // namespace

@interface MVAgent : NSObject <NSXPCListenerDelegate, MVSearchAgent>
- (instancetype)initWithServices:(const mv_fcp_services*)services allowUnsigned:(BOOL)allowUnsigned;
@end

@implementation MVAgent {
  dispatch_queue_t _work;  // searches, one at a time
  const mv_fcp_services* _services;
  std::string _thumbs;     // the viewer's cache folder, with a trailing '/'
  NSString* _team;
  BOOL _allowUnsigned;
  std::atomic<int> _connections;
  std::atomic<std::uint64_t> _idleGeneration;
}

- (instancetype)initWithServices:(const mv_fcp_services*)services allowUnsigned:(BOOL)allowUnsigned {
  if ((self = [super init])) {
    _work = dispatch_queue_create("mediaviewer.fcp.search",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                          QOS_CLASS_USER_INITIATED, 0));
    _services = services;
    _team = own_team();
    _allowUnsigned = allowUnsigned;
    _connections = 0;
    _idleGeneration = 0;
    if (services->thumbs_dir_utf8 && *services->thumbs_dir_utf8) _thumbs = std::string(services->thumbs_dir_utf8) + "/";
    // The pack loads now, while the editor types the first query: launchd
    // started this agent because the panel opened.
    dispatch_async(_work, ^{
      NSData* ignored = nil;
      (void)self->_services->roots_json(self->_services->ctx, &to_data, &ignored);
    });
    [self armIdle];
  }
  return self;
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
  } else if (!_allowUnsigned) {
    os_log_error(agent_log(), "unsigned agent: refusing connections");
    return NO;
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
  const std::uint64_t correlation = req->correlation_id;
  NSData* copy = [request copy];
  const std::string q = text ? std::string(text.UTF8String) : std::string();
  const std::string scope = scopeDir ? std::string(scopeDir.UTF8String) : std::string();
  dispatch_async(_work, ^{
    NSData* out = nil;
    const mv_status st = self->_services->run(self->_services->ctx, copy.bytes, copy.length, q.c_str(), scope.c_str(),
                                              &to_data, &out);
    if (st != MV_OK || !out) {
      os_log(agent_log(), "search failed: %d", static_cast<int>(st));
      reply(failure(correlation, st == MV_OK ? mv::status::internal : static_cast<mv::status>(st)));
      return;
    }
    reply(out);
  });
}

- (void)rootsWithReply:(void (^)(NSString*))reply {
  dispatch_async(_work, ^{
    NSData* out = nil;
    const mv_status st = self->_services->roots_json(self->_services->ctx, &to_data, &out);
    reply(st == MV_OK && out ? [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding] : nil);
  });
}

- (void)suggest:(NSString*)text withReply:(void (^)(NSString*))reply {
  const std::string q = text ? std::string(text.UTF8String) : std::string();
  dispatch_async(_work, ^{
    NSData* out = nil;
    const mv_status st = self->_services->suggest_json(self->_services->ctx, q.c_str(), &to_data, &out);
    reply(st == MV_OK && out ? [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding] : nil);
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

namespace {

mv_status MV_CALL run_agent(void*, const char* mach_service, const mv_fcp_services* services, uint32_t allow_unsigned) {
  if (!mach_service || !services || services->struct_size < sizeof(mv_fcp_services) || !services->run ||
      !services->roots_json || !services->suggest_json) {
    return MV_ERR_INVALID_ARG;
  }
  @autoreleasepool {
    MVAgent* agent = [[MVAgent alloc] initWithServices:services allowUnsigned:allow_unsigned != 0];
    NSXPCListener* listener = [[NSXPCListener alloc] initWithMachServiceName:@(mach_service)];
    listener.delegate = agent;
    [listener resume];
    os_log(agent_log(), "listening");
    [[NSRunLoop mainRunLoop] run];
  }
  return MV_ERR_INTERNAL;  // the run loop never returns; the idle exit ends the process
}

const mv_fcp_api kApi{sizeof(mv_fcp_api), 0, &run_agent};

}  // namespace

const mv_fcp_api* mv_fcp_agent_api() { return &kApi; }
