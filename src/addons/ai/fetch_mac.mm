// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model downloader's transport on the Mac (fetch.h): NSURLSession, an
// ephemeral session (no cookies, no cache, no credentials), streaming each
// chunk to the fetcher on the calling thread's behalf. Sends a User-Agent
// and, when resuming, a Range header; nothing else.
#include "addons/ai/fetch.h"

#import <Foundation/Foundation.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace mv::ai {
namespace {

// What the delegate hands the blocked caller: the head, then chunks, then the end.
struct exchange {
  std::mutex m;
  std::condition_variable cv;
  int status = -1;  // -1 until the response arrives
  std::deque<std::vector<std::uint8_t>> chunks;
  bool finished = false;
  std::string error;
  bool stop = false;  // the caller wants no more
};

}  // namespace
}  // namespace mv::ai

@interface MVFetchDelegate : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, assign) mv::ai::exchange* x;
@end

@implementation MVFetchDelegate
- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
  const int code = [response isKindOfClass:[NSHTTPURLResponse class]]
                       ? static_cast<int>(static_cast<NSHTTPURLResponse*>(response).statusCode)
                       : 0;
  {
    std::lock_guard lock(_x->m);
    _x->status = code;
  }
  _x->cv.notify_all();
  done((code == 200 || code == 206) ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
  std::unique_lock lock(_x->m);
  if (_x->stop) {
    [task cancel];
    return;
  }
  const auto* p = static_cast<const std::uint8_t*>(data.bytes);
  _x->chunks.emplace_back(p, p + data.length);
  // Back-pressure: a slow disk must not pile gigabytes up in memory.
  _x->cv.notify_all();
  _x->cv.wait(lock, [self] { return _x->chunks.size() < 64 || _x->stop; });
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  {
    std::lock_guard lock(_x->m);
    if (error && error.code != NSURLErrorCancelled) {
      _x->error = error.localizedDescription ? std::string(error.localizedDescription.UTF8String) : "network error";
    }
    _x->finished = true;
  }
  _x->cv.notify_all();
}
@end

namespace mv::ai {
namespace {

class mac_transport final : public http_transport {
 public:
  http_reply get(const std::string& url, std::uint64_t from, const std::function<bool(int)>& head,
                 const std::function<bool(std::span<const std::uint8_t>)>& body) override {
    http_reply reply;
    exchange x;
    @autoreleasepool {
      NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
      config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
      config.HTTPShouldSetCookies = NO;
      config.URLCache = nil;
      config.URLCredentialStorage = nil;
      config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
      config.timeoutIntervalForRequest = 60;
      config.HTTPAdditionalHeaders = @{@"User-Agent" : @"MediaViewer"};
      MVFetchDelegate* delegate = [MVFetchDelegate new];
      delegate.x = &x;
      NSOperationQueue* queue = [NSOperationQueue new];
      queue.maxConcurrentOperationCount = 1;
      NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
      NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
      if (!u) {
        reply.error = "bad URL";
        [session invalidateAndCancel];
        return reply;
      }
      NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
      if (from > 0) {
        [req setValue:[NSString stringWithFormat:@"bytes=%llu-", static_cast<unsigned long long>(from)]
            forHTTPHeaderField:@"Range"];
      }
      NSURLSessionDataTask* task = [session dataTaskWithRequest:req];
      [task resume];

      bool wanted = false;
      std::unique_lock lock(x.m);
      x.cv.wait(lock, [&] { return x.status >= 0 || x.finished; });
      reply.status = x.status < 0 ? 0 : x.status;
      if (x.status >= 0) {
        lock.unlock();
        wanted = head(x.status);
        lock.lock();
      }
      if (!wanted) x.stop = true;
      while (wanted) {
        x.cv.wait(lock, [&] { return !x.chunks.empty() || x.finished; });
        if (x.chunks.empty() && x.finished) break;
        std::vector<std::uint8_t> chunk = std::move(x.chunks.front());
        x.chunks.pop_front();
        x.cv.notify_all();
        lock.unlock();
        const bool more = body(chunk);
        lock.lock();
        if (!more) {
          x.stop = true;
          x.cv.notify_all();
          break;
        }
      }
      x.stop = true;
      x.cv.notify_all();
      lock.unlock();
      [task cancel];
      lock.lock();
      x.cv.wait(lock, [&] { return x.finished; });
      reply.error = x.error;
      if (!x.error.empty() && reply.status == 0) reply.status = 0;
      lock.unlock();
      [session finishTasksAndInvalidate];
    }
    return reply;
  }
};

}  // namespace

std::unique_ptr<http_transport> make_http_transport() { return std::make_unique<mac_transport>(); }

}  // namespace mv::ai
