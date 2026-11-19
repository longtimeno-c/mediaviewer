// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// update.h's port on the Mac: NSURLSession in an ephemeral session (no
// cookies, no cache, no credentials), a User-Agent and nothing else; the
// program's own path from _NSGetExecutablePath, links resolved (Homebrew's
// bin/driftwire is a link into its Cellar).
#include "addons/driftwire/cli/update.h"

#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

@interface MVDriftwireUpdateRedirects : NSObject <NSURLSessionTaskDelegate>
@end

@implementation MVDriftwireUpdateRedirects
// Redirects to https only.
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))done {
  done([request.URL.scheme isEqualToString:@"https"] ? request : nil);
}
@end

namespace mv::driftwire::cli {

http_answer https_get(const std::string& url, std::size_t max_bytes, int timeout_ms) {
  http_answer a;
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u || ![u.scheme isEqualToString:@"https"]) {
      a.error = "bad URL";
      return a;
    }
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    config.HTTPShouldSetCookies = NO;
    config.URLCache = nil;
    config.URLCredentialStorage = nil;
    config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    config.timeoutIntervalForRequest = timeout_ms / 1000.0;
    config.timeoutIntervalForResource = timeout_ms / 1000.0;
    config.HTTPAdditionalHeaders = @{@"User-Agent" : @"driftwire"};
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config
                                                          delegate:[MVDriftwireUpdateRedirects new]
                                                     delegateQueue:nil];
    __block int status = 0;
    __block std::string body;
    __block std::string error;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task =
        [session dataTaskWithURL:u
               completionHandler:^(NSData* data, NSURLResponse* response, NSError* err) {
                 if (err) {
                   error = err.localizedDescription ? std::string(err.localizedDescription.UTF8String)
                                                    : std::string("network error");
                 } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                   status = static_cast<int>(static_cast<NSHTTPURLResponse*>(response).statusCode);
                   if (data && data.length <= max_bytes) {
                     body.assign(static_cast<const char*>(data.bytes), data.length);
                   } else if (data) {
                     status = 0;
                     error = "too long";
                   }
                 }
                 dispatch_semaphore_signal(done);
               }];
    [task resume];
    const dispatch_time_t wait =
        dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(timeout_ms + 1000) * static_cast<std::int64_t>(NSEC_PER_MSEC));
    if (dispatch_semaphore_wait(done, wait) != 0) {
      [task cancel];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);  // the handler runs once, cancelled
      status = 0;
      error = "timed out";
    }
    [session finishTasksAndInvalidate];
    a.status = status;
    a.body = std::move(body);
    a.error = std::move(error);
  }
  return a;
}

https_reply https_request(const std::string& method, const std::string& url, const std::string& content_type,
                          const std::string& body, std::size_t max_bytes, int timeout_ms) {
  https_reply a;
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u || ![u.scheme isEqualToString:@"https"] || (method != "GET" && method != "POST")) {
      a.error = "bad request";
      return a;
    }
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    config.HTTPShouldSetCookies = NO;
    config.URLCache = nil;
    config.URLCredentialStorage = nil;
    config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    config.timeoutIntervalForRequest = timeout_ms / 1000.0;
    config.timeoutIntervalForResource = timeout_ms / 1000.0;
    config.HTTPAdditionalHeaders = @{@"User-Agent" : @"driftwire"};
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config
                                                          delegate:[MVDriftwireUpdateRedirects new]
                                                     delegateQueue:nil];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
    req.HTTPMethod = [NSString stringWithUTF8String:method.c_str()];
    if (method == "POST") {
      req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
      [req setValue:[NSString stringWithUTF8String:content_type.c_str()] forHTTPHeaderField:@"Content-Type"];
    }
    __block https_reply got;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task =
        [session dataTaskWithRequest:req
                   completionHandler:^(NSData* data, NSURLResponse* response, NSError* err) {
                     if (err) {
                       got.error = err.localizedDescription ? std::string(err.localizedDescription.UTF8String)
                                                            : std::string("network error");
                     } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                       auto* http = static_cast<NSHTTPURLResponse*>(response);
                       got.status = static_cast<int>(http.statusCode);
                       const auto header = [&](NSString* name) {
                         NSString* v = [http valueForHTTPHeaderField:name];
                         return v ? std::string(v.UTF8String) : std::string();
                       };
                       got.location = header(@"Location");
                       got.nonce = header(@"Replay-Nonce");
                       const std::string retry = header(@"Retry-After");
                       if (!retry.empty() && retry.find_first_not_of("0123456789") == std::string::npos &&
                           retry.size() < 10) {
                         got.retry_after = std::stoll(retry);
                       }
                       if (data && data.length <= max_bytes) {
                         got.body.assign(static_cast<const char*>(data.bytes), data.length);
                       } else if (data) {
                         got.status = 0;
                         got.error = "too long";
                       }
                     }
                     dispatch_semaphore_signal(done);
                   }];
    [task resume];
    const dispatch_time_t wait =
        dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(timeout_ms + 1000) * static_cast<std::int64_t>(NSEC_PER_MSEC));
    if (dispatch_semaphore_wait(done, wait) != 0) {
      [task cancel];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      got = https_reply{};
      got.error = "timed out";
    }
    [session finishTasksAndInvalidate];
    a = std::move(got);
  }
  return a;
}

std::string self_path() {
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size + 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  char resolved[PATH_MAX];
  if (!::realpath(buf.data(), resolved)) return std::string(buf.data());
  return resolved;
}

std::string local_app_data() { return {}; }

}  // namespace mv::driftwire::cli
