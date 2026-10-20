// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire over the internet with a direct code (issue #472, PR 2): the
// addresses a code carries, the code's text, the router's replies, and the
// mapper against stand-in routers on loopback (PCP, NAT-PMP, UPnP-IGD with
// its IPv6 pinhole). No real router or network is used here.
#include "driftwire_fixture.h"

#include "addons/driftwire/handover.h"
#include "addons/driftwire/invite.h"
#include "addons/driftwire/mapping.h"

namespace mp = mv::driftwire::mapping;

namespace {

std::string text_of(const std::string& t) { return t; }

sp::net::address addr(const char* t) {
  auto a = sp::net::parse_address(t);
  REQUIRE(a);
  return *a;
}

std::span<const std::uint8_t> bytes(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// A stand-in router: UDP on loopback for PCP / NAT-PMP, answering by `reply`.
struct udp_router {
  std::unique_ptr<sp::net::udp_socket> sock;
  std::atomic<bool> stop{false};
  std::atomic<int> requests{0};
  std::thread t;
  std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)> reply;
  explicit udp_router(std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)> r) : reply(std::move(r)) {
    auto s = sp::net::open_udp(false, 1, true);
    REQUIRE(s);
    sock = std::move(*s);
    t = std::thread([this] {
      std::uint8_t buf[1100];
      while (!stop.load()) {
        sp::net::endpoint from;
        auto n = sock->receive_from(buf, 50, from);
        if (!n) continue;
        ++requests;
        const auto out = reply({buf, *n});
        if (!out.empty()) (void)sock->send_to(out, from);
      }
    });
  }
  ~udp_router() {
    stop.store(true);
    t.join();
  }
};

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}

// PCP (RFC 6887): MAP answered with `outside`, external port `ext` (0: the internal one).
std::vector<std::uint8_t> pcp_answer(std::span<const std::uint8_t> req, const sp::net::address& outside, std::uint16_t ext,
                                     std::uint8_t result = 0) {
  if (req.size() < 60 || req[0] != 2 || req[1] != 1) return {};
  std::vector<std::uint8_t> r = {2, 0x81, 0, result};
  for (int i = 4; i < 8; ++i) r.push_back(req[i]);  // the lifetime asked
  put32(r, 1);                                       // epoch
  r.insert(r.end(), 12, 0);
  r.insert(r.end(), req.begin() + 24, req.begin() + 36);  // nonce
  r.push_back(6);
  r.insert(r.end(), 3, 0);
  put16(r, be16(req.data() + 40));
  put16(r, ext ? ext : be16(req.data() + 40));
  r.insert(r.end(), 10, 0);
  r.push_back(0xff);
  r.push_back(0xff);
  r.insert(r.end(), outside.bytes.begin(), outside.bytes.begin() + 4);
  return r;
}

// NAT-PMP (RFC 6886): the outside address, and TCP maps.
std::vector<std::uint8_t> nat_pmp_answer(std::span<const std::uint8_t> req, const sp::net::address& outside) {
  if (req.size() >= 2 && req[0] != 0) return {0, static_cast<std::uint8_t>(128 + req[1]), 0, 1};  // a PCP ask: version 0 says no
  if (req.size() == 2 && req[1] == 0) {
    std::vector<std::uint8_t> r = {0, 128, 0, 0};
    put32(r, 7);
    r.insert(r.end(), outside.bytes.begin(), outside.bytes.begin() + 4);
    return r;
  }
  if (req.size() == 12 && req[1] == 2) {
    std::vector<std::uint8_t> r = {0, 130, 0, 0};
    put32(r, 7);
    put16(r, be16(req.data() + 4));
    put16(r, be16(req.data() + 4) == 0 ? 0 : 40001);
    r.insert(r.end(), req.begin() + 8, req.begin() + 12);
    return r;
  }
  return {};
}

