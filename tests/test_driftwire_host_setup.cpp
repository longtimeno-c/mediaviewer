// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `driftwire host setup`, `host name`, `host pair` and `host vpn`
// (docs/plans/self-hosted.md §2, slices 5 and 7), and the DuckDNS keeper in
// the running host. Loopback and stand-ins only: a stand-in DuckDNS (the
// fetch the keeper and setup are given), a stand-in resolver, the host's
// relay on 127.0.0.1, a phone that is serve::client on ::1. Never
// duckdns.org, a real router or the public DHT. The router keeper in the
// running host is tested beside the stand-in routers
// (tests/test_driftwire_internet.cpp, "[host-setup]").
#include "catch_compat.h"

#include <sodium.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/args.h"
#include "addons/driftwire/cli/host_cmd.h"
#include "addons/driftwire/cli/host_keepers.h"
#include "addons/driftwire/cli/host_setup.h"
#include "addons/driftwire/cli/qr.h"
#include "addons/driftwire/ddns.h"
#include "addons/vpn/tunnel.h"
#include "catalogue/catalog_chain.h"
#include "core/json.h"
#include "import_fixture.h"
#include "io/child_process.h"
#include "tunnel_fixture.h"
#include "mobile/link_payload.h"
#include "wire/link.h"
#include "wire/net.h"
#include "wire/pair_check.h"
#include "wire/secrets.h"
#include "wire/serve.h"
#include "wire/serve_client.h"
#include "wire/splice.h"
#include "wire/tls.h"

using namespace mv::test;
namespace cli = mv::driftwire::cli;
namespace serve = mv::driftwire::serve;
namespace net = mv::driftwire::net;
namespace ddns = mv::driftwire::ddns;
namespace relay = mv::driftwire::relay;
namespace tunnel = mv::driftwire::tunnel;

namespace {

constexpr const char* kToken = "0123abcd-4567-89ef-0123-456789abcdef";

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

template <typename F>
bool eventually(F pred, int ms = 10000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return pred();
}

// The person at the terminal, scripted: each ask takes the next answer.
struct script {
  std::deque<std::string> answers;
  std::vector<std::string> prompts;
  std::vector<bool> hidden;
  std::string said;
  cli::setup_io io() {
    cli::setup_io o;
    o.ask = [this](const std::string& prompt, bool h) -> std::optional<std::string> {
      prompts.push_back(prompt);
      hidden.push_back(h);
      said += prompt;
      if (answers.empty()) return std::nullopt;
      std::string a = answers.front();
      answers.pop_front();
      return a;
    };
    o.say = [this](const std::string& t) { said += t; };
    o.colour = false;
    return o;
  }
};

// A stand-in DuckDNS: what each update asked, and what it answers.
struct standin_duckdns {
  std::mutex m;
  std::vector<std::string> urls;
  std::string answer = "OK\n81.2.69.160\n\nUPDATED";
  int status = 200;
  ddns::fetch_fn fetch() {
    return [this](const std::string& url, int) {
      std::lock_guard lock(m);
      urls.push_back(url);
      return ddns::fetch_answer{status, answer};
    };
  }
  std::size_t count() {
    std::lock_guard lock(m);
    return urls.size();
  }
  std::string last() {
    std::lock_guard lock(m);
    return urls.empty() ? std::string() : urls.back();
  }
};

cli::setup_world loopback_world(standin_duckdns* dns = nullptr) {
  cli::setup_world w;
  w.loopback = true;
  w.any_port = true;
  w.ask_router = false;
  if (dns) w.fetch = dns->fetch();
  w.resolve = [](const std::string&) { return std::vector<net::address>{*net::parse_address("81.2.69.160")}; };
  w.name_tries = 2;
  w.name_wait_ms = 10;
  return w;
}

// A host folder with serve.json on port 0 (the OS picks: the test build).
cli::host_layout new_host(const fs::path& data) {
  fs::create_directories(data);
  const cli::host_layout l = cli::host_layout_of(utf8(data));
  REQUIRE(cli::make_host_folders(l));
  serve::config c;
  c.port = 0;
  c.name = "Test PC";
  write_text(l.serve_config, serve::config_json(c));
  return l;
}

cli::host_service_options service_options(const cli::host_layout& l) {
  cli::host_service_options o;
  o.where = l;
  o.loopback_only = true;
  o.any_port = true;
  o.config_every_ms = 100;
  o.roots_every_ms = 100000;
  o.retry_ms = 200;
  o.power = false;
  o.present = [](const serve::root_config&) { return true; };
  o.locate = [](const serve::root_config& r) -> std::optional<std::string> { return r.path; };
  return o;
}

std::vector<std::uint8_t> unhex(const std::string& h) {
  std::vector<std::uint8_t> out(h.size() / 2);
  std::size_t n = 0;
  REQUIRE(sodium_hex2bin(out.data(), out.size(), h.data(), h.size(), nullptr, &n, nullptr) == 0);
  out.resize(n);
  return out;
}

std::array<std::uint8_t, 32> key32_of(const std::string& h) {
  const auto v = unhex(h);
  REQUIRE(v.size() == 32);
  std::array<std::uint8_t, 32> k{};
  std::copy(v.begin(), v.end(), k.begin());
  return k;
}

// The QR's payload, as the phone reads it (mobile/link_payload.cpp).
mv::json::value payload_of(const std::string& url) {
  const std::string prefix = "mediaviewer://pair#";
  REQUIRE(url.rfind(prefix, 0) == 0);
  const std::string coded = url.substr(prefix.size());
  std::vector<unsigned char> raw(coded.size());
  std::size_t len = 0;
  REQUIRE(sodium_base642bin(raw.data(), raw.size(), coded.data(), coded.size(), nullptr, &len, nullptr,
                            sodium_base64_VARIANT_URLSAFE_NO_PADDING) == 0);
  auto doc = mv::json::parse(std::string_view(reinterpret_cast<const char*>(raw.data()), len), 4);
  REQUIRE(doc);
  return std::move(*doc);
}

std::string str_of(const mv::json::value& v, const char* k) {
  const std::string* s = v.str(k);
  REQUIRE(s);
  return *s;
}

std::string read_all(serve::client& c, const std::string& root, const std::string& rel) {
  auto s = c.read_range(root, rel, 0, UINT64_MAX);
  REQUIRE(s);
  std::string out;
  for (;;) {
    auto n = (*s)->next(8000);
    REQUIRE(n);
    if (!*n) return out;
    out.append(reinterpret_cast<const char*>((*n)->bytes.data()), (*n)->bytes.size());
  }
}

// The matrix as '0'/'1' rows, hashed: the C# tests' pin (QrCodeTests.cs).
std::string matrix_sha(const cli::qr_code& q) {
  std::string rows;
  for (int y = 0; y < q.size(); ++y) {
    for (int x = 0; x < q.size(); ++x) rows.push_back(q.dark(x, y) ? '1' : '0');
    rows.push_back('\n');
  }
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const unsigned char*>(rows.data()), rows.size());
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : h) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out;
}

}  // namespace

