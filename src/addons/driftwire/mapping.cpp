// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/mapping.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mv::driftwire::mapping {

namespace {

using clock = std::chrono::steady_clock;

constexpr std::uint8_t kPcpVersion = 2;
constexpr std::uint8_t kPcpMap = 1;
constexpr std::uint8_t kTcp = 6;
constexpr std::size_t kMaxHttp = 64 * 1024;
constexpr const char* kDescription = "MediaViewer Driftwire";

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
         static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// IPv4 as PCP writes it: ::ffff:a.b.c.d.
void put_mapped(std::vector<std::uint8_t>& b, const net::address& a) {
  if (a.v6) {
    b.insert(b.end(), a.bytes.begin(), a.bytes.end());
    return;
  }
  for (int i = 0; i < 10; ++i) b.push_back(0);
  b.push_back(0xff);
  b.push_back(0xff);
  b.insert(b.end(), a.bytes.begin(), a.bytes.begin() + 4);
}

net::address from_mapped(const std::uint8_t* p) {
  net::address a;
  static constexpr std::uint8_t kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (std::memcmp(p, kPrefix, 12) == 0) {
    std::memcpy(a.bytes.data(), p + 12, 4);
  } else {
    a.v6 = true;
    std::memcpy(a.bytes.data(), p, 16);
  }
  return a;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// One request to the gateway's UDP port, retransmitted at 250 ms doubling
// (RFC 6886 §3.1, shortened), until a reply that `accept` takes or the timeout.
template <typename Accept>
bool ask_gateway(const net::endpoint& gw, std::span<const std::uint8_t> request, int timeout_ms,
                 const std::function<bool()>& cancelled, Accept&& accept) {
  auto sock = net::open_udp(false);
  if (!sock) return false;
  const auto until = clock::now() + std::chrono::milliseconds(timeout_ms);
  auto next_send = clock::now();
  int wait = 250;
  std::uint8_t buf[1100];
  while (clock::now() < until) {
    if (cancelled && cancelled()) return false;
    if (clock::now() >= next_send) {
      (void)(*sock)->send_to(request, gw);
      next_send = clock::now() + std::chrono::milliseconds(wait);
      wait *= 2;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(std::min(until, next_send) - clock::now());
    net::endpoint from;
    auto n = (*sock)->receive_from(buf, static_cast<int>(std::max<long long>(1, left.count())), from);
    if (!n) continue;
    // Only the gateway's own port answers count (RFC 6886 §3.1).
    if (from.address != gw.address || from.port != gw.port) continue;
    if (accept(std::span<const std::uint8_t>(buf, *n))) return true;
  }
  return false;
}

// Where a router on this network can be (#499): an IPv4 literal that is
// neither public nor this machine (loopback), nor 0/8, link-local, multicast
// or broadcast. Loopback only as the gateway itself (the tests' stand-in
// router); with `gateway`, that address and no other.
bool router_address(const net::address& a, const net::address* gateway) {
  if (gateway && a != *gateway) return false;
  if (a.v6 || net::is_public(a)) return false;
  const std::uint8_t b0 = a.bytes[0], b1 = a.bytes[1];
  return b0 != 0 && (b0 != 127 || gateway) && !(b0 == 169 && b1 == 254) && b0 < 224;
}

}  // namespace

bool service_type(std::string_view s) {
  constexpr std::string_view kUrn = "urn:";
  if (s.size() <= kUrn.size() || s.size() > 256 || s.substr(0, kUrn.size()) != kUrn) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' ||
           c == '-';
  });
}