// A stand-in UPnP gateway: SSDP on a loopback UDP port, HTTP on a loopback
// TCP port (the description and SOAP).
struct upnp_router {
  std::unique_ptr<sp::net::udp_socket> ssdp;
  std::unique_ptr<sp::net::listener> http;
  std::atomic<bool> stop{false};
  std::thread ssdp_thread, http_thread;
  std::mutex m;
  std::vector<std::string> actions;  // SOAPAction values seen
  std::atomic<int> requests{0};      // every HTTP request, the description's too
  std::string outside = "81.2.69.160";
  std::string wan_service;           // the description's WAN service type ("": the usual)
  std::string existing_443;          // a forward of 443 already there, to this client ("": none)
  bool igd2 = true;
  bool chunked = false;
  upnp_router() {
    auto s = sp::net::open_udp(false, 1, true);
    REQUIRE(s);
    ssdp = std::move(*s);
    auto h = sp::net::listen(true);
    REQUIRE(h);
    http = std::move(*h);
    ssdp_thread = std::thread([this] {
      std::uint8_t buf[2048];
      while (!stop.load()) {
        sp::net::endpoint from;
        auto n = ssdp->receive_from(buf, 50, from);
        if (!n) continue;
        const std::string reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: x\r\nLocation: http://127.0.0.1:" +
                                  std::to_string(http->port()) + "/rootDesc.xml\r\n\r\n";
        (void)ssdp->send_to(bytes(reply), from);
      }
    });
    http_thread = std::thread([this] {
      while (!stop.load()) {
        auto c = http->accept(100);
        if (!c) continue;
        serve(**c);
      }
    });
  }
  ~upnp_router() {
    stop.store(true);
    http->shutdown();
    ssdp_thread.join();
    http_thread.join();
  }
  void serve(sp::link& l) {
    ++requests;
    std::string req;
    std::uint8_t buf[4096];
    // The headers, then Content-Length of body.
    for (;;) {
      auto n = l.read_some(buf, 2000);
      if (!n || *n == 0) return;
      req.append(reinterpret_cast<const char*>(buf), *n);
      const auto end = req.find("\r\n\r\n");
      if (end == std::string::npos) continue;
      const auto cl = req.find("Content-Length: ");
      const std::size_t want = cl == std::string::npos ? 0 : std::stoul(req.substr(cl + 16));
      if (req.size() >= end + 4 + want) break;
    }
    std::string body;
    if (req.rfind("GET /rootDesc.xml", 0) == 0) {
      body = std::string("<?xml version=\"1.0\"?><root><device><deviceList><device><serviceList>") +
             "<service><serviceType>" +
             (wan_service.empty() ? std::string("urn:schemas-upnp-org:service:WANIPConnection:") + (igd2 ? "2" : "1")
                                  : wan_service) +
             "</serviceType><controlURL>/ctl/IPConn</controlURL></service>" +
             (igd2 ? "<service><serviceType>urn:schemas-upnp-org:service:WANIPv6FirewallControl:1</serviceType>"
                     "<controlURL>ctl/IP6FCtl</controlURL></service>"
                   : "") +
             "</serviceList></device></deviceList></device></root>";
    } else {
      const auto a = req.find("SOAPAction: \"");
      const std::string action = a == std::string::npos ? "" : req.substr(a + 13, req.find('"', a + 13) - a - 13);
      {
        std::lock_guard lock(m);
        actions.push_back(action);
      }
      const std::string op = action.substr(action.find('#') + 1);
      body = "<?xml version=\"1.0\"?><s:Envelope><s:Body><u:" + op + "Response>";
      if (op == "GetExternalIPAddress") body += "<NewExternalIPAddress>" + outside + "</NewExternalIPAddress>";
      if (op == "AddPinhole") body += "<UniqueID>42</UniqueID>";
      if (op == "GetSpecificPortMappingEntry") {
        if (existing_443.empty() || req.find("<NewExternalPort>443<") == std::string::npos) {
          // UPnP's "no such entry" (714).
          const std::string fault = "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>714</errorCode>"
                                    "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
          (void)l.write_all(bytes("HTTP/1.1 500 Internal Server Error\r\nContent-Length: " +
                                  std::to_string(fault.size()) + "\r\n\r\n" + fault));
          l.shutdown();
          return;
        }
        body += "<NewInternalPort>443</NewInternalPort><NewInternalClient>" + existing_443 +
                "</NewInternalClient><NewPortMappingDescription>web</NewPortMappingDescription>";
      }
      body += "</u:" + op + "Response></s:Body></s:Envelope>";
    }
    std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n";
    if (chunked) {
      out += "Transfer-Encoding: chunked\r\n\r\n";
      const std::size_t half = body.size() / 2;
      char line[16];
      std::snprintf(line, sizeof line, "%zx\r\n", half);
      out += line + body.substr(0, half) + "\r\n";
      std::snprintf(line, sizeof line, "%zx\r\n", body.size() - half);
      out += line + body.substr(half) + "\r\n0\r\n\r\n";
    } else {
      out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    (void)l.write_all(bytes(out));
    l.shutdown();
  }
  std::vector<std::string> seen() {
    std::lock_guard lock(m);
    return actions;
  }
};

mp::options on_loopback(std::uint16_t gateway_port, std::uint16_t ssdp_port = 9) {
  mp::options o;
  o.gateway = addr("127.0.0.1");
  o.gateway_port = gateway_port;
  o.ssdp = {"127.0.0.1", ssdp_port};
  o.timeout_ms = 800;
  return o;
}

}  // namespace

// ---- addresses ---------------------------------------------------------------------------------