TEST_CASE("host setup: the QR is the desktop's, module for module", "[host-setup]") {
  REQUIRE(sodium_init() >= 0);
  // The matrices the Windows card pins, read back by a scanner when it was written.
  auto q = cli::qr_code::encode("hello");
  REQUIRE(q);
  CHECK(q->version() == 1);
  CHECK(q->size() == 21);
  CHECK(q->mask() == 7);
  CHECK(matrix_sha(*q) == "D9E93C47DF2FD3419021124B6F90CDE9ADD476DDA7D7B9E721F3B2463D312960");
  q = cli::qr_code::encode("mediaviewer://pair#eyJ2IjoxLCJsaWJyYXJ5X2lkIjoiMDEyMyJ9");
  REQUIRE(q);
  CHECK(q->version() == 4);
  CHECK(q->mask() == 1);
  CHECK(matrix_sha(*q) == "EFAB0B5E7E736915C0DAE4FB6187AD984C23D93359D6A1C3F451FD1A78B2E2A1");
  CHECK(cli::qr_code::encode(std::string(14, 'a'))->version() == 1);
  CHECK(cli::qr_code::encode(std::string(15, 'a'))->version() == 2);
  CHECK(cli::qr_code::encode(std::string(2331, 'a'))->version() == 40);
  CHECK_FALSE(cli::qr_code::encode(std::string(2332, 'a')));
  CHECK_FALSE(q->dark(-1, 0));  // the quiet zone

  // In the terminal: two module rows a line, the same width every line.
  const auto lines = cli::qr_terminal_lines(*q, false, 2);
  CHECK(lines.size() == static_cast<std::size_t>((q->size() + 4 + 1) / 2));
  const auto columns = [](const std::string& s) {
    std::size_t n = 0;
    for (const unsigned char c : s) n += (c & 0xC0) != 0x80 ? 1 : 0;  // UTF-8 starts
    return n;
  };
  for (const auto& l : lines) CHECK(columns(l) == static_cast<std::size_t>(q->size() + 4));
  CHECK(lines.front().find_first_not_of(' ') == std::string::npos);  // the quiet zone's top
  const auto coloured = cli::qr_terminal_lines(*q, true, 2);
  CHECK(coloured.front().rfind("\x1b[30;47m", 0) == 0);
  CHECK(coloured.front().size() >= 4);
  CHECK(coloured.front().substr(coloured.front().size() - 4) == "\x1b[0m");
}