namespace {

// "http://192.168.1.1:5000/path" into its parts; only a host
// router_address() takes.
struct url {
  net::endpoint host;
  std::string path;
};
std::optional<url> parse_url(std::string_view u, const net::address* gateway) {
  constexpr std::string_view kScheme = "http://";
  if (u.size() > 1024 || lower(u.substr(0, kScheme.size())) != kScheme) return std::nullopt;
  u.remove_prefix(kScheme.size());
  const auto slash = u.find('/');
  const std::string_view hostport = u.substr(0, slash);
  url out;
  out.path = slash == std::string_view::npos ? "/" : std::string(u.substr(slash));
  const auto colon = hostport.rfind(':');
  const std::string_view host = colon == std::string_view::npos ? hostport : hostport.substr(0, colon);
  std::uint32_t port = 80;
  if (colon != std::string_view::npos) {
    port = 0;
    const std::string_view p = hostport.substr(colon + 1);
    if (p.empty() || p.size() > 5) return std::nullopt;
    for (const char c : p) {
      if (c < '0' || c > '9') return std::nullopt;
      port = port * 10 + static_cast<std::uint32_t>(c - '0');
    }
  }
  const auto ip = net::parse_address(host);
  if (!ip || ip->v6 || !router_address(*ip, gateway) || port == 0 || port > 65535) return std::nullopt;
  out.host = {std::string(host), static_cast<std::uint16_t>(port)};
  for (const char c : out.path) {
    if (static_cast<unsigned char>(c) < 0x21 || c == 0x7f) return std::nullopt;  // no spaces or controls in a request line
  }
  return out;
}

// One HTTP request to the router; the whole reply (Connection: close).
std::optional<http_reply> http(const url& to, std::string_view method, std::string_view headers, std::string_view body,
                               int timeout_ms) {
  auto l = net::connect(to.host, timeout_ms);
  if (!l) return std::nullopt;
  std::string req = std::string(method) + " " + to.path + " HTTP/1.1\r\nHost: " + to.host.address + ":" +
                    std::to_string(to.host.port) + "\r\nConnection: close\r\n" + std::string(headers);
  if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n";
  req += body;
  if (!(*l)->write_all({reinterpret_cast<const std::uint8_t*>(req.data()), req.size()})) return std::nullopt;
  std::string raw;
  const auto until = clock::now() + std::chrono::milliseconds(timeout_ms);
  std::uint8_t buf[4096];
  while (raw.size() < kMaxHttp) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now()).count();
    if (left <= 0) break;
    auto n = (*l)->read_some(buf, static_cast<int>(left));
    if (!n || *n == 0) break;
    raw.append(reinterpret_cast<const char*>(buf), *n);
  }
  (*l)->shutdown();
  return parse_http(raw);
}

std::string soap_body(const std::string& service, std::string_view action,
                      const std::vector<std::pair<std::string, std::string>>& args) {
  std::string b = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                  "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
  b += std::string(action) + " xmlns:u=\"" + service + "\">";
  for (const auto& [k, v] : args) b += "<" + k + ">" + v + "</" + k + ">";
  b += "</u:" + std::string(action) + "></s:Body></s:Envelope>";
  return b;
}

std::optional<http_reply> soap(const net::address& gateway, const std::string& control, const std::string& service,
                               std::string_view action, const std::vector<std::pair<std::string, std::string>>& args,
                               int timeout_ms) {
  // Checked again where it goes into a header (#499: a CR/LF in a service
  // type was a second request).
  const auto u = parse_url(control, &gateway);
  if (!u || !service_type(service)) return std::nullopt;
  const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" +
                              std::string(action) + "\"\r\n";
  return http(*u, "POST", headers, soap_body(service, action, args), timeout_ms);
}

}  // namespace

const char* method_name(method m) noexcept {
  switch (m) {
    case method::pcp: return "PCP";
    case method::nat_pmp: return "NAT-PMP";
    case method::upnp: return "UPnP";
    case method::none: break;
  }
  return "none";
}

// ---- parsers ------------------------------------------------------------------------------

std::optional<pcp_answer> parse_pcp_map(std::span<const std::uint8_t> r, const std::array<std::uint8_t, 12>& nonce) {
  if (r.size() < 60 || r[0] != kPcpVersion || r[1] != (0x80 | kPcpMap)) return std::nullopt;
  if (std::memcmp(r.data() + 24, nonce.data(), 12) != 0 || r[36] != kTcp) return std::nullopt;
  pcp_answer a;
  a.result = r[3];
  a.lifetime = get32(r.data() + 4);
  a.external_port = get16(r.data() + 42);
  a.external = from_mapped(r.data() + 44);
  return a;
}