TEST_CASE("driftwire internet: addresses parse and print as RFC 5952 says", "[driftwire]") {
  struct row {
    const char* in;
    const char* out;
  };
  for (const row r : {row{"192.0.2.1", "192.0.2.1"}, row{"::", "::"}, row{"::1", "::1"},
                      row{"2001:DB8:0:0:8:800:200C:417A", "2001:db8::8:800:200c:417a"},
                      row{"2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"}, row{"2001:0db8::0001", "2001:db8::1"},
                      row{"1:0:0:2:0:0:0:3", "1:0:0:2::3"}, row{"::ffff:192.0.2.128", "::ffff:c000:280"},
                      row{"fe80::1", "fe80::1"}, row{"1::", "1::"}, row{"1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0"}}) {
    INFO(r.in);
    const auto a = sp::net::parse_address(r.in);
    REQUIRE(a);
    CHECK(sp::net::address_text(*a) == r.out);
    CHECK(sp::net::parse_address(sp::net::address_text(*a)) == a);
  }
  for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1::2::3", ":1", "1:", "1:::2", "g::1",
                          "12345::1", "1:2:3:4:5:6:7:8:9", "::1.2.3", "1.2.3.4:80", "[::1]"}) {
    INFO(bad);
    CHECK_FALSE(sp::net::parse_address(bad));
  }
}

TEST_CASE("driftwire internet: only addresses the internet can reach count as public", "[driftwire]") {
  for (const char* yes : {"81.2.69.160", "8.8.8.8", "2a00:1450:4009:81f::200e", "2606:4700::1111"}) {
    INFO(yes);
    CHECK(sp::net::is_public(addr(yes)));
  }
  for (const char* no : {"10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "100.64.0.1", "100.127.255.1",
                         "127.0.0.1", "169.254.1.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "192.0.2.1",
                         "198.51.100.1", "203.0.113.1", "198.18.0.1", "::1", "::", "fe80::1", "fd00::1", "fc12::1",
                         "ff02::1", "2001:db8::1", "2002:c000:0204::1", "::ffff:8.8.8.8"}) {
    INFO(no);
    CHECK_FALSE(sp::net::is_public(addr(no)));
  }
}

// ---- the code ----------------------------------------------------------------------------------

TEST_CASE("driftwire internet: a code carries its secret and places, and a typo is caught", "[driftwire]") {
  const std::vector<sp::net::endpoint> places = {{"81.2.69.160", 41000}, {"2a00:1450:4009:81f::200e", 443},
                                                 {"192.168.1.20", 41000}};
  const auto c = sp::invite::make(places);
  REQUIRE(c.places.size() == 3);
  const std::string text = sp::invite::encode(c);
  INFO(text);
  CHECK(text.size() <= 90);  // three places, one IPv6: the longest a code usually gets
  auto back = sp::invite::decode(text);
  REQUIRE(back);
  CHECK(back->secret == c.secret);
  REQUIRE(back->places.size() == 3);
  CHECK(back->places[1].address == "2a00:1450:4009:81f::200e");
  CHECK(back->places[1].port == 443);
  // Pasted lower-case, without the dashes, with O for 0: the same code.
  std::string loose;
  for (char ch : text) {
    if (ch == '-') continue;
    loose.push_back(ch == '0' ? 'o' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  CHECK(sp::invite::decode(loose));
  // The link form.
  const std::string link = sp::invite::link(c);
  CHECK(link.rfind(sp::invite::kLinkPrefix, 0) == 0);
  auto from_link = sp::invite::decode(link);
  REQUIRE(from_link);
  CHECK(from_link->secret == c.secret);
  // A mistyped character is caught here, not spent on the receiver.
  int caught = 0, tries = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '-') continue;
    std::string typo = text;
    typo[i] = text[i] == 'Z' ? 'Y' : 'Z';
    ++tries;
    caught += sp::invite::decode(typo) ? 0 : 1;
  }
  CHECK(caught >= tries - 1);  // a two-byte check: one in 65 536 slips through
  // An IPv4-only code is short enough to read out.
  const auto small = sp::invite::make({{"81.2.69.160", 41000}});
  CHECK(sp::invite::encode(small).size() <= 32);
  CHECK(text_of(sp::invite::encode(small)).find('-') == 4);
  for (const char* junk : {"", "ABCD", "not a code at all", "mediaviewer://driftwire?code="}) CHECK_FALSE(sp::invite::decode(junk));
}

// ---- the router's replies ------------------------------------------------------------------------