TEST_CASE("host setup: the arguments; nothing secret is one", "[host-setup][host]") {
  const auto parse = [](std::vector<std::string> v) { return cli::parse_args(v, true); };
  auto p = parse({"host", "setup"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::setup);
  p = parse({"host", "name", "duckdns"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::name);
  CHECK(p.a.on);
  p = parse({"host", "name", "none"});
  REQUIRE(p.error.empty());
  CHECK_FALSE(p.a.on);
  CHECK(parse({"host", "pair"}).a.host == cli::host_action::pair);
  p = parse({"host", "vpn", "on", "--tls-port", "443"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::vpn);
  CHECK(p.a.on);
  CHECK(p.a.tls_port == 443);
  CHECK_FALSE(p.a.tls_off);
  p = parse({"host", "vpn", "on", "--tls-port", "off"});
  REQUIRE(p.error.empty());
  CHECK(p.a.tls_off);
  CHECK(p.a.tls_port == 0);
  CHECK_FALSE(parse({"host", "vpn", "off", "--tls-port", "off"}).error.empty());
  p = parse({"host", "vpn", "off"});
  REQUIRE(p.error.empty());
  CHECK_FALSE(p.a.on);
  // The subdomain and the token are typed, never arguments.
  CHECK_FALSE(parse({"host", "name", "duckdns", "home"}).error.empty());
  CHECK_FALSE(parse({"host", "name", "duckdns", kToken}).error.empty());
  CHECK_FALSE(parse({"host", "name", "home.duckdns.org"}).error.empty());
  CHECK_FALSE(parse({"host", "setup", "extra"}).error.empty());
  CHECK_FALSE(parse({"host", "vpn"}).error.empty());
  CHECK_FALSE(parse({"host", "vpn", "off", "--tls-port", "443"}).error.empty());
  CHECK_FALSE(parse({"host", "vpn", "on", "--tls-port", "0"}).error.empty());
  CHECK_FALSE(parse({"host", "vpn", "on", "--tls-port", "65536"}).error.empty());
  CHECK_FALSE(parse({"host", "status", "--tls-port", "443"}).error.empty());
  CHECK_FALSE(parse({"serve", "--tls-port", "443"}).error.empty());
  CHECK(contains(cli::help_text(), "host setup"));
}

TEST_CASE("host setup: host.json keeps the name, the web-site look and the library", "[host-setup][host]") {
  cli::host_settings s;
  s.ddns = "home";
  s.ddns_stamp = 1700000000;
  s.tls_port = 443;
  s.library_id = "0123456789abcdef0123456789abcdef";
  s.vpn = true;
  const auto back = cli::parse_host_settings(cli::host_settings_json(s));
  REQUIRE(back);
  CHECK(back->ddns == "home");
  CHECK(back->ddns_stamp == 1700000000);
  CHECK(back->tls_port == 443);
  CHECK(back->library_id == s.library_id);
  CHECK(back->vpn);
  // Slice 2's file reads as before; a name not as parse_subdomain gives it is not one of ours.
  CHECK(cli::parse_host_settings(R"({"relay_port":47614,"vpn":false,"stay_awake":true})"));
  CHECK_FALSE(cli::parse_host_settings(R"({"ddns":"home.duckdns.org"})"));
  CHECK_FALSE(cli::parse_host_settings(R"({"ddns":"a b"})"));
  CHECK_FALSE(cli::parse_host_settings(R"({"tls_port":70000})"));
  CHECK_FALSE(cli::parse_host_settings(R"({"library_id":"../x"})"));
  CHECK(cli::host_settings_json(cli::host_settings{}).find("ddns") == std::string::npos);
  // The web-site look: on with the VPN by default, on the relay's port + 1, or as set; "look": false is off.
  cli::host_settings v;
  CHECK(cli::look_port(v) == 0);  // no VPN
  v.vpn = true;
  CHECK(cli::look_port(v) == 47615);
  v.tls_port = 443;
  CHECK(cli::look_port(v) == 443);
  v.look = false;
  CHECK(cli::look_port(v) == 0);
  const auto off = cli::parse_host_settings(cli::host_settings_json(v));
  REQUIRE(off);
  CHECK_FALSE(off->look);
  CHECK(cli::parse_host_settings(R"({"vpn":true})")->look);
  CHECK_FALSE(cli::parse_host_settings(R"({"look":"no"})"));
}

TEST_CASE("host setup: your own domain for the VPN, beside DuckDNS", "[host-setup][host][vpn-stealth]") {
  CHECK(cli::parse_domain("vpn.example.com") == "vpn.example.com");
  CHECK(cli::parse_domain(" VPN.Example.COM. ") == "vpn.example.com");
  CHECK(cli::parse_domain("https://vpn.example.com/") == "vpn.example.com");
  CHECK(cli::parse_domain("a-b.c1.example.co.uk") == "a-b.c1.example.co.uk");
  for (const char* bad : {"", "localhost", "vpn..example.com", "-vpn.example.com", "vpn-.example.com", "vpn example.com",
                          "vpn.example.com:443", "vpn.example.com/path", "v_pn.example.com", "ex\xC3\xA4mple.com"}) {
    INFO(bad);
    CHECK_FALSE(cli::parse_domain(bad));
  }
  // The name the VPN is reached by: the own domain first, else DuckDNS's, else none.
  cli::host_settings s;
  CHECK(cli::vpn_name(s).empty());
  s.ddns = "home";
  CHECK(cli::vpn_name(s) == "home.duckdns.org");
  s.domain = "vpn.example.com";
  CHECK(cli::vpn_name(s) == "vpn.example.com");
  const auto back = cli::parse_host_settings(cli::host_settings_json(s));
  REQUIRE(back);
  CHECK(back->domain == "vpn.example.com");
  CHECK_FALSE(cli::parse_host_settings(R"({"domain":"not a domain"})"));
  // `host name domain NAME|none`, and the two CNAMEs it asks for.
  scratch_dir dir("host_domain");
  const cli::host_layout l = new_host(dir / "data");
  cli::host_settings with_duck;
  with_duck.ddns = "home";
  REQUIRE(cli::host_files::save_settings(l, with_duck));
  script t;
  cli::setup_io io = t.io();
  REQUIRE(cli::host_domain(l, "VPN.Example.com", io) == 0);
  CHECK(cli::host_files::settings(l).domain == "vpn.example.com");
  CHECK(contains(t.said, "vpn.example.com  CNAME  home.duckdns.org"));
  CHECK(contains(t.said, "_acme-challenge.vpn.example.com  CNAME  _acme-challenge.home.duckdns.org"));
  CHECK(cli::host_domain(l, "home.duckdns.org", io) == 1);  // that one is `host name duckdns`
  CHECK(cli::host_domain(l, "nope", io) == 1);
  CHECK(cli::host_files::settings(l).domain == "vpn.example.com");
  REQUIRE(cli::host_domain(l, "none", io) == 0);
  CHECK(cli::host_files::settings(l).domain.empty());
  CHECK(cli::host_files::settings(l).ddns == "home");  // DuckDNS stays
  // The command line's words.
  const auto parse = [](std::vector<std::string> v) { return cli::parse_args(v, true); };
  const auto p = parse({"host", "name", "domain", "vpn.example.com"});
  REQUIRE(p.error.empty());
  CHECK(p.a.host == cli::host_action::name);
  CHECK(p.a.host_words == std::vector<std::string>{"domain", "vpn.example.com"});
  CHECK_FALSE(parse({"host", "name", "domain"}).error.empty());
}

TEST_CASE("host vpn: a code with the public name never carries the look's inside port before the router maps",
          "[host-setup][host][vpn-stealth]") {
  if (!mv::driftwire::tls::available()) SKIP("this build has no BoringSSL");
  scratch_dir dir("host_vpn_code_unmapped");
  const cli::host_layout l = new_host(dir / "data");
  cli::host_settings s;
  s.vpn = true;
  s.ddns = "home";
  REQUIRE(cli::host_files::save_settings(l, s));
  const auto lock = net::try_lock_instance(l.lock);  // a running host's
  REQUIRE(lock);
  auto w = loopback_world();
  w.loopback = false;  // the name, as from away
  // `vpn on` prints its code before the router keeper has mapped: no outside port yet.
  REQUIRE(cli::host_files::write(l.state, R"({"relay_port":47614,"relay_tls_port":47615,"vpn":true})"));
  std::string why;
  bool tls = false;
  CHECK(cli::host_relay_line(l, w, why, &tls).rfind("home.duckdns.org:443#", 0) == 0);
  CHECK(tls);
  // Once mapped, the router's port, whatever it is.
  REQUIRE(cli::host_files::write(l.state,
                                 R"({"relay_port":47614,"relay_tls_port":47615,"tls_outside_port":8443,"vpn":true})"));
  CHECK(cli::host_relay_line(l, w, why, &tls).rfind("home.duckdns.org:8443#", 0) == 0);
}

TEST_CASE("host vpn: the code's port: the router's outside one for a name, the one it listens on for the LAN",
          "[host-setup][host][vpn-stealth]") {
  if (!mv::driftwire::tls::available()) SKIP("this build has no BoringSSL");
  scratch_dir dir("host_vpn_code_port");
  const cli::host_layout l = new_host(dir / "data");
  cli::host_settings s;
  s.vpn = true;
  REQUIRE(cli::host_files::save_settings(l, s));
  const auto lock = net::try_lock_instance(l.lock);  // a running host's
  REQUIRE(lock);
  // The router mapped outside 443 to the look's 47615; on the home network nothing listens on 443.
  REQUIRE(cli::host_files::write(l.state, R"({"relay_port":47614,"relay_tls_port":47615,"tls_outside_port":443,)"
                                          R"("relay_outside_port":47614,"vpn":true})"));
  std::string why;
  bool tls = false;
  const std::string line = cli::host_relay_line(l, loopback_world(), why, &tls);
  CHECK(tls);
  CHECK(line.rfind("127.0.0.1:47615#", 0) == 0);
  // The look could not listen: the plain relay's port.
  REQUIRE(cli::host_files::write(l.state, R"({"relay_port":47614,"tls_failed":true,"vpn":true})"));
  CHECK(cli::host_relay_line(l, loopback_world(), why, &tls).rfind("127.0.0.1:47614#", 0) == 0);
  CHECK_FALSE(tls);
}

TEST_CASE("host setup: what the router is asked for, and the status words", "[host-setup][host]") {
  cli::keeper_context k;
  CHECK(cli::wanted_ports(k).empty());
  k.service_port = 7878;
  k.relay_port = 47614;
  auto w = cli::wanted_ports(k);
  REQUIRE(w.size() == 1);  // the relay only with the VPN on
  CHECK(w[0].internal_port == 7878);
  CHECK(w[0].what == "service");
  CHECK(w[0].udp);  // the service's sessions over UDP, at the same port
  k.vpn = true;
  w = cli::wanted_ports(k);  // the VPN without the look: the plain relay's port
  REQUIRE(w.size() == 2);
  CHECK(w[1].internal_port == 47614);
  CHECK(w[1].what == "relay");
  CHECK_FALSE(w[1].udp);
  // With the look, only it is open outside (the plain relay's cleartext head never is):
  // outside 443, else 8443.
  k.relay_tls_port = 47615;
  w = cli::wanted_ports(k);
  REQUIRE(w.size() == 2);
  CHECK(w[1].internal_port == 47615);
  CHECK(w[1].external_port == 443);
  CHECK(w[1].fallback_external == cli::kLookFallbackPort);
  CHECK(cli::kLookFallbackPort == 8443);
  CHECK(cli::ago_words(1000, 1030) == "just now");
  CHECK(cli::ago_words(1000, 1000 + 180) == "3 min ago");
  CHECK(cli::ago_words(1000, 1000 + 7200) == "2 h ago");
  CHECK(cli::ago_words(1000, 1000 + 5 * 86400) == "5 days ago");
}

TEST_CASE("host setup: the guided run, then a phone pairs from its QR over loopback", "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_setup_run");
  fs::create_directories(s / "Photos");
  fs::create_directories(s / "Phone backup");
  write_text(s / "Photos/a.jpg", "jpeg bytes");
  const cli::host_layout l = new_host(s / "data");
  standin_duckdns dns;
  cli::setup_world w = loopback_world(&dns);
  int login_asks = 0;
  w.start_at_login = [&](const cli::args& a) {
    CHECK(a.host == cli::host_action::start_at_login);
    CHECK(a.on);
    ++login_asks;
    return 0;
  };
  script t;
  t.answers = {"Home.DuckDNS.org", kToken,                                 // 1. the name
               utf8(s / "Photos"), "", utf8(s / "Phone backup"), "200",    // 2. a folder, a backup folder
               "", "",                                                     // 4. pair now (yes), then Enter
               "y",                                                        // 5. the VPN
               ""};                                                        // 6. start at login (yes)
  cli::setup_io io = t.io();
  CHECK(cli::host_setup(l, io, w) == 0);
  CHECK(t.answers.empty());
  CHECK(login_asks == 1);

  // The token: asked hidden, sent to DuckDNS, kept in the host's secrets, nowhere else.
  REQUIRE(t.prompts.size() >= 2);
  CHECK_FALSE(t.hidden[0]);
  CHECK(t.hidden[1]);
  REQUIRE(dns.count() >= 1);
  CHECK(contains(dns.last(), "domains=home&"));
  CHECK(contains(dns.last(), std::string("token=") + kToken));
  CHECK_FALSE(contains(t.said, kToken));
  CHECK_FALSE(contains(read_text(l.settings), kToken));
  {
    auto store = cli::host_files::store(l.folder + "/secrets");
    REQUIRE(store);
    CHECK(ddns::load_token(*store) == std::string(kToken));
  }
  CHECK(contains(t.said, "Checked: home.duckdns.org finds this home."));
  CHECK(contains(t.said, "The router was not asked"));

  const cli::host_settings hs = cli::host_files::settings(l);
  CHECK(hs.ddns == "home");
  CHECK(hs.vpn);
  CHECK(hs.library_id.size() == 32);

  // serve.json: the folder, the backup folder with its limit, the host library's Catalogue.
  auto cfg = serve::parse_config(read_text(l.serve_config), true, true);
  REQUIRE(cfg);
  REQUIRE(cfg->roots.size() == 3);
  CHECK(cfg->roots[0].label == "Photos");
  CHECK_FALSE(cfg->roots[0].backup);
  CHECK(cfg->roots[1].label == "Phone backup");
  CHECK(cfg->roots[1].backup);
  CHECK(cfg->roots[1].limit == 200'000'000'000ULL);
  CHECK(cfg->roots[2].label == serve::kCatalogueLabel);
  CHECK(cfg->roots[2].path == cli::host_catalogue_of(l));
  const std::string library = read_text(fs::path(cli::host_catalogue_of(l)) / "library.json");
  CHECK(contains(library, "\"library_id\":\"" + hs.library_id + "\""));

  // The QR was drawn; the relay line for the laptop shown once, with the relay's key.
  CHECK(contains(t.said, "\xE2\x96\x88"));
  CHECK(contains(t.said, "The phone then shows a check"));
  const auto key = cli::host_files::relay_secret(l);
  REQUIRE(key);
  // With the web-site look (on by default), its code on the look's port; else the plain line.
  if (mv::driftwire::tls::available()) {
    CHECK(contains(t.said, "mediaviewer://relay-tls#127.0.0.1:47615#" + *key));
  } else {
    CHECK(contains(t.said, "127.0.0.1:47614#" + *key));
  }
  CHECK(contains(t.said, "never into your home network"));

  // The host runs (the lock as the supervisor holds it), and a phone pairs from a new code.
  const auto lock = net::try_lock_instance(l.lock);
  REQUIRE(lock);
  std::string why;
  auto host = cli::host_service::start(service_options(l), why);
  REQUIRE(host);
  REQUIRE(eventually([&] { return host->snapshot().service_port != 0; }));
  REQUIRE(eventually([&] { return contains(read_text(l.state), "\"service_port\":" + std::to_string(host->snapshot().service_port)); }));
  const auto code = cli::make_host_pair(l, w, why);
  REQUIRE(code);
  const std::uint16_t port = host->snapshot().service_port;
  CHECK(code->local == "127.0.0.1:" + std::to_string(port));
  CHECK(code->public_name == "home.duckdns.org:" + std::to_string(port));  // the name as the public route
  const auto p = payload_of(code->url);
  CHECK(p.integer("v") == 1);
  CHECK(str_of(p, "library_id") == hs.library_id);
  CHECK(str_of(p, "library_name") == "Test PC");
  const mv::json::value* service = p.find("service");
  REQUIRE(service);
  CHECK(str_of(*service, "public") == code->public_name);
  const auto identity = key32_of(str_of(*service, "identity"));
  const auto access = key32_of(str_of(p, "access_key"));
  const auto publisher = key32_of(str_of(p, "publisher_key"));
  CHECK(code->check == mv::driftwire::pairing_check(hs.library_id, publisher, identity, access));
  CHECK(p.find("relay") == nullptr);  // the host's own relay is behind the same router: no use to a phone
  // The phone's own reading of it: accepted, the same routes, the same check.
  const auto phone_reads = mv::mobile::parse_pair_payload(code->url);
  REQUIRE(phone_reads);
  CHECK(phone_reads->library_id == hs.library_id);
  CHECK(phone_reads->local == code->local);
  CHECK(phone_reads->public_name == code->public_name);
  CHECK(phone_reads->fingerprint == code->check);
  CHECK(phone_reads->relay.empty());

  std::array<std::uint8_t, 32> device{};
  randombytes_buf(device.data(), device.size());
  serve::client::options co;
  co.access_key = access;
  co.device_key = device;
  co.service_key = identity;
  co.id = "bbbbbbbbbbbbbbbb";
  co.name = "phone";
  co.pair_code = str_of(p, "code");
  co.device_name = "iPhone";
  auto l1 = net::connect({"::1", port}, 3000);
  REQUIRE(l1);
  auto phone = serve::client::open(std::shared_ptr<mv::driftwire::link>(std::move(*l1)), co);
  REQUIRE(phone);
  auto roots = (*phone)->roots();
  REQUIRE(roots);
  std::string catalogue_id, photos_id;
  for (const auto& r : *roots) {
    if (r.label == serve::kCatalogueLabel) catalogue_id = r.id;
    if (r.label == "Photos") photos_id = r.id;
  }
  REQUIRE_FALSE(catalogue_id.empty());
  REQUIRE_FALSE(photos_id.empty());
  // File mode's test: a folder's library.json names the paired library.
  CHECK(contains(read_all(**phone, catalogue_id, "library.json"), "\"library_id\":\"" + hs.library_id + "\""));
  CHECK(read_all(**phone, photos_id, "a.jpg") == "jpeg bytes");
  // The code works once.
  std::array<std::uint8_t, 32> other{};
  randombytes_buf(other.data(), other.size());
  co.device_key = other;
  auto l2 = net::connect({"::1", port}, 3000);
  REQUIRE(l2);
  serve::refusal refused = serve::refusal::none;
  CHECK_FALSE(serve::client::open(std::shared_ptr<mv::driftwire::link>(std::move(*l2)), co, &refused));
  CHECK(refused == serve::refusal::bad_code);
  // A second pairing keeps the same library.
  const auto again = cli::make_host_pair(l, w, why);
  REQUIRE(again);
  CHECK(str_of(payload_of(again->url), "library_id") == hs.library_id);
  CHECK(str_of(payload_of(again->url), "publisher_key") == str_of(p, "publisher_key"));
  host->stop();

  // Unsharing the person's folders takes the host library's Catalogue with the last one.
  for (const char* label : {"Photos", "Phone backup"}) {
    cli::args un;
    un.cmd = cli::command::host;
    un.host = cli::host_action::unshare;
    un.data = utf8(s / "data");
    un.label = label;
    CHECK(cli::run_host(un, true, "0.0.0") == 0);
  }
  cfg = serve::parse_config(read_text(l.serve_config), true, true);
  REQUIRE(cfg);
  CHECK(cfg->roots.empty());
  std::string no;
  CHECK_FALSE(cli::make_host_pair(l, w, no));
  CHECK(contains(no, "No folder is shared"));
}

TEST_CASE("host setup: a service a desktop claimed pairs from the desktop; its Catalogue stays", "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_setup_claimed");
  fs::create_directories(s / "Photos/.mediaviewer");
  const cli::host_layout l = new_host(s / "data");
  // As a desktop's claim leaves serve.json: its folder, and that folder's .mediaviewer as the Catalogue.
  {
    serve::config c;
    c.port = 0;
    c.name = "Test PC";
    c.roots.push_back({"a1a1a1a1a1a1a1a1", "Photos", utf8(s / "Photos")});
    c.roots.push_back({"c2c2c2c2c2c2c2c2", serve::kCatalogueLabel, utf8(s / "Photos/.mediaviewer")});
    write_text(l.serve_config, serve::config_json(c));
  }
  std::string why;
  CHECK_FALSE(cli::make_host_pair(l, loopback_world(), why));
  CHECK(contains(why, "A desktop has claimed this service"));
  CHECK_FALSE(fs::exists(fs::path(cli::host_catalogue_of(l)) / "library.json"));
  // The person's last folder unshared: the desktop's Catalogue is the desktop's to remove.
  cli::args un;
  un.cmd = cli::command::host;
  un.host = cli::host_action::unshare;
  un.data = utf8(s / "data");
  un.label = "Photos";
  CHECK(cli::run_host(un, true, "0.0.0") == 0);
  const auto cfg = serve::parse_config(read_text(l.serve_config), true, true);
  REQUIRE(cfg);
  REQUIRE(cfg->roots.size() == 1);
  CHECK(cfg->roots[0].label == serve::kCatalogueLabel);
}

TEST_CASE("host setup: a DuckDNS name skipped, refused, mistyped, then kept; and forgotten", "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_setup_name");
  const cli::host_layout l = new_host(s / "data");
  standin_duckdns dns;
  const cli::setup_world w = loopback_world(&dns);

  // Skipped: LAN only, said plainly; nothing asked of DuckDNS.
  {
    script t;
    t.answers = {""};
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, true, io, w) == 1);
    CHECK(contains(t.said, "phones reach this computer at home only"));
    CHECK(dns.count() == 0);
  }
  // A subdomain that is not one, a token that is not one, DuckDNS's KO, then right.
  {
    script t;
    t.answers = {"home.example.com", "home", "not-a-token", "home", kToken};
    cli::setup_io io = t.io();
    dns.answer = "KO";
    CHECK(cli::host_name(l, true, io, w) == 1);  // three tries
    CHECK(contains(t.said, "That is not a DuckDNS subdomain"));
    CHECK(contains(t.said, "That is not a DuckDNS token"));
    CHECK(contains(t.said, "DuckDNS said no"));
    CHECK(cli::host_files::settings(l).ddns.empty());
    auto store = cli::host_files::store(l.folder + "/secrets");
    REQUIRE(store);
    CHECK_FALSE(ddns::load_token(*store));  // a refused token is not kept
  }
  {
    dns.answer = "OK\n81.2.69.160\n\nUPDATED";
    script t;
    t.answers = {"cabin", kToken};
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, true, io, w) == 0);
    CHECK(cli::host_files::settings(l).ddns == "cabin");
    CHECK(cli::host_files::settings(l).ddns_stamp != 0);
    CHECK(contains(t.said, "Checked: cabin.duckdns.org finds this home."));
  }
  // The name resolves elsewhere (not yet updated in the resolver): said, not a failure.
  {
    cli::setup_world late = w;
    late.resolve = [](const std::string&) { return std::vector<net::address>{*net::parse_address("81.2.69.1")}; };
    script t;
    t.answers = {"cabin", kToken};
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, true, io, late) == 0);
    CHECK(contains(t.said, "does not show this home's address yet"));
  }
  // Nothing answered (offline).
  {
    dns.status = 0;
    dns.answer.clear();
    script t;
    t.answers = {"cabin", kToken, ""};
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, true, io, w) == 1);
    CHECK(contains(t.said, "DuckDNS did not answer"));
    dns.status = 200;
    dns.answer = "OK\n81.2.69.160\n\nUPDATED";
  }
  // The test build never reaches duckdns.org without a stand-in.
  {
    script t;
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, true, io, loopback_world()) == 1);
    CHECK(contains(t.said, "never reaches duckdns.org"));
  }
  // Forgotten: the name and the token.
  {
    script t;
    cli::setup_io io = t.io();
    CHECK(cli::host_name(l, false, io, w) == 0);
    CHECK(cli::host_files::settings(l).ddns.empty());
    auto store = cli::host_files::store(l.folder + "/secrets");
    REQUIRE(store);
    CHECK_FALSE(ddns::load_token(*store));
  }
}