std::optional<nat_pmp_answer> parse_nat_pmp_map(std::span<const std::uint8_t> r) {
  if (r.size() < 16 || r[0] != 0 || r[1] != 128 + 2) return std::nullopt;
  nat_pmp_answer a;
  a.result = get16(r.data() + 2);
  a.external_port = get16(r.data() + 10);
  a.lifetime = get32(r.data() + 12);
  return a;
}

std::optional<net::address> parse_nat_pmp_address(std::span<const std::uint8_t> r) {
  if (r.size() < 12 || r[0] != 0 || r[1] != 128 || get16(r.data() + 2) != 0) return std::nullopt;
  net::address a;
  std::memcpy(a.bytes.data(), r.data() + 8, 4);
  return a;
}

std::optional<http_reply> parse_http(std::string_view raw) {
  if (raw.size() > kMaxHttp) return std::nullopt;
  const auto head_end = raw.find("\r\n\r\n");
  if (head_end == std::string_view::npos || raw.substr(0, 9).size() < 9) return std::nullopt;
  const std::string_view status_line = raw.substr(0, raw.find("\r\n"));
  if (status_line.substr(0, 5) != "HTTP/") return std::nullopt;
  const auto sp = status_line.find(' ');
  if (sp == std::string_view::npos || sp + 4 > status_line.size()) return std::nullopt;
  http_reply out;
  for (std::size_t i = sp + 1; i < sp + 4; ++i) {
    if (status_line[i] < '0' || status_line[i] > '9') return std::nullopt;
    out.status = out.status * 10 + (status_line[i] - '0');
  }
  const std::string head = lower(raw.substr(0, head_end));
  std::string_view body = raw.substr(head_end + 4);
  if (head.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
    // Chunk sizes in hex, each line; a zero chunk ends it.
    std::string decoded;
    for (;;) {
      const auto eol = body.find("\r\n");
      if (eol == std::string_view::npos || eol == 0 || eol > 8) return std::nullopt;
      std::size_t n = 0;
      for (std::size_t i = 0; i < eol; ++i) {
        const char c = body[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
          if (c == ';') break;  // a chunk extension
          return std::nullopt;
        }
        n = n * 16 + static_cast<std::size_t>(d);
      }
      body.remove_prefix(eol + 2);
      if (n == 0) break;
      if (n > body.size()) return std::nullopt;
      decoded.append(body.substr(0, n));
      body.remove_prefix(std::min(body.size(), n + 2));
    }
    out.body = std::move(decoded);
  } else {
    out.body = std::string(body);
  }
  return out;
}