TEST_CASE("driftwire internet: UPnP's answers are read, and only a local gateway is followed", "[driftwire]") {
  const auto gw = addr("192.168.1.1");
  CHECK(mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\n\r\n", gw) ==
        "http://192.168.1.1:5000/rootDesc.xml");
  // A public or named host is not the router on this network: not followed.
  CHECK_FALSE(mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: http://81.2.69.160/x.xml\r\n\r\n", gw));
  CHECK_FALSE(mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: http://router.example/x.xml\r\n\r\n", gw));
  CHECK_FALSE(mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: https://192.168.1.1/x.xml\r\n\r\n", gw));
  const std::string xml =
      "<root><URLBase>http://192.168.1.1:5000/</URLBase><service><serviceType>urn:schemas-upnp-org:service:"
      "WANPPPConnection:1</serviceType><controlURL>ctl/PPP</controlURL></service><service><serviceType>urn:"
      "schemas-upnp-org:service:WANIPConnection:2</serviceType><controlURL>/ctl/IP</controlURL></service></root>";
  const auto d = mp::parse_description(xml, "http://192.168.1.1:5000/rootDesc.xml", gw);
  REQUIRE(d);
  CHECK(d->service == "urn:schemas-upnp-org:service:WANIPConnection:2");
  CHECK(d->control == "http://192.168.1.1:5000/ctl/IP");
  const auto chunked = mp::parse_http("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
  REQUIRE(chunked);
  CHECK(chunked->status == 200);
  CHECK(chunked->body == "Wikipedia");
  CHECK(mp::xml_value("<a:B x=\"1\"> v </a:B>", "B") == "v");
  CHECK_FALSE(mp::parse_http("garbage"));
}

TEST_CASE("driftwire internet: a hostile SSDP answer or device description is refused (#499)", "[driftwire]") {
  const auto gw = addr("192.168.1.1");
  auto ssdp = [&](const std::string& location) {
    return mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: " + location + "\r\n\r\n", gw);
  };
  // Another host on the network, this machine, 0/8, link-local, multicast:
  // only the gateway itself is followed.
  for (const char* bad : {"http://192.168.1.77:5000/d.xml", "http://127.0.0.1:631/", "http://0.0.0.0:80/",
                          "http://169.254.1.1/x", "http://224.0.0.1/x", "http://255.255.255.255/x",
                          "http://192.168.1.1:5000/a b", "http://192.168.1.1:0/"}) {
    INFO(bad);
    CHECK_FALSE(ssdp(bad));
  }
  // Even with no gateway to compare with (never so in the mapper), loopback
  // and the rest are not a router.
  for (const char* bad : {"127.0.0.1", "0.0.0.0", "169.254.0.1", "239.255.255.250"}) {
    INFO(bad);
    CHECK_FALSE(mp::parse_ssdp_location("HTTP/1.1 200 OK\r\nLOCATION: http://" + std::string(bad) + "/\r\n\r\n", addr("192.168.1.1")));
  }
  CHECK(mp::service_type("urn:schemas-upnp-org:service:WANIPConnection:2"));
  for (const char* bad : {"", "urn:", "urn:x\r\nHost: evil", "urn:x\"y", "urn:x y", "urn:x#y", "http:x", "urn:x\n"}) {
    INFO(bad);
    CHECK_FALSE(mp::service_type(bad));
  }
  auto described = [&](const std::string& base, const std::string& type, const std::string& control) {
    const std::string xml = "<root>" + (base.empty() ? "" : "<URLBase>" + base + "</URLBase>") +
                            "<service><serviceType>" + type + "</serviceType><controlURL>" + control +
                            "</controlURL></service></root>";
    return mp::parse_description(xml, "http://192.168.1.1:5000/rootDesc.xml", gw);
  };
  const std::string wan = "urn:schemas-upnp-org:service:WANIPConnection:1";
  // A CR/LF in the service type was a second HTTP request to the control URL.
  CHECK_FALSE(described("", wan + "#x\"\r\n\r\nPOST /evil HTTP/1.1\r\nX: \"", "/ctl"));
  CHECK_FALSE(described("", "urn:schemas-upnp-org:service:WANIPConnection:1 x", "/ctl"));
  // A control URL anywhere but the gateway.
  for (const char* bad : {"http://127.0.0.1:8080/", "http://0.0.0.0/c", "http://169.254.169.254/latest", "http://192.168.1.2/c",
                          "http://81.2.69.160/c"}) {
    INFO(bad);
    CHECK_FALSE(described("", wan, bad));
  }
  // A URLBase elsewhere is not used: relative URLs stay at the gateway.
  const auto d = described("http://127.0.0.1:631/", wan, "/ctl");
  REQUIRE(d);
  CHECK(d->control == "http://192.168.1.1:5000/ctl");
  // A location not at the gateway.
  CHECK_FALSE(mp::parse_description("<root/>", "http://192.168.1.9:5000/d.xml", gw));
}

// ---- the mapper, against stand-in routers -----------------------------------------------------------

TEST_CASE("driftwire internet: PCP maps a port and removes it", "[driftwire]") {
  const auto outside = addr("81.2.69.160");
  std::atomic<int> deletes{0};
  udp_router router([&](std::span<const std::uint8_t> req) {
    if (req.size() >= 8 && req[4] == 0 && req[5] == 0 && req[6] == 0 && req[7] == 0) ++deletes;
    return pcp_answer(req, outside, 443);
  });
  mp::mapper m(on_loopback(router.sock->port()));
  auto got = m.map_tcp(41000, 443, 3600);
  REQUIRE(got);
  CHECK(got->how == mp::method::pcp);
  CHECK(got->external.address == "81.2.69.160");
  CHECK(got->external.port == 443);
  m.remove(*got);
  CHECK(deletes.load() == 1);
}

TEST_CASE("driftwire internet: a NAT-PMP router answers when PCP is not spoken", "[driftwire]") {
  const auto outside = addr("81.2.69.160");
  udp_router router([&](std::span<const std::uint8_t> req) { return nat_pmp_answer(req, outside); });
  mp::mapper m(on_loopback(router.sock->port()));
  auto got = m.map_tcp(41000, 0, 3600);
  REQUIRE(got);
  CHECK(got->how == mp::method::nat_pmp);
  CHECK(got->external.address == "81.2.69.160");
  CHECK(got->external.port == 40001);
  REQUIRE(m.notes().size() >= 2);
  CHECK(m.notes()[0] == "PCP: no answer");
}

TEST_CASE("driftwire internet: a router behind another NAT is reported, not used", "[driftwire]") {
  const auto cgnat = addr("100.64.12.34");
  udp_router router([&](std::span<const std::uint8_t> req) { return pcp_answer(req, cgnat, 0); });
  mp::mapper m(on_loopback(router.sock->port()));
  CHECK_FALSE(m.map_tcp(41000, 0, 3600));
  CHECK(m.behind_another_nat());
}

TEST_CASE("driftwire internet: UPnP-IGD maps a port, opens an IPv6 pinhole, and removes both", "[driftwire]") {
  for (const bool chunked : {false, true}) {
    upnp_router upnp;
    upnp.chunked = chunked;
    // No PCP / NAT-PMP answer (a port nobody listens on): UPnP is asked.
    auto quiet = sp::net::open_udp(false, 1, true);
    REQUIRE(quiet);
    mp::mapper m(on_loopback((*quiet)->port(), upnp.ssdp->port()));
    auto got = m.map_tcp(41000, 0, 3600);
    INFO("chunked " << chunked);
    REQUIRE(got);
    CHECK(got->how == mp::method::upnp);
    CHECK(got->external.address == "81.2.69.160");
    CHECK(got->external.port == 41000);
    auto hole = m.pinhole_v6(addr("2a00:1450:4009:81f::200e"), 41000, 3600);
    REQUIRE(hole);
    CHECK(hole->pinhole_id == "42");
    m.remove(*got);
    m.remove(*hole);
    const auto seen = upnp.seen();
    auto has = [&](const char* op) {
      return std::any_of(seen.begin(), seen.end(), [&](const std::string& s) { return s.find(op) != std::string::npos; });
    };
    CHECK(has("#GetExternalIPAddress"));
    CHECK(has("#AddPortMapping"));
    CHECK(has("WANIPv6FirewallControl:1#AddPinhole"));
    CHECK(has("#DeletePortMapping"));
    CHECK(has("#DeletePinhole"));
  }
}

TEST_CASE("driftwire internet: UPnP follows only the gateway's SSDP answer, and refuses a smuggling service type", "[driftwire]") {
  {
    // The stand-in answers SSDP from 127.0.0.1; the gateway is elsewhere
    // (another device answering on the network): not followed.
    upnp_router upnp;
    auto quiet = sp::net::open_udp(false, 1, true);
    REQUIRE(quiet);
    auto o = on_loopback((*quiet)->port(), upnp.ssdp->port());
    o.gateway = addr("127.0.0.2");
    o.timeout_ms = 400;
    mp::mapper m(o);
    CHECK_FALSE(m.map_tcp(41000, 0, 120));
    CHECK(std::find(m.notes().begin(), m.notes().end(), "UPnP: no gateway answered") != m.notes().end());
    CHECK(upnp.requests.load() == 0);  // not even its description was fetched
  }
  {
    upnp_router upnp;
    upnp.wan_service = "urn:schemas-upnp-org:service:WANIPConnection:1#X\"\r\nContent-Length: 0\r\n\r\nPOST /x HTTP/1.1\r\nA: \"";
    auto quiet = sp::net::open_udp(false, 1, true);
    REQUIRE(quiet);
    mp::mapper m(on_loopback((*quiet)->port(), upnp.ssdp->port()));
    CHECK_FALSE(m.map_tcp(41000, 0, 120));
    CHECK(upnp.seen().empty());  // no SOAP request at all
  }
}

TEST_CASE("driftwire internet: a port someone else forwards is not taken over", "[driftwire]") {
  upnp_router upnp;
  upnp.igd2 = false;
  upnp.existing_443 = "192.168.1.50";  // the person's own forward
  auto quiet = sp::net::open_udp(false, 1, true);
  REQUIRE(quiet);
  mp::mapper m(on_loopback((*quiet)->port(), upnp.ssdp->port()));
  CHECK_FALSE(m.map_tcp(41000, 443, 120));
  const auto seen = upnp.seen();
  CHECK(std::none_of(seen.begin(), seen.end(), [](const std::string& s) { return s.find("#AddPortMapping") != std::string::npos; }));
}

TEST_CASE("driftwire internet: mappings are renewed while held and removed after (#499)", "[driftwire]") {
  const auto outside = addr("81.2.69.160");
  std::atomic<int> maps{0}, deletes{0};
  udp_router router([&](std::span<const std::uint8_t> req) {
    if (req.size() >= 8 && req[4] == 0 && req[5] == 0 && req[6] == 0 && req[7] == 0) {
      ++deletes;
    } else {
      ++maps;
    }
    return pcp_answer(req, outside, 41000);
  });
  auto mapper = std::make_unique<mp::mapper>(on_loopback(router.sock->port()));
  auto got = mapper->map_tcp(41000, 0, 2);
  REQUIRE(got);
  CHECK(maps.load() == 1);
  {
    mp::keeper k(std::move(mapper), {*got}, 2);  // renewed every second
    std::this_thread::sleep_for(std::chrono::milliseconds(2600));
    CHECK(maps.load() >= 3);
    CHECK(deletes.load() == 0);
  }
  CHECK(deletes.load() == 1);
}

// ---- the internet handshake ------------------------------------------------------------------------

namespace {
struct internet_pair {
  mv::result<std::unique_ptr<sp::channel>> sender = mv::err(mv::status::internal);
  mv::result<std::unique_ptr<sp::channel>> receiver = mv::err(mv::status::internal);
};
internet_pair open_internet(const std::array<std::uint8_t, 6>& typed, std::optional<std::array<std::uint8_t, 6>> shown) {
  auto [a, b] = loopback_pair();
  internet_pair p;
  std::thread t([&, l = std::shared_ptr<sp::link>(std::move(b))]() mutable {
    auto h = sp::channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (!h) {
      p.receiver = mv::err(h.error());
      return;
    }
    sp::channel::options o;
    if (shown) o.internet_secret = std::vector<std::uint8_t>(shown->begin(), shown->end());
    p.receiver = sp::channel::open_server(l, std::move(*h), "bbbbbbbbbbbbbbbb", "Bea", o);
  });
  p.sender = sp::channel::open_internet(std::shared_ptr<sp::link>(std::move(a)), typed, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  return p;
}
}  // namespace

TEST_CASE("driftwire internet: the right code opens both ends, with the same check", "[driftwire]") {
  const std::array<std::uint8_t, 6> secret = {1, 2, 3, 4, 5, 6};
  auto p = open_internet(secret, secret);
  REQUIRE(p.sender);
  REQUIRE(p.receiver);
  CHECK((*p.sender)->internet());
  CHECK((*p.receiver)->internet());
  CHECK((*p.sender)->code() == (*p.receiver)->code());
  CHECK((*p.sender)->code().size() == 7);
  CHECK((*p.sender)->resumable());
  CHECK((*p.receiver)->peer_name() == "Ann");
  REQUIRE((*p.sender)->send(sp::msg::offer, std::string_view("{\"n\":1}")));
  sp::message m;
  REQUIRE((*p.receiver)->receive(m, 3000));
  CHECK(m.text() == "{\"n\":1}");
}

TEST_CASE("driftwire internet: a wrong code is refused at the receiver's first read", "[driftwire]") {
  const std::array<std::uint8_t, 6> shown = {1, 2, 3, 4, 5, 6};
  std::array<std::uint8_t, 6> typed = shown;
  typed[5] ^= 1;
  auto p = open_internet(typed, shown);
  REQUIRE_FALSE(p.receiver);
  CHECK(p.receiver.error() == mv::status::permission_denied);
  REQUIRE_FALSE(p.sender);
  // And with no code waiting, an internet hello is simply closed.
  auto q = open_internet(shown, std::nullopt);
  REQUIRE_FALSE(q.receiver);
  CHECK(q.receiver.error() == mv::status::not_found);
  REQUIRE_FALSE(q.sender);
}

// ---- two engines over a code ----------------------------------------------------------------------

namespace {

// One MediaViewer on "the internet": no LAN beacon heard, no Bluetooth; its
// codes point at its loopback addresses and no router is asked.
struct far_client {
  fake_host host;
  sp::engine eng;
  far_client(const fs::path& received, const std::string& name, int code_ms = sp::kCodeLifetimeMs)
      : host(received), eng(host, options(name, code_ms)) {
    eng.screen_opened("");
    eng.on_event("internet", 2, "true");
  }
  static sp::engine::options options(const std::string& name, int code_ms) {
    sp::engine::options o;
    o.my_name = name;
    o.use_bluetooth = false;
    o.loopback_only = true;
    o.make_discovery = [] { return std::make_unique<inject_discovery>(); };
    o.internet_loopback = true;
    o.internet_mapping = std::nullopt;
    o.code_lifetime_ms = code_ms;
    return o;
  }
  std::string make_code() {
    eng.on_event("make_code", 1, "");
    std::string code;
    REQUIRE(eventually([&] {
      const auto v = eng.snapshot();
      code = v.code_text;
      return v.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::ready) && !code.empty();
    }));
    return code;
  }
  void send_to(const std::string& code) {
    eng.on_event("send_code", 2, "\"" + code + "\"");
    eng.on_event("send_to_code", 1, "");
  }
  void pick(const std::vector<fs::path>& files) {
    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(utf8(f));
    eng.set_files(paths);
    REQUIRE(eventually([&] {
      const auto v = eng.snapshot();
      return !v.reading_folder && v.file_count >= files.size();
    }));
  }
};

}  // namespace

TEST_CASE("driftwire internet: a code sends files, with the name confirmed and the same check on both screens",
          "[driftwire]") {
  scratch_dir s("driftwire_code_send");
  far_client bea(s / "bea_received", "Bea");
  far_client ann(s / "ann_received", "Ann");
  const fs::path f = s / "out" / "clip.mov";
  write_bytes(f, pattern(3 * 1024 * 1024 + 17, 9));
  ann.pick({f});
  const std::string code = bea.make_code();
  ann.send_to(code);
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::naming; }));
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::connecting; }));
  const auto a = ann.eng.snapshot();
  const auto b = bea.eng.snapshot();
  CHECK(a.out_peer == "Bea");
  CHECK(b.in_peer == "Ann");
  CHECK(b.in_internet);
  CHECK(a.out_check == b.in_code);  // the check, the same on both screens
  CHECK(a.out_via == "the internet");
  // The code is spent the moment it is tried.
  CHECK(b.code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::used));
  ann.eng.on_event("out_confirm", 1, "");
  REQUIRE(eventually([&] { return bea.eng.snapshot().in == sp::view::in_phase::asking; }));
  bea.eng.on_event("accept", 1, "");
  ann.eng.wait_idle();
  bea.eng.wait_idle();
  INFO(ann.eng.snapshot().out_note);
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::done);
  CHECK(read_bytes(s / "bea_received" / "clip.mov") == read_bytes(f));
  CHECK_FALSE(any_temporary(s / "bea_received"));

  // The same code again: it worked once, so it does not work now.
  far_client cy(s / "cy_received", "Cy");
  cy.pick({f});
  cy.send_to(code);
  REQUIRE(eventually([&] { return cy.eng.snapshot().out == sp::view::out_phase::ended ||
                                  cy.eng.snapshot().out_attempt >= 2; }, 20));
  cy.eng.on_event("out_cancel", 1, "");
  cy.eng.wait_idle();
  CHECK(cy.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(bea.eng.snapshot().in != sp::view::in_phase::asking);
}