// A secrets store whose read waits: the keeper's worker held where the host's
// first state.json is written (the token not read yet).
struct held_store final : mv::driftwire::secrets::store {
  std::mutex m;
  std::condition_variable cv;
  int reads = 0;
  bool open = false;
  bool put(const std::string&, std::span<const std::uint8_t>) override { return false; }
  std::optional<std::vector<std::uint8_t>> get(const std::string&) override {
    std::unique_lock lock(m);
    ++reads;
    cv.notify_all();
    cv.wait(lock, [this] { return open; });
    return std::nullopt;
  }
  bool erase(const std::string&) override { return false; }
  void wait_reads(int n) {
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return reads >= n; });
  }
  void let_go() {
    {
      std::lock_guard lock(m);
      open = true;
    }
    cv.notify_all();
  }
};

TEST_CASE("host keepers: a status asked before the worker has looked names what is wanted, never \"none\"",
          "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  // The router: not asked yet, not "nothing to open".
  CHECK(cli::make_router_keeper()->status_line() == "router      not asked yet");

  held_store store;
  cli::ddns_keeper_config c;
  c.fetch = [](const std::string&, int) { return ddns::fetch_answer{}; };
  c.on_loopback = true;
  auto k = cli::make_ddns_keeper(std::move(c));
  cli::keeper_context ctx;
  ctx.secrets = &store;
  ctx.loopback_only = true;
  ctx.ddns_subdomain = "home";
  k->start(ctx);
  store.wait_reads(1);  // the worker is reading the token: nothing built yet
  CHECK(k->status_line() == "DuckDNS     home.duckdns.org, updating");
  // A new name while it still works on the old one: the new one at once.
  ctx.ddns_subdomain = "cabin";
  k->ports_changed(ctx);
  CHECK(k->status_line() == "DuckDNS     cabin.duckdns.org, updating");
  store.let_go();
  CHECK(eventually([&] { return contains(k->status_line(), "cabin.duckdns.org: no token kept"); }));
  // The name taken away: none, and said so.
  ctx.ddns_subdomain.clear();
  k->ports_changed(ctx);
  CHECK(contains(k->status_line(), "DuckDNS     none: "));
  k->stop();
}