std::optional<std::string> xml_value(std::string_view xml, std::string_view tag) {
  // <tag>, <p:tag>, or with attributes; the first one.
  for (std::size_t at = 0; at < xml.size();) {
    const auto open = xml.find('<', at);
    if (open == std::string_view::npos) return std::nullopt;
    const auto close = xml.find('>', open);
    if (close == std::string_view::npos) return std::nullopt;
    std::string_view name = xml.substr(open + 1, close - open - 1);
    at = close + 1;
    if (name.empty() || name[0] == '/' || name[0] == '?' || name[0] == '!') continue;
    if (const auto space = name.find_first_of(" \t\r\n/"); space != std::string_view::npos) name = name.substr(0, space);
    if (const auto colon = name.find(':'); colon != std::string_view::npos) name = name.substr(colon + 1);
    if (name != tag) continue;
    if (xml[close - 1] == '/') return std::string();  // <tag/>
    const auto end = xml.find("</", at);
    if (end == std::string_view::npos) return std::nullopt;
    std::string v(xml.substr(at, end - at));
    while (!v.empty() && (v.front() == ' ' || v.front() == '\n' || v.front() == '\r' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
    return v;
  }
  return std::nullopt;
}

std::optional<std::string> parse_ssdp_location(std::string_view reply, const net::address& gateway) {
  if (reply.size() > 4096 || reply.substr(0, 5) != "HTTP/") return std::nullopt;
  for (std::size_t at = 0; at < reply.size();) {
    const auto eol = reply.find("\r\n", at);
    const std::string_view line = reply.substr(at, eol == std::string_view::npos ? std::string_view::npos : eol - at);
    at = eol == std::string_view::npos ? reply.size() : eol + 2;
    if (lower(line.substr(0, 9)) != "location:") continue;
    std::string_view v = line.substr(9);
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
    if (!parse_url(v, &gateway)) return std::nullopt;
    return std::string(v);
  }
  return std::nullopt;
}

std::optional<described> parse_description(std::string_view xml, std::string_view location,
                                           const net::address& gateway) {
  const auto loc = parse_url(location, &gateway);
  if (!loc) return std::nullopt;
  std::string base = "http://" + loc->host.address + ":" + std::to_string(loc->host.port);
  if (const auto b = xml_value(xml, "URLBase"); b && parse_url(*b, &gateway)) {
    base = *b;
    while (!base.empty() && base.back() == '/') base.pop_back();
  }
  auto resolve = [&](const std::string& c) -> std::string {
    if (lower(c.substr(0, 7)) == "http://") return parse_url(c, &gateway) ? c : std::string();
    const std::string full = base + (c.empty() || c[0] != '/' ? "/" : "") + c;
    return parse_url(full, &gateway) ? full : std::string();  // a space or control in the path: not a request line
  };
  described out;
  for (std::size_t at = 0;;) {
    const auto s = xml.find("<service>", at);
    if (s == std::string_view::npos) break;
    const auto e = xml.find("</service>", s);
    if (e == std::string_view::npos) break;
    const std::string_view block = xml.substr(s, e - s);
    at = e + 10;
    const auto type = xml_value(block, "serviceType");
    const auto control = xml_value(block, "controlURL");
    if (!type || !control || !service_type(*type)) continue;
    const std::string full = resolve(*control);
    if (full.empty()) continue;
    const bool wan = type->find(":service:WANIPConnection:") != std::string::npos ||
                     type->find(":service:WANPPPConnection:") != std::string::npos;
    // IGDv2's WANIPConnection:2 over :1, the first otherwise.
    if (wan && (out.service.empty() || type->find("WANIPConnection:2") != std::string::npos)) {
      out.service = *type;
      out.control = full;
    } else if (type->find(":service:WANIPv6FirewallControl:") != std::string::npos && out.fw_service.empty()) {
      out.fw_service = *type;
      out.fw_control = full;
    }
  }
  if (out.service.empty()) return std::nullopt;
  return out;
}

// ---- the mapper -------------------------------------------------------------------------------

mapper::mapper(options o) : opts_(std::move(o)) {
  (void)sodium_init();
  gateway_ = opts_.gateway ? opts_.gateway : net::default_gateway();
  if (gateway_) me_ = net::local_address_toward(*gateway_);
}

std::optional<mapped> mapper::map_tcp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s) {
  if (!gateway_ || !me_) {
    notes_.emplace_back("no router found to ask for a port");
    return std::nullopt;
  }
  if (auto m = try_pcp(internal_port, external_port, lifetime_s)) return m;
  if (cancelled()) return std::nullopt;
  if (auto m = try_nat_pmp(internal_port, external_port, lifetime_s)) return m;
  if (cancelled()) return std::nullopt;
  return try_upnp(internal_port, external_port, lifetime_s);
}

std::optional<mapped> mapper::try_pcp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s) {
  mapped m;
  m.how = method::pcp;
  m.internal_port = internal_port;
  randombytes_buf(m.pcp_nonce.data(), m.pcp_nonce.size());
  std::vector<std::uint8_t> req = {kPcpVersion, kPcpMap, 0, 0};
  put32(req, static_cast<std::uint32_t>(lifetime_s));
  put_mapped(req, *me_);
  req.insert(req.end(), m.pcp_nonce.begin(), m.pcp_nonce.end());
  req.push_back(kTcp);
  req.insert(req.end(), 3, 0);
  put16(req, internal_port);
  put16(req, external_port);
  put_mapped(req, net::address{});  // no external address preferred
  std::optional<pcp_answer> got;
  const net::endpoint gw{net::address_text(*gateway_), opts_.gateway_port};
  (void)ask_gateway(gw, req, opts_.timeout_ms, opts_.cancelled, [&](std::span<const std::uint8_t> r) {
    // A NAT-PMP-only router answers a PCP request with version 0: not PCP.
    if (r.size() >= 2 && r[0] == 0) return true;
    got = parse_pcp_map(r, m.pcp_nonce);
    return got.has_value();
  });
  if (!got) {
    notes_.emplace_back("PCP: no answer");
    return std::nullopt;
  }
  if (got->result != 0 || got->external_port == 0) {
    notes_.emplace_back("PCP: the router refused (result " + std::to_string(got->result) + ")");
    return std::nullopt;
  }
  if (!net::is_public(got->external)) {
    behind_nat_ = true;
    notes_.emplace_back("PCP: the router's outside address is not public (another NAT beyond it)");
    remove(m);
    return std::nullopt;
  }
  m.external = {net::address_text(got->external), got->external_port};
  notes_.emplace_back("PCP: mapped");
  return m;
}