TEST_CASE("driftwire internet: a wrong code is refused, and the code it guessed at dies", "[driftwire]") {
  scratch_dir s("driftwire_code_wrong");
  far_client bea(s / "bea_received", "Bea");
  far_client mal(s / "mal_received", "Mallory");
  far_client ann(s / "ann_received", "Ann");
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(1000, 3));
  mal.pick({f});
  ann.pick({f});
  const std::string code = bea.make_code();
  // Mallory knows where Bea is (the places) but not the secret.
  auto c = sp::invite::decode(code);
  REQUIRE(c);
  c->secret[0] ^= 0x5a;
  mal.send_to(sp::invite::encode(*c));
  mal.eng.wait_idle();
  CHECK(mal.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(mal.eng.snapshot().out_note.find("did not work") != std::string::npos);
  REQUIRE(eventually([&] { return bea.eng.snapshot().code_note.find("wrong") != std::string::npos; }));
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);  // nothing was offered there
  // The real code, after the wrong one: dead too (one attempt per code).
  ann.send_to(code);
  REQUIRE(eventually([&] { return ann.eng.snapshot().out == sp::view::out_phase::ended ||
                                  ann.eng.snapshot().out_attempt >= 2; }, 20));
  ann.eng.on_event("out_cancel", 1, "");
  ann.eng.wait_idle();
  CHECK(bea.eng.snapshot().in == sp::view::in_phase::none);
}