TEST_CASE("host setup: the DuckDNS keeper in the running host; host status shows it, never the token",
          "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_ddns_keeper");
  const cli::host_layout l = new_host(s / "data");
  standin_duckdns dns;
  {
    script t;
    t.answers = {"home", kToken};
    cli::setup_io io = t.io();
    REQUIRE(cli::host_name(l, true, io, loopback_world(&dns)) == 0);
  }
  const std::size_t before = dns.count();
  auto o = service_options(l);
  o.keepers.ddns = [&] {
    cli::ddns_keeper_config c;
    c.fetch = dns.fetch();
    c.on_loopback = true;
    c.when.min_gap = std::chrono::milliseconds(10);
    c.when.backoff_first = std::chrono::milliseconds(10);
    return cli::make_ddns_keeper(std::move(c));
  };
  std::string why;
  auto host = cli::host_service::start(o, why);
  REQUIRE(host);
  REQUIRE(eventually([&] { return contains(read_text(l.state), "DuckDNS     home.duckdns.org, updated just now"); }));
  CHECK(dns.count() > before);
  CHECK(contains(dns.last(), "domains=home&"));
  CHECK_FALSE(contains(read_text(l.state), kToken));
  CHECK_FALSE(contains(read_text(l.state), "81.2.69.160"));  // the address stays off the status (rule 6)

  // A new name from another terminal: the keeper takes it within seconds.
  {
    script t;
    t.answers = {"cabin", kToken};
    cli::setup_io io = t.io();
    REQUIRE(cli::host_name(l, true, io, loopback_world(&dns)) == 0);
  }
  REQUIRE(eventually([&] { return contains(dns.last(), "domains=cabin&") && contains(read_text(l.state), "cabin.duckdns.org, updated"); }));
  host.reset();  // stopped, and its folder's lock let go

  // The test build's real keepers stay idle: never duckdns.org, never a router.
  auto idle = service_options(l);
  idle.keepers = cli::host_keepers();
  const std::size_t quiet = dns.count();
  auto host2 = cli::host_service::start(idle, why);
  REQUIRE(host2);
  REQUIRE(eventually([&] {
    const std::string st = read_text(l.state);
    return contains(st, "the test build never reaches duckdns.org") && contains(st, "the test build never asks a router");
  }));
  CHECK(dns.count() == quiet);
  host2->stop();
}