std::optional<mapped> mapper::try_nat_pmp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s) {
  const net::endpoint gw{net::address_text(*gateway_), opts_.gateway_port};
  std::optional<net::address> outside;
  const std::uint8_t ask_address[2] = {0, 0};
  (void)ask_gateway(gw, ask_address, opts_.timeout_ms, opts_.cancelled, [&](std::span<const std::uint8_t> r) {
    outside = parse_nat_pmp_address(r);
    return outside.has_value();
  });
  if (!outside) {
    notes_.emplace_back("NAT-PMP: no answer");
    return std::nullopt;
  }
  std::vector<std::uint8_t> req = {0, 2, 0, 0};
  put16(req, internal_port);
  put16(req, external_port);
  put32(req, static_cast<std::uint32_t>(lifetime_s));
  std::optional<nat_pmp_answer> got;
  (void)ask_gateway(gw, req, opts_.timeout_ms, opts_.cancelled, [&](std::span<const std::uint8_t> r) {
    got = parse_nat_pmp_map(r);
    return got.has_value();
  });
  if (!got || got->result != 0 || got->external_port == 0) {
    notes_.emplace_back("NAT-PMP: the router refused");
    return std::nullopt;
  }
  mapped m;
  m.how = method::nat_pmp;
  m.internal_port = internal_port;
  if (!net::is_public(*outside)) {
    behind_nat_ = true;
    notes_.emplace_back("NAT-PMP: the router's outside address is not public (another NAT beyond it)");
    m.external = {net::address_text(*outside), got->external_port};
    remove(m);
    return std::nullopt;
  }
  m.external = {net::address_text(*outside), got->external_port};
  notes_.emplace_back("NAT-PMP: mapped");
  return m;
}

std::optional<mapper::upnp_device> mapper::find_upnp() {
  if (upnp_searched_) return upnp_;
  upnp_searched_ = true;
  auto sock = net::open_udp(false, 2);
  if (!sock) return std::nullopt;
  const auto until = clock::now() + std::chrono::milliseconds(opts_.timeout_ms);
  for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                         "urn:schemas-upnp-org:device:InternetGatewayDevice:1"}) {
    const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " +
                               std::string(st) + "\r\n\r\n";
    (void)(*sock)->send_to({reinterpret_cast<const std::uint8_t*>(search.data()), search.size()}, opts_.ssdp);
  }
  std::uint8_t buf[2048];
  while (clock::now() < until && !cancelled()) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now()).count();
    net::endpoint from;
    auto n = (*sock)->receive_from(buf, static_cast<int>(std::max<long long>(1, left)), from);
    if (!n) continue;
    // Only the gateway's own answer (#499: any device on the network could
    // answer first and point this machine anywhere).
    if (net::parse_address(from.address) != gateway_) continue;
    const auto location = parse_ssdp_location({reinterpret_cast<const char*>(buf), *n}, *gateway_);
    if (!location) continue;
    const auto u = parse_url(*location, &*gateway_);
    if (!u) continue;
    auto desc = http(*u, "GET", "", "", opts_.timeout_ms);
    if (!desc || desc->status != 200) continue;
    const auto d = parse_description(desc->body, *location, *gateway_);
    if (!d) continue;
    upnp_ = upnp_device{d->control, d->service, d->fw_control, d->fw_service};
    return upnp_;
  }
  return std::nullopt;
}