TEST_CASE("driftwire internet: an expired code is refused, and the sender says why and keeps trying until Cancel",
          "[driftwire]") {
  scratch_dir s("driftwire_code_expired");
  far_client bea(s / "bea_received", "Bea", 300);
  far_client ann(s / "ann_received", "Ann");
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(1000, 4));
  ann.pick({f});
  const std::string code = bea.make_code();
  REQUIRE(eventually([&] {
    return bea.eng.snapshot().code_phase == static_cast<std::uint8_t>(sp::internet_receiver::phase::expired);
  }));
  ann.send_to(code);
  // The listener closed with the code: refused, round after round.
  REQUIRE(eventually([&] { return ann.eng.snapshot().out_attempt >= 2; }, 10));
  const auto v = ann.eng.snapshot();
  CHECK(v.out == sp::view::out_phase::reaching);
  CHECK(v.out_note.find("not listening") != std::string::npos);
  ann.eng.on_event("out_cancel", 1, "");
  ann.eng.wait_idle();
  CHECK(ann.eng.snapshot().out == sp::view::out_phase::ended);
  CHECK(ann.eng.snapshot().out_note == "Cancelled. Nothing was sent.");
}

TEST_CASE("driftwire internet: what is not a code is caught before anything is tried", "[driftwire]") {
  scratch_dir s("driftwire_code_junk");
  far_client ann(s / "ann_received", "Ann");
  const fs::path f = s / "out" / "a.jpg";
  write_bytes(f, pattern(1000, 5));
  ann.pick({f});
  ann.send_to("HELLO-WORL-D123");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto v = ann.eng.snapshot();
  CHECK(v.out == sp::view::out_phase::idle);
  CHECK(v.send_code_note.find("not a whole code") != std::string::npos);
}

