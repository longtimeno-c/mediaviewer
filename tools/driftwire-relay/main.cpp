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
// copies bytes between them (src/wire/splice.h). The two
// MediaViewers' handshake and session run end to end through it: it never
// holds a key of theirs and sees only ciphertext. It prints counts, never an
// address or a token. No configuration file, no state on disk, no account.
//
// The Tunnel (2026-11-12): a key holder may also ask it to open a TCP
// connection to a web site and copy bytes both ways (splice.h, forwarding),
// so the person's own traffic leaves from here on a network that filters
// sites. Only to ports 80 and 443 unless --forward-ports says others ("none"
// turns it off), at most --max-forwards at once, public addresses only.
//
//   mv-driftwire-relay [--key-file PATH] [--port N] [--max-pairs N] [--wait-s N]
//                       [--max-gb N] [--rate-mbs N] [--idle-s N] [--loopback]
//                       [--forward-ports LIST|none] [--max-forwards N]
//                       [--tls-port N [--tls-cert PEM --tls-key PEM | --tls-name NAME] [--decoy-dir DIR]]
//                       [--forward-udp-ports LIST|none] [--dns ADDRESS[:PORT]]
// Slice 5 (the VPN): key holders' datagram flows to those UDP ports (53 by
// default), and their DNS to --dns (else this machine's first resolver).
// Slice 4: --tls-port (443) also serves the hop inside TLS behind a knock
// only a key holder can make; anything else gets a web site (tls.h).
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
#include <vector>

#include "wire/splice.h"

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

// "80,443": ports, 1..65535, at most 32; "none": empty.
bool port_list(const char* text, std::vector<std::uint16_t>& out) {
  out.clear();
  if (std::strcmp(text, "none") == 0) return true;
  std::string s(text);
  std::size_t at = 0;
  while (at <= s.size()) {
    const std::size_t comma = std::min(s.find(',', at), s.size());
    unsigned long long v = 0;
    if (!number(s.substr(at, comma - at).c_str(), v, 65535) || v == 0 || out.size() >= 32) return false;
    out.push_back(static_cast<std::uint16_t>(v));
    at = comma + 1;
  }
  return !out.empty();
}

// "1.1.1.1", "1.1.1.1:53", "[2606:4700::1111]:53".
bool dns_of(const char* text, relay::server::options& o) {
  std::string s(text);
  std::string host = s;
  unsigned long long port = 53;
  if (!s.empty() && s.front() == '[') {
    const auto close = s.find(']');
    if (close == std::string::npos) return false;
    host = s.substr(1, close - 1);
    if (close + 1 < s.size() && (s[close + 1] != ':' || !number(s.c_str() + close + 2, port, 65535))) return false;
  } else if (s.find(':') == s.rfind(':') && s.find(':') != std::string::npos) {
    host = s.substr(0, s.find(':'));
    if (!number(s.c_str() + s.find(':') + 1, port, 65535)) return false;
  }
  if (!mv::driftwire::net::parse_address(host) || port == 0) return false;
  o.dns_upstream = mv::driftwire::net::endpoint{host, static_cast<std::uint16_t>(port)};
  return true;
}

// A key typed by a person, not made here: guesses at it can only be tried
// live, one per connection, but a short one is still worth saying.
constexpr std::size_t kWarnBelowChars = 20;

int usage() {
  std::fprintf(stderr,
               "usage: mv-driftwire-relay [--key-file PATH] [--port N] [--max-pairs N] [--wait-s N] [--max-gb N] "
               "[--rate-mbs N] [--idle-s N] [--loopback] [--forward-ports LIST|none] [--max-forwards N] "
               "[--tls-port N] [--tls-cert PEM] [--tls-key PEM] [--tls-name NAME] [--decoy-dir DIR] "
               "[--forward-udp-ports LIST|none] [--dns ADDRESS[:PORT]]\n"
               "  --key-file   a file whose first line is the key (16+ characters); else MV_RELAY_KEY; else a\n"
               "               new one, printed once (--key KEY also works, but shows in the process list)\n"
               "  --port       TCP port, IPv6 and IPv4 (default %u)\n"
               "  --max-pairs  transfers relayed at once (default 8)\n"
               "  --wait-s     how long one side waits for the other (default 600)\n"
               "  --max-gb     bytes each way per transfer, in GiB (default 64)\n"
               "  --rate-mbs   MB/s each way per transfer, 0 for no cap (default 0)\n"
               "  --idle-s     a transfer silent this long ends (default 120)\n"
               "  --loopback   listen on 127.0.0.1 only, on a port the OS picks (tests)\n"
               "  --forward-ports  ports a key holder's Tunnel may reach, e.g. 80,443 (default), or none\n"
               "  --max-forwards   Tunnel connections at once (default 64)\n"
               "  --tls-port   also listen here (443) behind TLS, looking like a web site (Tunnel: Look like a web site)\n"
               "  --tls-cert, --tls-key  its certificate chain and key (PEM; a real one passes a look best)\n"
               "  --tls-name   else a self-signed certificate for this name\n"
               "  --decoy-dir  the web site's pages (index.html ...); else a small built-in page\n"
               "  --forward-udp-ports  UDP ports a key holder's VPN may reach (default 53), or none\n"
               "  --dns        the resolver the VPN's DNS goes to (default: this machine's first)\n",
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
    } else if (std::strcmp(a, "--forward-ports") == 0 && port_list(next, o.forward_ports)) {
      // set
    } else if (std::strcmp(a, "--max-forwards") == 0 && number(next, v, 4096) && v > 0) {
      o.max_forwards = static_cast<int>(v);
    } else if (std::strcmp(a, "--tls-port") == 0 && number(next, v, 65535) && v > 0) {
      o.tls_port = static_cast<std::uint16_t>(v);
    } else if (std::strcmp(a, "--tls-cert") == 0) {
      o.tls_cert = next;
    } else if (std::strcmp(a, "--tls-key") == 0) {
      o.tls_key = next;
    } else if (std::strcmp(a, "--tls-name") == 0) {
      o.tls_name = next;
    } else if (std::strcmp(a, "--decoy-dir") == 0) {
      o.decoy.dir = next;
    } else if (std::strcmp(a, "--forward-udp-ports") == 0 && port_list(next, o.forward_udp_ports)) {
      // set
    } else if (std::strcmp(a, "--dns") == 0 && dns_of(next, o)) {
      // set
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
  if (o.tls_port != 0) {
    std::printf("mv-driftwire-relay: also on port %u behind TLS (%s certificate), a web site to anyone without the key\n",
                static_cast<unsigned>((*s)->tls_port()), o.tls_cert.empty() ? "a self-signed" : "your");
  }
  if (o.forward_ports.empty()) {
    std::printf("mv-driftwire-relay: Tunnel forwarding off\n");
  } else {
    std::string ports;
    for (const auto p : o.forward_ports) ports += (ports.empty() ? "" : ",") + std::to_string(p);
    std::printf("mv-driftwire-relay: Tunnel forwarding for key holders to ports %s\n", ports.c_str());
  }
  std::fflush(stdout);
  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto c = (*s)->snapshot();
  s->reset();
  std::printf("mv-driftwire-relay: stopped; %llu relayed, %llu forwarded, %llu web pages, %llu refused, %llu bytes\n",
              static_cast<unsigned long long>(c.paired), static_cast<unsigned long long>(c.forwarded),
              static_cast<unsigned long long>(c.website),
              static_cast<unsigned long long>(c.refused + c.forward_refused), static_cast<unsigned long long>(c.bytes));
  return 0;
}