std::optional<mapped> mapper::try_upnp(std::uint16_t internal_port, std::uint16_t external_port, int lifetime_s) {
  const auto dev = find_upnp();
  if (!dev) {
    notes_.emplace_back("UPnP: no gateway answered");
    return std::nullopt;
  }
  auto ip = soap(*gateway_, dev->control, dev->service, "GetExternalIPAddress", {}, opts_.timeout_ms);
  const auto outside_text = ip && ip->status == 200 ? xml_value(ip->body, "NewExternalIPAddress") : std::nullopt;
  const auto outside = outside_text ? net::parse_address(*outside_text) : std::nullopt;
  if (!outside) {
    notes_.emplace_back("UPnP: the router did not say its outside address");
    return std::nullopt;
  }
  if (!net::is_public(*outside)) {
    behind_nat_ = true;
    notes_.emplace_back("UPnP: the router's outside address is not public (another NAT beyond it)");
    return std::nullopt;
  }
  const std::uint16_t want = external_port ? external_port : internal_port;
  // A port already forwarded, unless to this machine by an earlier run of
  // this app (a crash), is someone's (#499: AddPortMapping would overwrite
  // the person's own forward, and remove() then delete it).
  bool taken = false;
  if (auto e = soap(*gateway_, dev->control, dev->service, "GetSpecificPortMappingEntry",
                    {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(want)}, {"NewProtocol", "TCP"}},
                    opts_.timeout_ms);
      e && e->status == 200) {
    taken = xml_value(e->body, "NewInternalClient") != net::address_text(*me_) ||
            xml_value(e->body, "NewPortMappingDescription") != kDescription;
  }
  auto r = taken ? std::nullopt : soap(*gateway_, dev->control, dev->service, "AddPortMapping",
                {{"NewRemoteHost", ""},
                 {"NewExternalPort", std::to_string(want)},
                 {"NewProtocol", "TCP"},
                 {"NewInternalPort", std::to_string(internal_port)},
                 {"NewInternalClient", net::address_text(*me_)},
                 {"NewEnabled", "1"},
                 {"NewPortMappingDescription", kDescription},
                 {"NewLeaseDuration", std::to_string(lifetime_s)}},
                opts_.timeout_ms);
  std::uint16_t got = r && r->status == 200 ? want : 0;
  if (!got && dev->service.find("WANIPConnection:2") != std::string::npos) {
    // IGDv2: the router picks a free port.
    auto any = soap(*gateway_, dev->control, dev->service, "AddAnyPortMapping",
                    {{"NewRemoteHost", ""},
                     {"NewExternalPort", std::to_string(want)},
                     {"NewProtocol", "TCP"},
                     {"NewInternalPort", std::to_string(internal_port)},
                     {"NewInternalClient", net::address_text(*me_)},
                     {"NewEnabled", "1"},
                     {"NewPortMappingDescription", kDescription},
                     {"NewLeaseDuration", std::to_string(lifetime_s)}},
                    opts_.timeout_ms);
    if (any && any->status == 200) {
      if (const auto p = xml_value(any->body, "NewReservedPort")) {
        const unsigned long v = std::strtoul(p->c_str(), nullptr, 10);
        if (v > 0 && v <= 65535) got = static_cast<std::uint16_t>(v);
      }
    }
  }
  if (!got) {
    notes_.emplace_back("UPnP: the router refused the mapping");
    return std::nullopt;
  }
  mapped m;
  m.how = method::upnp;
  m.internal_port = internal_port;
  m.external = {net::address_text(*outside), got};
  m.upnp_control = dev->control;
  m.upnp_service = dev->service;
  notes_.emplace_back("UPnP: mapped");
  return m;
}

