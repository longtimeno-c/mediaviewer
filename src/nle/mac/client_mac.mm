// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-search-client: the search agent's test client (docs/design/23 Phase 1).
//
//   mv-search-client QUERY [--json] [--thumbs]         one search over XPC
//   mv-search-client --bench N QUERY...                 p50 / p95 over XPC and
//                                                       in this process
//   mv-search-client --wait-exit SECONDS                exit timing: 0 when the
//                                                       agent's service went away
//
// --bench runs each query N times through the agent and N times through the
// same reader loaded here (search_session::open), warm, alternating, and
// prints the two latency distributions: the verify line's "query p95 in the
// agent at most in-app p95 + 5 ms". It needs the agent registered (launchd
// or SMAppService); a signed client of the agent's team.
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_addon.h>

#include "addon/manifest.h"
#include "addon/store.h"
#include "nle/search_session.h"
#include "nle/search_wire.h"
#include "core/json.h"
#include "io/paths.h"
#import "nle/mac/agent_protocol.h"

namespace {

using clock_type = std::chrono::steady_clock;

NSXPCConnection* connect() {
  NSXPCConnection* c = [[NSXPCConnection alloc] initWithMachServiceName:@MV_FCP_MACH_SERVICE options:0];
  c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MVSearchAgent)];
  [c resume];
  return c;
}

// One search over XPC, synchronously; nil on a connection error.
NSData* ask(NSXPCConnection* c, const mv::nle::request& r, const std::string& q) {
  __block NSData* out = nil;
  __block bool failed = false;
  id<MVSearchAgent> agent = [c synchronousRemoteObjectProxyWithErrorHandler:^(NSError* e) {
    (void)e;
    failed = true;
  }];
  const auto req = mv::nle::encode(r);
  [agent run:[NSData dataWithBytes:req.data() length:req.size()]
          text:[NSString stringWithUTF8String:q.c_str()]
      scopeDir:@""
     withReply:^(NSData* reply) {
       out = reply;
     }];
  return failed ? nil : out;
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
  return v[std::min(i, v.size() - 1)];
}

int bench(int n, const std::vector<std::string>& queries) {
  auto root = mv::io::addons_dir();
  auto thumbs = mv::io::thumb_cache_dir();
  if (!root || !thumbs) return 2;
  const auto key = mv::addon::pinned_public_key();
  mv::addon::store store(*root, std::vector<std::uint8_t>(key.begin(), key.end()), MV_ADDON_HOST_API);
  auto local = mv::nle::search_session::open(store, *thumbs);
  if (!local || !(*local)->wait_ready(300000)) {
    std::fprintf(stderr, "in-process reader did not load\n");
    return 2;
  }
  NSXPCConnection* c = connect();
  mv::nle::request r;
  // Warm both: the agent's load, the text cache of each query.
  for (const std::string& q : queries) {
    for (int i = 0; i < 3; ++i) {
      NSData* d = ask(c, r, q);
      if (!d) {
        std::fprintf(stderr, "agent unreachable\n");
        return 2;
      }
      (void)(*local)->run(r, q, "", 30000);
    }
  }
  std::vector<double> agent_ms, local_ms;
  bool same = true;
  for (int i = 0; i < n; ++i) {
    for (const std::string& q : queries) {
      r.correlation_id = static_cast<std::uint64_t>(i + 1);
      auto t0 = clock_type::now();
      NSData* d = ask(c, r, q);
      auto t1 = clock_type::now();
      const mv::nle::reply mine = (*local)->run(r, q, "", 30000);
      auto t2 = clock_type::now();
      agent_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      local_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
      auto theirs = mv::nle::decode(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(d.bytes), d.length));
      if (!theirs || theirs->rows.size() != mine.rows.size()) {
        same = false;
        continue;
      }
      for (std::size_t k = 0; k < mine.rows.size(); ++k) {
        if (theirs->rows[k].path != mine.rows[k].path || theirs->rows[k].pts_ms != mine.rows[k].pts_ms) same = false;
      }
    }
  }
  [c invalidate];
  mv::json::writer w;
  w.begin_object();
  w.key("queries").integer(static_cast<std::int64_t>(queries.size()));
  w.key("runs").integer(n);
  w.key("agent_p50_ms").number(pct(agent_ms, 0.50));
  w.key("agent_p95_ms").number(pct(agent_ms, 0.95));
  w.key("in_process_p50_ms").number(pct(local_ms, 0.50));
  w.key("in_process_p95_ms").number(pct(local_ms, 0.95));
  w.key("same_results").boolean(same);
  w.end_object();
  std::printf("%s\n", w.str().c_str());
  return same ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() >= 3 && args[0] == "--bench") {
      return bench(std::stoi(args[1]), std::vector<std::string>(args.begin() + 2, args.end()));
    }
    if (args.size() == 2 && args[0] == "--wait-exit") {
      // Polls for a running agent process (MediaViewer in agent mode, not the
      // viewer); exits 0 once it is gone.
      const int limit = std::stoi(args[1]);
      for (int s = 0; s <= limit; ++s) {
        if (system("pgrep -f '(^|/)MediaViewer --search-agent$' >/dev/null") != 0) {
          std::printf("{\"exited_after_s\":%d}\n", s);
          return 0;
        }
        [NSThread sleepForTimeInterval:1.0];
      }
      std::printf("{\"exited_after_s\":-1}\n");
      return 1;
    }
    std::string query;
    bool json = false, thumbs = false;
    for (const std::string& a : args) {
      if (a == "--json") json = true;
      else if (a == "--thumbs") thumbs = true;
      else query = a;
    }
    if (query.empty()) {
      std::fputs("usage: mv-search-client QUERY [--json] [--thumbs] | --bench N QUERY... | --wait-exit S\n", stderr);
      return 2;
    }
    NSXPCConnection* c = connect();
    mv::nle::request r;
    r.correlation_id = 1;
    NSData* d = ask(c, r, query);
    if (!d) {
      std::fputs("agent unreachable\n", stderr);
      return 2;
    }
    auto rep = mv::nle::decode(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(d.bytes), d.length));
    if (!rep) {
      std::fputs("bad reply\n", stderr);
      return 2;
    }
    if (rep->code != mv::status::ok) {
      std::fprintf(stderr, "search: %s\n", mv::status_name(rep->code));
      return 2;
    }
    __block std::size_t thumb_bytes = 0;
    if (thumbs) {
      id<MVSearchAgent> agent = [c synchronousRemoteObjectProxyWithErrorHandler:^(NSError*) {}];
      for (const auto& x : rep->rows) {
        if (x.thumb.empty()) continue;
        [agent thumbnail:[NSString stringWithUTF8String:x.thumb.c_str()]
               withReply:^(NSData* jpeg) {
                 thumb_bytes += jpeg.length;
               }];
      }
    }
    [c invalidate];
    if (json) {
      mv::json::writer w;
      w.begin_object();
      w.key("rows").integer(static_cast<std::int64_t>(rep->rows.size()));
      w.key("thumb_bytes").integer(static_cast<std::int64_t>(thumb_bytes));
      w.key("results").begin_array();
      for (const auto& x : rep->rows) {
        w.begin_object();
        w.key("path").string(x.path);
        w.key("pts_ms").integer(x.pts_ms);
        w.key("score").number(x.score);
        w.end_object();
      }
      w.end_array();
      w.end_object();
      std::printf("%s\n", w.str().c_str());
    } else {
      std::printf("%zu results, %zu thumbnail bytes\n", rep->rows.size(), thumb_bytes);
    }
    return rep->rows.empty() ? 1 : 0;
  }
}