// The VPN host end to end, measured (docs/DEVELOPMENT.md "VPN host bench"): this
// host's relay with the VPN on and the shipped forwarding (public sites only),
// a laptop's Tunnel proxy with the relay line, and curl fetching a public URL
// through it and directly, alternated. One JSON line per fetch. Reaches the internet.
//   mv_import_tests "[.vpn-host-bench]"
//     MV_VPN_BENCH_URL     what to fetch (default 25 MB from speed.cloudflare.com)
//     MV_VPN_BENCH_ROUNDS  fetches of each (default 5)
TEST_CASE("host vpn: a public site through this host's relay, against direct", "[.vpn-host-bench]") {
  REQUIRE(sodium_init() >= 0);
  const char* u = std::getenv("MV_VPN_BENCH_URL");
  const std::string url = u && *u ? u : "https://speed.cloudflare.com/__down?bytes=25000000";
  const char* r = std::getenv("MV_VPN_BENCH_ROUNDS");
  const int rounds = r && *r ? std::atoi(r) : 5;
  scratch_dir s("host_vpn_bench");
  const cli::host_layout l = new_host(s / "data");
  const cli::setup_world w = loopback_world();
  const auto lock = net::try_lock_instance(l.lock);
  REQUIRE(lock);
  std::string why;
  auto host = cli::host_service::start(service_options(l), why);
  REQUIRE(host);
  script t;
  cli::setup_io io = t.io();
  REQUIRE(cli::host_vpn(l, true, 0, io, w) == 0);
  REQUIRE(eventually([&] { return host->snapshot().vpn && host->snapshot().relay_port != 0; }));
  REQUIRE(eventually([&] { return contains(read_text(l.state), "\"vpn\":true"); }));
  REQUIRE(eventually([&] { return !mv::driftwire::tls::available() || host->snapshot().relay_tls_port != 0; }));
  tunnel::proxy::options po;
  po.relay = cli::host_relay_line(l, w, why, &po.tls);
  if (po.tls) po.tls_port = host->snapshot().relay_tls_port;
  REQUIRE_FALSE(po.relay.empty());
  auto proxy = tunnel::proxy::start(po);
  REQUIRE(proxy);
  const std::string through = "http://127.0.0.1:" + std::to_string((*proxy)->port());
  const auto fetch = [&](bool via) {
    std::vector<std::string> args = {"-s", "-o", "/dev/null", "--max-time", "120", "-w",
                                     "%{http_code} %{time_connect} %{time_starttransfer} %{time_total} %{speed_download}"};
    if (via) {
      args.push_back("-x");
      args.push_back(through);
    }
    args.push_back(url);
    mv::io::child_process c;
    REQUIRE(c.start("/usr/bin/curl", args));
    std::string line, out;
    while (c.read_line(line)) out += line;
    (void)c.wait();
    return out;
  };
  for (int i = 0; i < rounds * 2; ++i) {
    const bool via = i % 2 == 1;
    const std::string got = fetch(via);
    int code = 0;
    double connect = 0, first = 0, total = 0, speed = 0;
    REQUIRE(std::sscanf(got.c_str(), "%d %lf %lf %lf %lf", &code, &connect, &first, &total, &speed) == 5);
    CHECK(code == 200);
    std::printf("{\"round\":%d,\"via\":\"%s\",\"code\":%d,\"connect_ms\":%.1f,\"first_byte_ms\":%.1f,\"total_ms\":%.1f,"
                "\"mb_s\":%.2f}\n",
                i, via ? "vpn_host" : "direct", code, connect * 1000, first * 1000, total * 1000, speed / (1 << 20));
    std::fflush(stdout);
  }
  const auto st = host->snapshot();
  CHECK(st.vpn);
  (*proxy).reset();
  host->stop();
}