std::optional<mapped> mapper::pinhole_v6(const net::address& local, std::uint16_t port, int lifetime_s) {
  if (!local.v6 || !net::is_public(local)) return std::nullopt;
  const auto dev = find_upnp();
  if (!dev || dev->fw_service.empty()) {
    notes_.emplace_back("IPv6: no router firewall to open (IGDv2)");
    return std::nullopt;
  }
  auto r = soap(*gateway_, dev->fw_control, dev->fw_service, "AddPinhole",
                {{"RemoteHost", ""},
                 {"RemotePort", "0"},
                 {"InternalClient", net::address_text(local)},
                 {"InternalPort", std::to_string(port)},
                 {"Protocol", "6"},
                 {"LeaseTime", std::to_string(lifetime_s)}},
                opts_.timeout_ms);
  const auto id = r && r->status == 200 ? xml_value(r->body, "UniqueID") : std::nullopt;
  // A number (ui2), as it goes back into the delete's body.
  if (!id || id->empty() || id->size() > 5 ||
      !std::all_of(id->begin(), id->end(), [](char c) { return c >= '0' && c <= '9'; })) {
    notes_.emplace_back("IPv6: the router refused a pinhole");
    return std::nullopt;
  }
  mapped m;
  m.how = method::upnp;
  m.pinhole = true;
  m.internal_port = port;
  m.external = {net::address_text(local), port};
  m.upnp_control = dev->fw_control;
  m.upnp_service = dev->fw_service;
  m.pinhole_id = *id;
  notes_.emplace_back("IPv6: pinhole opened");
  return m;
}

bool mapper::renew(const mapped& m, int lifetime_s) {
  if (!gateway_ || !me_) return false;
  const net::endpoint gw{net::address_text(*gateway_), opts_.gateway_port};
  switch (m.how) {
    case method::pcp: {
      // The same nonce and ports: the router extends the mapping it has.
      std::vector<std::uint8_t> req = {kPcpVersion, kPcpMap, 0, 0};
      put32(req, static_cast<std::uint32_t>(lifetime_s));
      put_mapped(req, *me_);
      req.insert(req.end(), m.pcp_nonce.begin(), m.pcp_nonce.end());
      req.push_back(kTcp);
      req.insert(req.end(), 3, 0);
      put16(req, m.internal_port);
      put16(req, m.external.port);
      put_mapped(req, net::address{});
      std::optional<pcp_answer> got;
      (void)ask_gateway(gw, req, opts_.timeout_ms, opts_.cancelled, [&](std::span<const std::uint8_t> r) {
        got = parse_pcp_map(r, m.pcp_nonce);
        return got.has_value();
      });
      return got && got->result == 0 && got->external_port == m.external.port;
    }
    case method::nat_pmp: {
      std::vector<std::uint8_t> req = {0, 2, 0, 0};
      put16(req, m.internal_port);
      put16(req, m.external.port);
      put32(req, static_cast<std::uint32_t>(lifetime_s));
      std::optional<nat_pmp_answer> got;
      (void)ask_gateway(gw, req, opts_.timeout_ms, opts_.cancelled, [&](std::span<const std::uint8_t> r) {
        got = parse_nat_pmp_map(r);
        return got.has_value();
      });
      return got && got->result == 0 && got->external_port == m.external.port;
    }
    case method::upnp: {
      std::optional<http_reply> r;
      if (m.pinhole) {
        r = soap(*gateway_, m.upnp_control, m.upnp_service, "UpdatePinhole",
                 {{"UniqueID", m.pinhole_id}, {"NewLeaseTime", std::to_string(lifetime_s)}}, opts_.timeout_ms);
      } else {
        // AddPortMapping again for the same client and port extends it.
        r = soap(*gateway_, m.upnp_control, m.upnp_service, "AddPortMapping",
                 {{"NewRemoteHost", ""},
                  {"NewExternalPort", std::to_string(m.external.port)},
                  {"NewProtocol", "TCP"},
                  {"NewInternalPort", std::to_string(m.internal_port)},
                  {"NewInternalClient", net::address_text(*me_)},
                  {"NewEnabled", "1"},
                  {"NewPortMappingDescription", kDescription},
                  {"NewLeaseDuration", std::to_string(lifetime_s)}},
                 opts_.timeout_ms);
      }
      return r && r->status == 200;
    }
    case method::none:
      break;
  }
  return false;
}