TEST_CASE("driftwire internet: the offer asks for places, and places are read with care", "[driftwire]") {
  // #472 PR 5: only a sender that says so is sent msg::places.
  const std::vector<sp::outgoing_file> files{{"/x/a.jpg", "a.jpg", 10, 0}};
  auto plain = sp::parse_offer(sp::offer_json(files));
  auto asks = sp::parse_offer(sp::offer_json(files, true));
  REQUIRE(plain);
  REQUIRE(asks);
  CHECK_FALSE(plain->handover);
  CHECK(asks->handover);
  // #472: "dht" only rides with "handover"; the places say so apart.
  auto dht_too = sp::parse_offer(sp::offer_json(files, true, true));
  auto dht_alone = sp::parse_offer(sp::offer_json(files, false, true));
  REQUIRE(dht_too);
  REQUIRE(dht_alone);
  CHECK(dht_too->dht);
  CHECK_FALSE(asks->dht);
  CHECK_FALSE(dht_alone->dht);
  CHECK(sp::places_dht(R"({"places":[],"dht":true})"));
  CHECK_FALSE(sp::places_dht(R"({"places":[]})"));
  CHECK_FALSE(sp::places_dht("junk"));
  {
    // The session's DHT keys: the same from the same secret and count of
    // rejoins, others from another secret or after another move.
    std::array<std::uint8_t, 32> a{}, b{};
    b[0] = 1;
    const auto ka = sp::handover_keys(a, 0), ka2 = sp::handover_keys(a, 0), kb = sp::handover_keys(b, 0);
    const auto ka1 = sp::handover_keys(a, 1);
    CHECK(ka.public_key == ka2.public_key);
    CHECK(ka.salt_base == ka2.salt_base);
    CHECK(ka.public_key != kb.public_key);
    CHECK(ka.box != kb.box);
    CHECK(ka.public_key != ka1.public_key);
    CHECK(ka.salt_base != ka1.salt_base);
  }
  const auto p = sp::parse_places(
      R"({"places":[{"a":"203.0.113.5","p":41000},{"a":"2001:db8::5","p":443},{"a":"not an address","p":1},)"
      R"({"a":"198.51.100.1","p":0},{"a":"198.51.100.2","p":70000},{"p":5},{"a":"198.51.100.3"}]})");
  REQUIRE(p.size() == 2);
  CHECK(p[0].address == "203.0.113.5");
  CHECK(p[0].port == 41000);
  CHECK(p[1].address == "2001:db8::5");
  std::string many = R"({"places":[)";
  for (int i = 0; i < 20; ++i) many += std::string(i ? "," : "") + R"({"a":"203.0.113.)" + std::to_string(i) + R"(","p":1})";
  many += "]}";
  CHECK(sp::parse_places(many).size() == sp::kMaxPlaces);
  CHECK(sp::parse_places("").empty());
  CHECK(sp::parse_places(R"({"places":5})").empty());
}

// #472 "Retries": failed rounds wait 1 s, 2 s, 5 s, 10 s, then every 30 s.
TEST_CASE("driftwire internet: failed rounds back off 1, 2, 5, 10, then 30 s", "[driftwire]") {
  CHECK(sp::backoff_ms(0) == 0);
  CHECK(sp::backoff_ms(-3) == 0);
  CHECK(sp::backoff_ms(1) == 1000);
  CHECK(sp::backoff_ms(2) == 2000);
  CHECK(sp::backoff_ms(3) == 5000);
  CHECK(sp::backoff_ms(4) == 10000);
  CHECK(sp::backoff_ms(5) == 30000);
  CHECK(sp::backoff_ms(1000) == 30000);
}
