// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-driftwire-relay: a relay server of your own for Driftwire (issue
// #472, option 10; docs/design/28 "Relays"). Run it on a machine both ends
// can reach (a VPS, a home server with a forwarded port), then type
// "<its address or name>:<port>#<key>" in Driftwire's "Relay (your own
// server)". Only a holder of the key can wait there, so it relays for your
// codes and nobody else's; the key is never in a code, and never on the wire
// (#500: CPace runs on it, splice.h).
//
// It pairs two TCP connections that present the same random token and
// copies bytes between them (src/addons/driftwire/splice.h). The two
// MediaViewers' handshake and session run end to end through it: it never
// holds a key of theirs and sees only ciphertext. It prints counts, never an
// address or a token. No configuration file, no state on disk, no account.
//
//   mv-driftwire-relay [--key-file PATH] [--port N] [--max-pairs N] [--wait-s N]
//                       [--max-gb N] [--rate-mbs N] [--idle-s N] [--loopback]
// The key: the first line of --key-file, else MV_RELAY_KEY, else --key
// (which other users can see in the process list), else a new one, printed
// once; a key given is never printed (#500: service logs keep what is).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "addons/driftwire/splice.h"

namespace relay = mv::driftwire::relay;

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

// The first line of `path`, without its line end.
std::optional<std::string> first_line(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return std::nullopt;
  std::string out;
  for (int c = std::fgetc(f); c != EOF && c != '\n' && c != '\r' && out.size() < 4096; c = std::fgetc(f)) {
    out.push_back(static_cast<char>(c));
  }
  std::fclose(f);
  return out;
}

// A key typed by a person, not made here: guesses at it can only be tried
// live, one per connection, but a short one is still worth saying.
constexpr std::size_t kWarnBelowChars = 20;

int usage() {
  std::fprintf(stderr,
               "usage: mv-driftwire-relay [--key-file PATH] [--port N] [--max-pairs N] [--wait-s N] [--max-gb N] "
               "[--rate-mbs N] [--idle-s N] [--loopback]\n"
               "  --key-file   a file whose first line is the key (16+ characters); else MV_RELAY_KEY; else a\n"
               "               new one, printed once (--key KEY also works, but shows in the process list)\n"
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
  std::string key_from_args;
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
    if (std::strcmp(a, "--key") == 0 && std::strlen(next) >= relay::kMinKeyChars) {
      key_from_args = next;
    } else if (std::strcmp(a, "--key-file") == 0) {
      const auto k = first_line(next);
      if (!k || k->size() < relay::kMinKeyChars) {
        std::fprintf(stderr, "mv-driftwire-relay: --key-file needs a readable file whose first line is 16+ characters\n");
        return 2;
      }
      o.key = *k;
    } else if (std::strcmp(a, "--port") == 0 && number(next, v, 65535)) {
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
  bool made = false;
  if (o.key.empty()) {
    const char* env = std::getenv("MV_RELAY_KEY");
    if (env && std::strlen(env) >= relay::kMinKeyChars) {
      o.key = env;
    } else if (!key_from_args.empty()) {
      o.key = key_from_args;
      std::fprintf(stderr, "mv-driftwire-relay: --key shows in the process list to other users of this machine; "
                           "prefer --key-file or MV_RELAY_KEY\n");
    } else {
      o.key = relay::random_key();
      made = true;
    }
  }
  if (!made && o.key.size() < kWarnBelowChars) {
    std::fprintf(stderr, "mv-driftwire-relay: the key is short (%zu characters); anyone may try guesses at it live. "
                         "Run once without a key for a made one (26 random characters)\n", o.key.size());
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  auto s = relay::server::start(o);
  if (!s) {
    std::fprintf(stderr, "mv-driftwire-relay: could not listen (%s)\n", mv::status_name(s.error()));
    return 1;
  }
  std::printf("mv-driftwire-relay: listening on port %u (%s)\n", static_cast<unsigned>((*s)->port()),
              o.loopback_only ? "127.0.0.1 only" : "IPv6 and IPv4");
  // A key made here, once, on the person's own terminal: what they type
  // after '#'. One they gave is theirs already, and is never printed.
  if (made) {
    std::printf("mv-driftwire-relay: in Driftwire type <this machine's address or name>:%u#%s\n"
                "mv-driftwire-relay: this key is shown only now; keep it in a file for --key-file\n",
                static_cast<unsigned>((*s)->port()), o.key.c_str());
  } else {
    std::printf("mv-driftwire-relay: in Driftwire type <this machine's address or name>:%u#<your key>\n",
                static_cast<unsigned>((*s)->port()));
  }
  std::fflush(stdout);
  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto c = (*s)->snapshot();
  s->reset();
  std::printf("mv-driftwire-relay: stopped; %llu relayed, %llu refused, %llu bytes\n",
              static_cast<unsigned long long>(c.paired), static_cast<unsigned long long>(c.refused),
              static_cast<unsigned long long>(c.bytes));
  return 0;
}