void mapper::remove(const mapped& m) {
  if (!gateway_) return;
  const net::endpoint gw{net::address_text(*gateway_), opts_.gateway_port};
  const int quick = std::min(opts_.timeout_ms, 1000);
  switch (m.how) {
    case method::pcp: {
      if (!me_) return;
      std::vector<std::uint8_t> req = {kPcpVersion, kPcpMap, 0, 0};
      put32(req, 0);  // lifetime 0: delete
      put_mapped(req, *me_);
      req.insert(req.end(), m.pcp_nonce.begin(), m.pcp_nonce.end());
      req.push_back(kTcp);
      req.insert(req.end(), 3, 0);
      put16(req, m.internal_port);
      put16(req, 0);
      put_mapped(req, net::address{});
      (void)ask_gateway(gw, req, quick, nullptr,
                        [&](std::span<const std::uint8_t> r) { return parse_pcp_map(r, m.pcp_nonce).has_value(); });
      break;
    }
    case method::nat_pmp: {
      std::vector<std::uint8_t> req = {0, 2, 0, 0};
      put16(req, m.internal_port);
      put16(req, 0);
      put32(req, 0);
      (void)ask_gateway(gw, req, quick, nullptr,
                        [&](std::span<const std::uint8_t> r) { return parse_nat_pmp_map(r).has_value(); });
      break;
    }
    case method::upnp:
      if (m.pinhole) {
        (void)soap(*gateway_, m.upnp_control, m.upnp_service, "DeletePinhole", {{"UniqueID", m.pinhole_id}}, quick);
      } else {
        (void)soap(*gateway_, m.upnp_control, m.upnp_service, "DeletePortMapping",
                   {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(m.external.port)}, {"NewProtocol", "TCP"}},
                   quick);
      }
      break;
    case method::none:
      break;
  }
}

// ---- the keeper -------------------------------------------------------------------------------

keeper::keeper(std::unique_ptr<mapper> m, std::vector<mapped> held, int lifetime_s)
    : mapper_(std::move(m)), held_(std::move(held)), lifetime_s_(std::max(1, lifetime_s)) {
  if (mapper_ && !held_.empty()) thread_ = std::thread([this] { run(); });
}

keeper::~keeper() {
  {
    std::lock_guard lock(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (mapper_) {
    for (const auto& m : held_) mapper_->remove(m);
  }
}

void keeper::run() {
  // At half the lease; one the router did not take is asked again soon (it
  // may have been busy), while the rest of its lease runs.
  const auto half = std::chrono::milliseconds(lifetime_s_ * 500);
  const auto soon = std::min(half, std::chrono::milliseconds(10000));
  std::vector<clock::time_point> due(held_.size(), clock::now() + half);
  std::unique_lock lock(m_);
  while (!stop_) {
    const auto next = *std::min_element(due.begin(), due.end());
    if (cv_.wait_until(lock, next, [&] { return stop_; })) return;
    const auto now = clock::now();
    for (std::size_t i = 0; i < held_.size(); ++i) {
      if (due[i] > now) continue;
      lock.unlock();
      const bool ok = mapper_->renew(held_[i], lifetime_s_);
      lock.lock();
      if (stop_) return;
      due[i] = clock::now() + (ok ? half : soon);
    }
  }
}

}  // namespace mv::driftwire::mapping