TEST_CASE("host vpn: a laptop's Tunnel forwards through this host's relay on loopback; never into the LAN",
          "[host-setup][host]") {
  REQUIRE(sodium_init() >= 0);
  scratch_dir s("host_vpn");
  echo_server echo;  // a "site": on loopback, so this test lets the relay reach private addresses
  const cli::host_layout l = new_host(s / "data");
  const cli::setup_world w = loopback_world();
  const auto lock = net::try_lock_instance(l.lock);  // as the supervisor holds it
  REQUIRE(lock);
  auto o = service_options(l);
  std::mutex seen_m;  // the relay starts on the host's loop: what it was given, checked here
  std::vector<std::uint16_t> seen_tcp, seen_udp;
  bool seen_private = true;
  o.relay_tweak = [&](relay::server::options& ro) {
    if (!ro.forward_ports.empty()) {
      {
        std::lock_guard g(seen_m);
        seen_tcp = ro.forward_ports;
        seen_udp = ro.forward_udp_ports;
        seen_private = ro.forward_private;
      }
      ro.forward_ports = {echo.port()};
      ro.forward_private = true;
    }
  };
  std::string why;
  auto host = cli::host_service::start(o, why);
  REQUIRE(host);
  CHECK_FALSE(host->snapshot().vpn);

  script t;
  cli::setup_io io = t.io();
  const bool look = mv::driftwire::tls::available();
  REQUIRE(cli::host_vpn(l, true, 0, io, w) == 0);
  REQUIRE(eventually([&] { return host->snapshot().vpn && host->snapshot().relay_port != 0; }));
  // The web-site look is on by default with the VPN, and the line is its port.
  REQUIRE(eventually([&] { return !look || host->snapshot().relay_tls_port != 0; }));
  const std::uint16_t line_port = look ? host->snapshot().relay_tls_port : host->snapshot().relay_port;
  REQUIRE(eventually([&] {
    return contains(read_text(l.state), "\"relay_port\":" + std::to_string(host->snapshot().relay_port)) &&
           contains(read_text(l.state), "\"relay_tls_port\":" + std::to_string(look ? line_port : 0)) &&
           contains(read_text(l.state), "\"vpn\":true");
  }));
  bool tls = !look;
  std::string line = cli::host_relay_line(l, w, why, &tls);
  REQUIRE_FALSE(line.empty());
  CHECK(tls == look);
  CHECK(line.rfind("127.0.0.1:" + std::to_string(line_port) + "#", 0) == 0);
  // With the look, `vpn on` prints the code that says so (Connect turns the look on).
  if (look) {
    CHECK(contains(t.said, "mediaviewer://relay-tls#127.0.0.1:"));  // printed before the relay took it
    CHECK(contains(t.said, "Connect to a VPN host"));
  }
  {
    std::lock_guard g(seen_m);
    CHECK(seen_tcp == std::vector<std::uint16_t>{relay::kAnyPort});  // all traffic with the VPN on
    CHECK(seen_udp == std::vector<std::uint16_t>{relay::kAnyPort});
    CHECK_FALSE(seen_private);
  }

  // The laptop: its Tunnel proxy with that line (and the look, as Connect sets it), and a site through it.
  tunnel::proxy::options po;
  po.relay = line;
  po.tls = tls;
  po.tls_port = line_port;
  auto proxy = tunnel::proxy::start(po);
  REQUIRE(proxy);
  bool echoed = false;
  CHECK(connect_through((*proxy)->port(), "127.0.0.1:" + std::to_string(echo.port()), &echoed) ==
        "HTTP/1.1 200 Connection established");
  CHECK(echoed);
  CHECK(host->snapshot().vpn);

  // Off: the relay forwards nothing.
  REQUIRE(cli::host_vpn(l, false, 0, io, w) == 0);
  REQUIRE(eventually([&] { return !host->snapshot().vpn; }));
  CHECK(contains(t.said, "The VPN is off"));
  (*proxy).reset();
  po.relay = cli::host_relay_line(l, w, why, &po.tls);
  CHECK_FALSE(po.tls);  // the look goes with the VPN
  auto proxy2 = tunnel::proxy::start(po);
  REQUIRE(proxy2);
  CHECK(connect_through((*proxy2)->port(), "127.0.0.1:" + std::to_string(echo.port())).rfind("HTTP/1.1 200", 0) != 0);
  (*proxy2).reset();
  host->stop();

  // As shipped: forwarding refuses this home's own network (owner call 5), on the VPN's own ports too.
  scratch_dir s2("host_vpn_lan");
  const cli::host_layout l2 = new_host(s2 / "data");
  const auto lock2 = net::try_lock_instance(l2.lock);
  REQUIRE(lock2);
  auto o2 = service_options(l2);
  o2.relay_tweak = [&](relay::server::options& ro) {
    if (!ro.forward_ports.empty()) ro.forward_ports = {echo.port()};  // the echo's port allowed; private addresses not
  };
  auto host2 = cli::host_service::start(o2, why);
  REQUIRE(host2);
  script t2;
  cli::setup_io io2 = t2.io();
  REQUIRE(cli::host_vpn(l2, true, 0, io2, w) == 0);
  REQUIRE(eventually([&] { return host2->snapshot().vpn && contains(read_text(l2.state), "\"vpn\":true"); }));
  REQUIRE(eventually([&] {
    return !look || contains(read_text(l2.state), "\"relay_tls_port\":" +
                                                      std::to_string(host2->snapshot().relay_tls_port));
  }));
  po.relay = cli::host_relay_line(l2, w, why, &po.tls);
  po.tls_port = host2->snapshot().relay_tls_port;
  auto proxy3 = tunnel::proxy::start(po);
  REQUIRE(proxy3);
  const std::string refused = connect_through((*proxy3)->port(), "127.0.0.1:" + std::to_string(echo.port()));
  CHECK(refused.rfind("HTTP/1.1 403", 0) == 0);
  (*proxy3).reset();

  // The web-site look: `--tls-port off` turns it off, and `vpn on` alone (as the desktop's VPN
  // code asks it) leaves it off; `--tls-port N` brings it back on that port.
  if (look) {
    REQUIRE(cli::host_vpn(l2, true, 0, io2, w, true) == 0);
    CHECK_FALSE(cli::host_files::settings(l2).look);
    REQUIRE(eventually([&] { return host2->snapshot().relay_tls_port == 0 && host2->snapshot().vpn; }));
    REQUIRE(cli::host_vpn(l2, true, 0, io2, w) == 0);
    CHECK_FALSE(cli::host_files::settings(l2).look);
    REQUIRE(cli::host_vpn(l2, true, 443, io2, w) == 0);
    CHECK(cli::host_files::settings(l2).look);
    CHECK(cli::host_files::settings(l2).tls_port == 443);
    REQUIRE(eventually([&] { return host2->snapshot().relay_tls_port != 0; }));
    // Off: no look while the VPN is off.
    REQUIRE(cli::host_vpn(l2, false, 0, io2, w) == 0);
    REQUIRE(eventually([&] { return host2->snapshot().relay_tls_port == 0 && !host2->snapshot().vpn; }));
  }
  host2->stop();
}

// docs/plans/mobile.md §12: a code from a library whose owner requires the app lock says so, so
// the phone asks for Face ID or its passcode before it pairs; a code from before it says nothing.
TEST_CASE("pairing code: the owner's lock travels in the code", "[phone-pairing]") {
  mv::ai::catalog::pair_payload p;
  p.library_id = "11111111-2222-3333-4444-555555555555";
  p.library_name = "Home";
  p.publisher_key.assign(32, 0x11);
  p.service_local = "192.168.1.20:7878";
  p.service_identity_hex = std::string(64, '2');
  p.access_key_hex = std::string(64, '3');
  p.code = std::string(32, 'c');
  p.expires = 2000000000;
  const auto open = mv::mobile::parse_pair_payload(mv::ai::catalog::pair_url(p));
  REQUIRE(open);
  CHECK_FALSE(open->lock_required);
  p.lock = true;
  const auto locked = mv::mobile::parse_pair_payload(mv::ai::catalog::pair_url(p));
  REQUIRE(locked);
  CHECK(locked->lock_required);
  CHECK(locked->fingerprint == open->fingerprint);  // the same library, the same check code
}
