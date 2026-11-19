// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host_setup.h"

#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/data_dir.h"
#include "addons/driftwire/cli/host_keepers.h"
#include "addons/driftwire/cli/qr.h"
#include "addons/driftwire/cli/serve_tty.h"
#include "addons/driftwire/cli/update.h"
#include "core/json.h"
#include "wire/pair_check.h"
#include "wire/serve.h"
#include "wire/splice.h"
#include "wire/tls.h"

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::int64_t now_of(const setup_world& w) {
  return w.now_unix ? w.now_unix() : static_cast<std::int64_t>(std::time(nullptr));
}

void pause_ms(const setup_world& w, int ms) {
  if (w.sleep) {
    w.sleep(ms);
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
}

void wipe(std::string& s) {
  if (!s.empty()) sodium_memzero(s.data(), s.size());
  s.clear();
}

std::string hex_of(const std::uint8_t* p, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kHex[p[i] >> 4];
    out[2 * i + 1] = kHex[p[i] & 15];
  }
  return out;
}

std::string with_port(const net::address& a, std::uint16_t port) {
  const std::string t = net::address_text(a);
  return (a.v6 ? "[" + t + "]" : t) + ":" + std::to_string(port);
}

// A yes or no; `yes_first`: Enter alone is yes.
bool ask_yes(setup_io& io, const std::string& prompt, bool yes_first) {
  const auto a = io.ask(prompt + (yes_first ? " [Y/n] " : " [y/N] "), false);
  if (!a) return false;
  if (a->empty()) return yes_first;
  const char c = (*a)[0];
  return c == 'y' || c == 'Y';
}

ddns::fetch_fn fetch_of(const setup_world& w) {
  if (w.fetch) return w.fetch;
  return [](const std::string& url, int ms) {
    auto a = https_get(url, ddns::kMaxAnswer, ms);
    return ddns::fetch_answer{a.status, std::move(a.body)};  // never a.error: it can hold the URL, and so the token
  };
}

bool user_root(const serve::root_config& r) { return r.label != serve::kCatalogueLabel; }

std::size_t user_roots(const serve::config& c) {
  std::size_t n = 0;
  for (const auto& r : c.roots) n += user_root(r) ? 1 : 0;
  return n;
}

std::int64_t state_number(const std::optional<json::value>& st, const char* key) {
  return st ? st->integer(key).value_or(0) : 0;
}

// This computer on the home network: the address toward the gateway, else the
// first private IPv4 one.
std::optional<net::address> lan_address(const setup_world& w) {
  if (w.loopback) return net::parse_address("127.0.0.1");
  if (w.lan) return w.lan;
  if (const auto gw = net::default_gateway()) {
    if (auto a = net::local_address_toward(*gw)) return a;
  }
  for (const auto& a : net::local_addresses()) {
    if (!a.v6 && net::is_private_v4(a)) return a;
  }
  return std::nullopt;
}

