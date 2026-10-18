// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-sharepoint-relay: a relay server of your own for Sharepoint (issue
// #472, option 10; docs/design/28 "Relays"). Run it on a machine both ends
// can reach (a VPS, a home server with a forwarded port), then type
// "<its address or name>:<port>" in Sharepoint's "Relay (your own server)".
//
// It pairs two TCP connections that present the same random token and
// copies bytes between them (src/addons/sharepoint/splice.h). The two
// MediaViewers' handshake and session run end to end through it: it never
// holds a key and sees only ciphertext. It prints counts, never an address
// or a token. No configuration file, no state on disk, no account.
//
//   mv-sharepoint-relay [--port N] [--max-pairs N] [--wait-s N] [--max-gb N]
//                       [--rate-mbs N] [--idle-s N] [--loopback]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "addons/sharepoint/splice.h"

namespace relay = mv::sharepoint::relay;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

bool number(const char* text, unsigned long long& out, unsigned long long max) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (*end != '\0' || v > max) return false;
  out = v;
  return true;
}

int usage() {
  std::fprintf(stderr,
               "usage: mv-sharepoint-relay [--port N] [--max-pairs N] [--wait-s N] [--max-gb N] [--rate-mbs N] "
               "[--idle-s N] [--loopback]\n"
               "  --port       TCP port, IPv6 and IPv4 (default %u)\n"
               "  --max-pairs  transfers relayed at once (default 8)\n"
               "  --wait-s     how long one side waits for the other (default 600)\n"
               "  --max-gb     bytes each way per transfer, in GiB (default 64)\n"
               "  --rate-mbs   MB/s each way per transfer, 0 for no cap (default 0)\n"
               "  --idle-s     a transfer silent this long ends (default 120)\n"
               "  --loopback   listen on 127.0.0.1 only, on a port the OS picks (tests)\n",
               static_cast<unsigned>(relay::kDefaultPort));
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  relay::server::options o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    unsigned long long v = 0;
    if (std::strcmp(a, "--loopback") == 0) {
      o.loopback_only = true;
      continue;
    }
    if (!next) return usage();
    ++i;
    if (std::strcmp(a, "--port") == 0 && number(next, v, 65535)) {
      o.port = static_cast<std::uint16_t>(v);
    } else if (std::strcmp(a, "--max-pairs") == 0 && number(next, v, 1024) && v > 0) {
      o.max_pairs = static_cast<int>(v);
    } else if (std::strcmp(a, "--wait-s") == 0 && number(next, v, 24 * 3600) && v > 0) {
      o.wait_ms = static_cast<int>(v * 1000);
    } else if (std::strcmp(a, "--max-gb") == 0 && number(next, v, 1ull << 20) && v > 0) {
      o.lim.max_bytes = v << 30;
    } else if (std::strcmp(a, "--rate-mbs") == 0 && number(next, v, 1ull << 20)) {
      o.lim.rate = v * 1000 * 1000;
    } else if (std::strcmp(a, "--idle-s") == 0 && number(next, v, 24 * 3600) && v > 0) {
      o.lim.idle_ms = static_cast<int>(v * 1000);
    } else {
      return usage();
    }
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  auto s = relay::server::start(o);
  if (!s) {
    std::fprintf(stderr, "mv-sharepoint-relay: could not listen (%s)\n", mv::status_name(s.error()));
    return 1;
  }
  std::printf("mv-sharepoint-relay: listening on port %u (%s)\n", static_cast<unsigned>((*s)->port()),
              o.loopback_only ? "127.0.0.1 only" : "IPv6 and IPv4");
  std::fflush(stdout);
  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto c = (*s)->snapshot();
  s->reset();
  std::printf("mv-sharepoint-relay: stopped; %llu relayed, %llu refused, %llu bytes\n",
              static_cast<unsigned long long>(c.paired), static_cast<unsigned long long>(c.refused),
              static_cast<unsigned long long>(c.bytes));
  return 0;
}