// The host library: its id (host.json) and its publisher key (the host's
// secrets), and its library.json in the Catalogue folder. False with `why`.
bool host_library(const host_layout& l, host_settings& s, std::array<std::uint8_t, 32>& publisher, std::string& why) {
  auto store = host_files::store(l.folder + "/secrets");
  if (!store) {
    why = "the host's secrets folder must be this user's alone (mode 700).";
    return false;
  }
  std::array<std::uint8_t, 32> seed{};
  if (store->exists(kHostPublisherSecret)) {
    auto v = store->get(kHostPublisherSecret);
    if (!v || v->size() != seed.size()) {
      why = "the host library's key cannot be read.";
      return false;
    }
    std::copy(v->begin(), v->end(), seed.begin());
    sodium_memzero(v->data(), v->size());
  } else {
    randombytes_buf(seed.data(), seed.size());
    if (!store->put(kHostPublisherSecret, seed)) {
      why = "could not keep the host library's key.";
      return false;
    }
  }
  std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_seed_keypair(publisher.data(), sk.data(), seed.data());
  sodium_memzero(sk.data(), sk.size());
  sodium_memzero(seed.data(), seed.size());
  if (s.library_id.empty()) {
    std::uint8_t id[16];
    randombytes_buf(id, sizeof id);
    s.library_id = hex_of(id, sizeof id);
    if (!host_files::save_settings(l, s)) {
      why = "could not write host.json.";
      return false;
    }
  }
  // library.json, as the desktop writes it on a NAS share before anything is
  // published: the phone reads its library_id only (file mode).
  const std::string dir = host_catalogue_of(l);
  std::error_code ec;
  fs::create_directories(to_path(dir), ec);
  const std::string file = dir + "/library.json";
  json::writer w;
  w.begin_object()
      .key("format").integer(1)
      .key("library_id").string(s.library_id)
      .key("public_key").string(hex_of(publisher.data(), publisher.size()))
      .key("epoch").integer(1)
      .key("chain").begin_array().end_array();
  w.end_object();
  const std::string text = w.take();
  if (host_files::read(file, 64 * 1024).value_or(std::string()) != text && !host_files::write(file, text)) {
    why = "could not write the host library's library.json.";
    return false;
  }
  // The Catalogue folder in serve.json (as a NAS claim adds it).
  const auto lock = host_files::lock_config(l);
  if (!lock) {
    why = "serve.json is being changed by something else; try again.";
    return false;
  }
  auto cfg = host_files::config(l, true);
  if (!cfg) {
    why = "serve.json is not valid; not changing it.";
    return false;
  }
  const bool there = std::any_of(cfg->roots.begin(), cfg->roots.end(),
                                 [&](const serve::root_config& r) { return r.path == dir; });
  // A desktop claimed this service: its library is the phones', and a second one beside it would be
  // refused by a phone once the desktop publishes (mobile/sync.cpp find_root).
  if (std::any_of(cfg->roots.begin(), cfg->roots.end(), [&](const serve::root_config& r) {
        return r.label == serve::kCatalogueLabel && r.path != dir;
      })) {
    why = "A desktop has claimed this service: pair phones from its Settings, Phone access, so they get its library.";
    return false;
  }
  if (!there) {
    if (cfg->roots.size() >= serve::kMaxRoots) {
      why = "32 folders are shared already; unshare one first.";
      return false;
    }
    serve::root_config rc;
    rc.id = host_files::new_id();
    rc.label = serve::kCatalogueLabel;
    rc.path = dir;
    cfg->roots.push_back(std::move(rc));
    if (!host_files::write(l.serve_config, serve::config_json(*cfg))) {
      why = "could not write serve.json.";
      return false;
    }
  }
  return true;
}

std::string pair_url_of(const std::string& library_id, const std::string& library_name,
                        const std::array<std::uint8_t, 32>& publisher, const host_pair_code& p,
                        const std::array<std::uint8_t, 32>& identity, const std::array<std::uint8_t, 32>& access,
                        const std::string& code) {
  // catalog::pair_url's payload, field for field (src/catalogue/catalog_chain.cpp).
  json::writer w;
  w.begin_object();
  w.key("v").integer(1);
  w.key("library_id").string(library_id);
  w.key("library_name").string(library_name);
  w.key("publisher_key").string(hex_of(publisher.data(), publisher.size()));
  w.key("service").begin_object();
  w.key("local").string(p.local);
  w.key("public").string(p.public_name);
  w.key("identity").string(hex_of(identity.data(), identity.size()));
  w.end_object();
  w.key("access_key").string(hex_of(access.data(), access.size()));
  w.key("code").string(code);
  w.key("expires").integer(p.expires);
  w.end_object();
  std::string text = w.take();
  std::string b64(sodium_base64_ENCODED_LEN(text.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(b64.data(), b64.size(), reinterpret_cast<const unsigned char*>(text.data()), text.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  b64.resize(std::char_traits<char>::length(b64.c_str()));
  wipe(text);
  return "mediaviewer://pair#" + b64;
}

// The router line of a running host (state.json's keepers), as `host status` shows it.
std::string running_router_line(const std::optional<json::value>& st) {
  if (!st) return {};
  const json::value* k = st->find("keepers");
  if (!k || k->k != json::kind::array) return {};
  for (const auto& line : k->a) {
    if (line.k == json::kind::string && line.s.rfind("router", 0) == 0) return line.s;
  }
  return {};
}

// ---- the steps ------------------------------------------------------------------------------

// 1. A DuckDNS name: true when one is set (now or before).
bool name_step(const host_layout& l, setup_io& io, const setup_world& w, bool in_setup) {
  host_settings s = host_files::settings(l);
  auto store = host_files::store(l.folder + "/secrets");
  if (!store) {
    io.say("The host's secrets folder must be this user's alone (mode 700).\n");
    return false;
  }
  if (in_setup) {
    io.say("\n1. A name\n"
           "   With a free DuckDNS name (duckdns.org), phones and your laptop find this computer away from\n"
           "   home, even when the home's address changes. DuckDNS learns your home's address, nothing about\n"
           "   your files. Without a name, phones reach this computer at home only.\n");
  }
  if (in_setup && !s.ddns.empty() && ddns::load_token(*store)) {
    if (ask_yes(io, "   Keep " + ddns::duckdns_name(s.ddns) + "?", true)) return true;
  }
  if (w.loopback && !w.fetch) {
    io.say("   The test build never reaches duckdns.org: no name.\n");
    return false;
  }
  const ddns::fetch_fn fetch = fetch_of(w);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto typed = io.ask("   Your DuckDNS subdomain (\"home\" for home.duckdns.org; Enter alone to skip): ", false);
    if (!typed || typed->empty()) {
      io.say("   No name: phones reach this computer at home only (driftwire host name duckdns adds one later).\n");
      return false;
    }
    const auto sub = ddns::parse_subdomain(*typed);
    if (!sub) {
      io.say("   That is not a DuckDNS subdomain: letters, digits and '-', as in home or home.duckdns.org.\n");
      continue;
    }
    auto pasted = io.ask("   Its token (the one duckdns.org shows once you sign in; typed or pasted, not shown): ", true);
    if (!pasted) return false;
    auto token = ddns::parse_token(*pasted);
    wipe(*pasted);
    if (!token) {
      io.say("   That is not a DuckDNS token (it looks like 8-4-4-4-12 letters and digits).\n");
      continue;
    }
    const std::string name = ddns::duckdns_name(*sub);
    io.say("   Asking DuckDNS to point " + name + " at this home...\n");
    ddns::request r;
    r.subdomain = *sub;
    r.token = *token;
    r.clear_ipv6 = true;  // a name used before may hold an IPv6 address that does not answer here
    ddns::duckdns dns(fetch, w.ddns_base);
    const ddns::answer a = dns.update(r);
    wipe(r.token);
    if (a.what != ddns::outcome::ok) {
      wipe(*token);
      switch (a.what) {
        case ddns::outcome::refused:
          io.say("   DuckDNS said no: check the subdomain and the token.\n");
          break;
        case ddns::outcome::no_answer:
          io.say("   DuckDNS did not answer: is this computer online?\n");
          break;
        default:
          io.say("   Something other than DuckDNS answered (a sign-in page on this network?).\n");
          break;
      }
      continue;
    }
    if (!ddns::save_token(*store, *token)) {
      wipe(*token);
      io.say("   Could not keep the token.\n");
      return false;
    }
    wipe(*token);
    s = host_files::settings(l);
    s.ddns = *sub;
    s.ddns_stamp = now_of(w);
    if (!host_files::save_settings(l, s)) {
      io.say("   Could not write host.json.\n");
      return false;
    }
    io.say("   DuckDNS points " + name + " at this home. The host keeps it up to date while it runs.\n");
    // The check: the name resolves to what DuckDNS set (its TTL is 60 s).
    bool shown = false;
    for (int i = 0; i < std::max(1, w.name_tries); ++i) {
      if (i == 1) io.say("   Waiting for the name to show (it can take a minute or so)...\n");
      if (i > 0) pause_ms(w, w.name_wait_ms);
      if (ddns::check_name(name, a, w.resolve).ok()) {
        shown = true;
        break;
      }
    }
    io.say(shown ? "   Checked: " + name + " finds this home.\n"
                 : "   " + name + " does not show this home's address yet; give it a few minutes "
                       "(driftwire host status says when it last updated).\n");
    return true;
  }
  io.say("   No name for now (driftwire host name duckdns tries again).\n");
  return false;
}

// 2. The folders.
void folder_step(const host_layout& l, setup_io& io, const setup_world& w) {
  io.say("\n2. A folder or drive for phones\n");
  auto cfg = host_files::config(l, w.any_port);
  if (!cfg) {
    io.say("   serve.json is not valid; fix or remove it, then run setup again.\n");
    return;
  }
  std::string now;
  for (const auto& r : cfg->roots) {
    if (user_root(r)) now += (now.empty() ? "" : ", ") + r.label + (r.backup ? " (backup)" : "");
  }
  if (!now.empty()) io.say("   Shared now: " + now + ".\n");
  for (;;) {
    const auto typed = io.ask(now.empty() ? "   A folder to give phones (e.g. /Volumes/Photos or D:\\Photos; Enter alone to skip): "
                                          : "   Another folder (Enter alone when done): ",
                              false);
    if (!typed || typed->empty()) break;
    std::string label;
    const std::string why = host_files::share(l, *typed, "", false, 0, w.any_port, label);
    if (!why.empty()) {
      io.say("   " + why + "\n");
      continue;
    }
    io.say("   Shared \"" + label + "\" (driftwire host share PATH --label L names it otherwise).\n");
    now += (now.empty() ? "" : ", ") + label;
  }
  const auto backup = io.ask("   A folder phones may back up into (Enter alone for none): ", false);
  if (!backup || backup->empty()) return;
  std::uint64_t limit = 0;
  for (;;) {
    const auto gb = io.ask("   The most it may hold, in GB (Enter alone for no limit; 1 GB is always left free): ", false);
    if (!gb || gb->empty()) break;
    char* end = nullptr;
    const double v = std::strtod(gb->c_str(), &end);
    if (end && *end == '\0' && v > 0 && v < 1e6) {
      limit = static_cast<std::uint64_t>(v * 1e9);
      break;
    }
    io.say("   A number of GB, like 200.\n");
  }
  std::string label;
  const std::string why = host_files::share(l, *backup, "", true, limit, w.any_port, label);
  io.say(why.empty() ? "   Phones may back up into \"" + label + "\".\n" : "   " + why + "\n");
}

// 3. The router.
void router_step(const host_layout& l, setup_io& io, const setup_world& w) {
  io.say("\n3. The router\n");
  const auto cfg = host_files::config(l, w.any_port);
  const host_settings s = host_files::settings(l);
  if (host_files::running(l)) {
    const std::string line = running_router_line(host_files::state(l));
    io.say(line.empty() ? "   The running host asks the router itself; driftwire host status shows what it said.\n"
                        : "   The running host asked it:\n   " + line + "\n");
  } else if (!w.ask_router || (w.loopback && !w.router.router.gateway)) {
    io.say("   The router was not asked (the test build never asks a real one).\n");
  } else {
    keeper_context ctx;
    ctx.service_port = cfg && user_roots(*cfg) > 0 ? cfg->port : 0;
    ctx.relay_port = s.relay_port;
    ctx.relay_tls_port = look_port(s);
    ctx.vpn = s.vpn;
    auto want = wanted_ports(ctx);
    if (want.empty()) {
      io.say("   Nothing to open yet: no folder is shared and the VPN is off.\n");
    } else {
      io.say("   Asking it to forward " + std::string(want.size() > 1 ? "the ports" : "the port") + " to this computer...\n");
      host_ports::options o = w.router;
      o.ledger = l.folder + "/setup-router.ledger";
      host_ports::keeper probe(std::move(want), std::move(o));
      const host_ports::report r = probe.status();
      for (const auto& p : r.ports) {
        if (p.v4 == host_ports::port_state::mapped) {
          io.say("   The router forwards " + p.wanted.what + " port " + std::to_string(p.external_port) + " here (" +
                 mapping::method_name(p.how) + ").\n");
        }
      }
      const auto advice = host_ports::advice(r);
      for (const auto& a : advice) io.say("   " + a + "\n");
      if (advice.empty()) io.say("   While the host runs it keeps them open; they close when it stops.\n");
      if (r.kind == host_ports::outside_kind::carrier_nat || r.kind == host_ports::outside_kind::double_nat) {
        io.say("   Phones still reach this computer at home.\n");
      }
    }  // the probe's forwards removed here: the running host asks for its own
  }
  io.say("   The real test, once a phone is paired: turn its Wi-Fi off and open MediaViewer on mobile data.\n");
}

// The QR and the words around it.
int show_pair(const host_layout& l, setup_io& io, const setup_world& w) {
  std::string why;
  auto p = make_host_pair(l, w, why);
  if (!p) {
    io.say("   " + why + "\n");
    return 1;
  }
  const auto q = qr_code::encode(p->url);
  wipe(p->url);  // it holds the access key and the code: the QR on screen is the one copy
  if (!q) {
    io.say("   The pairing code is too long for a QR.\n");
    return 1;
  }
  io.say("\n");
  for (const auto& line : qr_terminal_lines(*q, io.colour)) io.say("  " + line + "\n");
  io.say("\n   Scan it with MediaViewer on the phone. The phone then shows a check: it must be " + p->check + ".\n");
  io.say("   The code works once, for 15 minutes (driftwire host pair makes another). If the QR looks broken,\n"
         "   make the window at least " + std::to_string(q->size() + 6) + " columns wide.\n");
  if (p->public_name.empty()) io.say("   Without a DuckDNS name the phone finds this computer at home only.\n");
  if (!host_files::running(l)) io.say("   Start the host (driftwire host) before scanning, so the phone finds it.\n");
  return 0;
}

}  // namespace

// ---- the pieces -----------------------------------------------------------------------------

std::optional<host_pair_code> make_host_pair(const host_layout& l, const setup_world& w, std::string& why) {
  if (sodium_init() < 0) {
    why = "could not start its cryptography.";
    return std::nullopt;
  }
  if (!make_host_folders(l)) {
    why = "its folder must be this user's alone (mode 700).";
    return std::nullopt;
  }
  auto cfg = host_files::config(l, w.any_port);
  if (!cfg) {
    why = "serve.json is not valid; not changing it.";
    return std::nullopt;
  }
  if (user_roots(*cfg) == 0) {
    why = "No folder is shared yet: a phone would have nothing to open (driftwire host share PATH).";
    return std::nullopt;
  }
  const std::string mine = host_catalogue_of(l);
  if (std::any_of(cfg->roots.begin(), cfg->roots.end(), [&](const serve::root_config& r) {
        return r.label == serve::kCatalogueLabel && r.path != mine;
      })) {
    why = "A desktop has claimed this service: pair phones from its Settings, Phone access, so they get its library.";
    return std::nullopt;
  }
  host_settings s = host_files::settings(l);
  std::array<std::uint8_t, 32> publisher{};
  if (!host_library(l, s, publisher, why)) return std::nullopt;
  auto store = host_files::store(l.serve + "/secrets");
  if (!store) {
    why = "the service's secrets folder must be this user's alone (mode 700).";
    return std::nullopt;
  }
  std::array<std::uint8_t, 32> identity{}, access{};
  bool made = false;
  if (!serve::set_up(*store, identity, access, made)) {
    why = "the service's keys cannot be read or made.";
    return std::nullopt;
  }
  std::unique_ptr<net::instance_lock> devices;
  for (int i = 0; i < 30 && !devices; ++i) {
    devices = net::try_lock_instance(l.serve + "/serve-devices.lock");
    if (!devices) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!devices) {
    sodium_memzero(access.data(), access.size());
    why = "the device list is being changed by something else; try again.";
    return std::nullopt;
  }
  const std::int64_t now = now_of(w);
  std::string code = serve::issue_pair_code(*store, kHostPairSeconds, now);
  devices.reset();
  if (code.empty()) {
    sodium_memzero(access.data(), access.size());
    why = "could not keep the code.";
    return std::nullopt;
  }
  const auto st = host_files::state(l);
  const std::int64_t running_port = state_number(st, "service_port");
  const auto port = static_cast<std::uint16_t>(running_port ? running_port : cfg->port);
  host_pair_code p;
  p.expires = now + kHostPairSeconds;
  if (const auto lan = lan_address(w)) p.local = with_port(*lan, port);
  if (!s.ddns.empty()) {
    const std::int64_t outside = state_number(st, "service_outside_port");
    p.public_name = ddns::duckdns_name(s.ddns) + ":" + std::to_string(outside ? outside : port);
  }
  if (p.local.empty() && p.public_name.empty()) {
    sodium_memzero(access.data(), access.size());
    wipe(code);
    why = "this computer has no address on a network to put in the code.";
    return std::nullopt;
  }
  p.url = pair_url_of(s.library_id, cfg->name.empty() ? std::string("This computer") : cfg->name, publisher, p, identity,
                      access, code);
  p.check = pairing_check(s.library_id, publisher, identity, access);
  sodium_memzero(access.data(), access.size());
  wipe(code);
  return p;
}

std::string host_relay_line(const host_layout& l, const setup_world& w, std::string& why, bool* tls) {
  const host_settings s = host_files::settings(l);
  auto key = host_files::relay_secret(l);
  if (!key) {
    why = "the relay's key cannot be read or made.";
    return {};
  }
  const auto st = host_files::state(l);
  // The look, unless the running relay says it could not listen.
  const bool look =
      look_port(s) != 0 && tls::available() && !(st && st->boolean("tls_failed").value_or(false));
  if (tls) *tls = look;
  std::int64_t port = 0;
  if (look) {
    port = state_number(st, "tls_outside_port");
    if (!port) port = state_number(st, "relay_tls_port");
    if (!port) port = look_port(s);
  } else {
    port = state_number(st, "relay_outside_port");
    if (!port) port = state_number(st, "relay_port");
    if (!port) port = s.relay_port;
  }
  std::string where;
  if (const std::string name = vpn_name(s); !name.empty() && !w.loopback) {
    where = name + ":" + std::to_string(port);
  } else if (const auto lan = lan_address(w)) {
    where = with_port(*lan, static_cast<std::uint16_t>(port));
  } else {
    wipe(*key);
    why = "this computer has no address on a network for the relay line.";
    return {};
  }
  std::string line = where + "#" + *key;
  wipe(*key);
  return line;
}

int host_name(const host_layout& l, bool duckdns, setup_io& io, const setup_world& w) {
  if (!make_host_folders(l)) {
    io.say("driftwire host: its folder must be this user's alone (mode 700).\n");
    return 1;
  }
  if (duckdns) return name_step(l, io, w, false) ? 0 : 1;
  host_settings s = host_files::settings(l);
  if (auto store = host_files::store(l.folder + "/secrets")) (void)ddns::forget_token(*store);
  s.ddns.clear();
  s.ddns_stamp = now_of(w);
  if (!host_files::save_settings(l, s)) {
    io.say("driftwire host: could not write host.json.\n");
    return 1;
  }
  io.say("No DuckDNS name now: phones reach this computer at home only. The token is forgotten.\n");
  return 0;
}

int host_pair(const host_layout& l, setup_io& io, const setup_world& w) { return show_pair(l, io, w); }

int host_domain(const host_layout& l, const std::string& typed, setup_io& io) {
  if (!make_host_folders(l)) {
    io.say("driftwire host: its folder must be this user's alone (mode 700).\n");
    return 1;
  }
  host_settings s = host_files::settings(l);
  if (typed == "none") {
    s.domain.clear();
  } else {
    const auto d = parse_domain(typed);
    if (!d) {
      io.say("driftwire host: \"" + typed + "\" is not a domain (vpn.example.com).\n");
      return 1;
    }
    if (d->size() > 12 && d->substr(d->size() - 12) == ".duckdns.org") {
      io.say("driftwire host: that is a DuckDNS name: set it with driftwire host name duckdns.\n");
      return 1;
    }
    s.domain = *d;
  }
  if (!host_files::save_settings(l, s)) {
    io.say("driftwire host: could not write host.json.\n");
    return 1;
  }
  if (s.domain.empty()) {
    io.say("No own domain now: the VPN code carries " +
           (s.ddns.empty() ? std::string("this computer's address") : ddns::duckdns_name(s.ddns)) + ".\n");
    return 0;
  }
  io.say("The VPN code, and the name a network sees, are now " + s.domain + ".\n");
  if (!s.ddns.empty()) {
    const std::string duck = ddns::duckdns_name(s.ddns);
    io.say("At your domain's DNS, add two CNAME records (DuckDNS keeps the address; the second lets Let's "
           "Encrypt check the certificate):\n\n  " +
           s.domain + "  CNAME  " + duck + "\n  _acme-challenge." + s.domain + "  CNAME  _acme-challenge." + duck +
           "\n\n");
  } else {
    io.say("Point it at this home yourself (an A record, or your provider's dynamic DNS). Without a DuckDNS name "
           "the web-site look keeps a certificate of its own making.\n");
  }
  io.say("Then ask for the code again (driftwire host vpn on) and paste it on the laptop.\n");
  return 0;
}

int host_vpn(const host_layout& l, bool on, std::uint16_t tls_port, setup_io& io, const setup_world& w,
             bool tls_off) {
  if (!make_host_folders(l)) {
    io.say("driftwire host: its folder must be this user's alone (mode 700).\n");
    return 1;
  }
  host_settings s = host_files::settings(l);
  s.vpn = on;
  // The look stays as it was set (on by default): `vpn on` alone, as the
  // desktop's VPN code asks it, never turns it on or off.
  if (on && tls_port != 0) {
    s.look = true;
    s.tls_port = tls_port;
  }
  if (on && tls_off) s.look = false;
  if (!host_files::save_settings(l, s)) {
    io.say("driftwire host: could not write host.json.\n");
    return 1;
  }
  if (!on) {
    io.say("The VPN is off: the relay forwards nothing, and the router's relay port closes.\n");
    return 0;
  }
  std::string why;
  bool tls = false;
  std::string line = host_relay_line(l, w, why, &tls);
  if (line.empty()) {
    io.say("driftwire host: " + why + "\n");
    return 1;
  }
  // With the look, the code (it says so; Connect turns the look on), else the line as before.
  if (tls) line = serve::relay_connect_code(line, true);
  // A paragraph a line (the session's transcript wraps them to its width).
  io.say("The VPN is on: this computer carries your laptop's traffic out to the "
         "internet, never into your home network.\n"
         "On the laptop, paste this code once in Driftwire's Connect to a VPN host. It holds the relay's key: "
         "keep it to yourself.\n\n  " +
         line + "\n\n");
  wipe(line);
  io.say("Then turn on the Tunnel or All traffic (VPN); sites see this home's address.\n");
  if (tls) {
    io.say("It looks like a web site on port " + std::to_string(look_port(s)) +
           " (443 from outside when the router allows), and the code turns on Look like a web site on the laptop.\n");
  } else if (look_port(s) != 0) {
    io.say("The web-site look could not listen on port " + std::to_string(look_port(s)) +
           ": this line is the relay's own port, which a network filter can spot.\n");
  }
  if (vpn_name(s).empty()) io.say("Without a DuckDNS name or your own domain this works on the home network only.\n");
  if (!host_files::running(l)) io.say("Start the host (driftwire host) for it to take effect.\n");
  return 0;
}

int host_setup(const host_layout& l, setup_io& io, const setup_world& w) {
  if (!make_host_folders(l)) {
    io.say("driftwire host: its folder must be this user's alone (mode 700).\n");
    return 1;
  }
  io.say("Hosting from this computer: what a NAS would do for your phones, and your laptop's VPN exit.\n"
         "Enter alone takes the choice in capitals, or skips.\n");
  (void)name_step(l, io, w, true);
  folder_step(l, io, w);
  router_step(l, io, w);

  io.say("\n4. A phone\n");
  const auto cfg = host_files::config(l, w.any_port);
  if (!cfg || user_roots(*cfg) == 0) {
    io.say("   No folder is shared: pair a phone once one is (driftwire host pair).\n");
  } else if (ask_yes(io, "   Pair a phone now?", true)) {
    (void)show_pair(l, io, w);
    (void)io.ask("   Enter when the phone is paired, or to go on: ", false);
  } else {
    io.say("   driftwire host pair shows the QR when you want it.\n");
  }

  io.say("\n5. The VPN\n"
         "   This computer can be your laptop's way out to the internet when you are away (the Tunnel, or All\n"
         "   traffic): sites then see your home's address. Never a way into your home network.\n");
  const host_settings s = host_files::settings(l);
  if (ask_yes(io, "   Use this computer as your laptop's VPN exit?", s.vpn)) {
    (void)host_vpn(l, true, 0, io, w);
  } else if (s.vpn) {
    (void)host_vpn(l, false, 0, io, w);
  } else {
    io.say("   Off (driftwire host vpn on turns it on).\n");
  }

  io.say("\n6. Starting with this computer\n");
  if (ask_yes(io, "   Start the host when you log in (no administrator needed)?", true)) {
    if (w.start_at_login) {
      args a;
      a.cmd = command::host;
      a.host = host_action::start_at_login;
      a.on = true;
      (void)w.start_at_login(a);
    } else {
      io.say("   driftwire host start-at-login on does it.\n");
    }
  }
  io.say(std::string("\nDone. ") +
         (host_files::running(l) ? "The running host takes it all within seconds; "
                                 : "Run driftwire host now (or log out and in); ") +
         "driftwire host status shows it.\n");
  return 0;
}

// ---- from the command line ------------------------------------------------------------------

namespace {

std::optional<std::string> read_typed_line(bool hidden) {
  std::string out;
  bool got = false, console = false;
  {
    const serve_echo_off quiet(hidden);
    console = quiet.active();
    for (int c; (c = std::fgetc(stdin)) != EOF;) {
      got = true;
      if (c == '\n') break;
      if (out.size() < 4096) out.push_back(static_cast<char>(c));
    }
  }
  if (console) print("\n");
  if (!out.empty() && out.back() == '\r') out.pop_back();
  if (!got) return std::nullopt;
  return out;
}

setup_io terminal_io(const args& a) {
  setup_io io;
  io.ask = [](const std::string& prompt, bool hidden) {
    print(prompt);
    return read_typed_line(hidden);
  };
  io.say = [](const std::string& text) { print(text); };
  io.colour = !a.no_color && std::getenv("NO_COLOR") == nullptr && serve_stdin_is_terminal();
  return io;
}

setup_world real_world(const args& a, const host_layout& l, bool test_hooks) {
  setup_world w;
  w.loopback = test_hooks && a.test_loopback;
  w.any_port = test_hooks;
  w.ask_router = !w.loopback;
  w.start_at_login = [&l, a, test_hooks](const args& on) {
    args b = on;
    b.data = a.data;
    return host_files::start_at_login(b, l, test_hooks);
  };
  return w;
}

int needs_terminal(bool test_hooks) {
  if (test_hooks || serve_stdin_is_terminal()) return 0;
  print("driftwire host: this asks questions: run it in a terminal.\n", true);
  return 1;
}

}  // namespace

int run_host_setup(const args& a, const host_layout& l, bool test_hooks) {
  if (const int r = needs_terminal(test_hooks)) return r;
  setup_io io = terminal_io(a);
  return host_setup(l, io, real_world(a, l, test_hooks));
}

int run_host_name(const args& a, const host_layout& l, bool test_hooks) {
  if (a.host_words.size() == 2 && a.host_words[0] == "domain") {
    setup_io io = terminal_io(a);
    return host_domain(l, a.host_words[1], io);
  }
  if (a.on) {
    if (const int r = needs_terminal(test_hooks)) return r;
  }
  setup_io io = terminal_io(a);
  return host_name(l, a.on, io, real_world(a, l, test_hooks));
}

int run_host_pair(const args& a, const host_layout& l, bool test_hooks) {
  setup_io io = terminal_io(a);
  return host_pair(l, io, real_world(a, l, test_hooks));
}

int run_host_vpn(const args& a, const host_layout& l, bool test_hooks) {
  setup_io io = terminal_io(a);
  return host_vpn(l, a.on, a.tls_port, io, real_world(a, l, test_hooks), a.tls_off);
}

}  // namespace mv::driftwire::cli
