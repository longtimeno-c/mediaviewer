// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/tunnel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>

namespace mv::driftwire::tunnel {

namespace {

using clock = std::chrono::steady_clock;

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

std::optional<std::uint16_t> port_of(std::string_view digits) {
  if (digits.empty() || digits.size() > 5) return std::nullopt;
  unsigned v = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  if (v == 0 || v > 65535) return std::nullopt;
  return static_cast<std::uint16_t>(v);
}

// A link two owners can shut down: the proxy's destructor, and the stream.
struct shared_link final : link {
  std::shared_ptr<link> l;
  explicit shared_link(std::shared_ptr<link> x) : l(std::move(x)) {}
  expected write_all(std::span<const std::uint8_t> b) override { return l->write_all(b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override { return l->read_some(into, ms); }
  void shutdown() noexcept override { l->shutdown(); }
  transport via() const noexcept override { return l->via(); }
};

expected write_text(link& l, std::string_view s) {
  return l.write_all({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

}  // namespace

// ---- parsing -------------------------------------------------------------------------------

std::optional<target> parse_authority(std::string_view text, std::uint16_t default_port) {
  target t;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    t.host = std::string(text.substr(1, close - 1));
    const auto rest = text.substr(close + 1);
    if (rest.empty()) {
      t.port = default_port;
    } else {
      if (rest.front() != ':') return std::nullopt;
      const auto p = port_of(rest.substr(1));
      if (!p) return std::nullopt;
      t.port = *p;
    }
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      t.host = std::string(text);
      t.port = default_port;
    } else if (text.find(':') != colon) {
      t.host = std::string(text);  // a bare IPv6 address
      t.port = default_port;
    } else {
      const auto p = port_of(text.substr(colon + 1));
      if (!p) return std::nullopt;
      t.host = std::string(text.substr(0, colon));
      t.port = *p;
    }
  }
  if (t.port == 0 || !relay::forward_host_ok(t.host)) return std::nullopt;
  return t;
}

std::optional<http_ask> parse_http_head(std::string_view head) {
  // Lines, "\r\n" or a bare "\n".
  std::vector<std::string_view> lines;
  std::size_t at = 0;
  while (at < head.size()) {
    auto nl = head.find('\n', at);
    if (nl == std::string_view::npos) nl = head.size();
    auto line = head.substr(at, nl - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    at = nl + 1;
    if (line.empty()) break;  // the blank line ends the head
    lines.push_back(line);
  }
  if (lines.empty()) return std::nullopt;
  const auto first = lines.front();
  const auto sp1 = first.find(' ');
  const auto sp2 = first.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == sp1) return std::nullopt;
  const auto method = first.substr(0, sp1);
  const auto uri = first.substr(sp1 + 1, sp2 - sp1 - 1);
  const auto version = first.substr(sp2 + 1);
  if (version.substr(0, 5) != "HTTP/" || method.empty() || uri.empty()) return std::nullopt;
  for (const char c : method) {
    if (!std::isupper(static_cast<unsigned char>(c))) return std::nullopt;
  }
  http_ask a;
  if (method == "CONNECT") {
    const auto t = parse_authority(uri, 0);
    if (!t) return std::nullopt;
    a.connect = true;
    a.to = *t;
    return a;
  }
  // A plain request in absolute form: http:// only (https goes by CONNECT).
  constexpr std::string_view kScheme = "http://";
  if (uri.size() <= kScheme.size() || !iequals(uri.substr(0, kScheme.size()), kScheme)) return std::nullopt;
  const auto rest = uri.substr(kScheme.size());
  const auto end = std::min(rest.find('/'), rest.find('?'));
  auto authority = rest.substr(0, std::min(end, rest.size()));
  if (const auto at_sign = authority.rfind('@'); at_sign != std::string_view::npos) {
    authority = authority.substr(at_sign + 1);  // credentials in the URL are not the host
  }
  const auto t = parse_authority(authority, 80);
  if (!t) return std::nullopt;
  a.to = *t;
  std::string path(end == std::string_view::npos ? std::string_view("/") : rest.substr(end));
  if (path.front() == '?') path.insert(path.begin(), '/');
  a.head.append(method).append(" ").append(path).append(" ").append(version).append("\r\n");
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const auto colon = lines[i].find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto name = lines[i].substr(0, colon);
    if (iequals(name, "Proxy-Connection") || iequals(name, "Proxy-Authorization") || iequals(name, "Connection") ||
        iequals(name, "Keep-Alive")) {
      continue;
    }
    a.head.append(lines[i]).append("\r\n");
  }
  a.head.append("Connection: close\r\n\r\n");
  return a;
}

std::optional<socks_ask> parse_socks_request(std::span<const std::uint8_t> b, std::size_t& needed) {
  needed = 4;
  if (b.size() < 4) return std::nullopt;
  if (b[0] != 5) {
    needed = 0;  // not SOCKS5: never whole
    return std::nullopt;
  }
  socks_ask a;
  if (b[1] != 1) {
    a.reply = 7;  // command not supported (BIND, UDP ASSOCIATE)
    return a;
  }
  switch (b[3]) {
    case 1: {  // IPv4
      needed = 4 + 4 + 2;
      if (b.size() < needed) return std::nullopt;
      net::address ip;
      std::copy_n(b.begin() + 4, 4, ip.bytes.begin());
      a.to.host = net::address_text(ip);
      a.to.port = static_cast<std::uint16_t>(b[8] << 8 | b[9]);
      break;
    }
    case 3: {  // a name
      needed = 5;
      if (b.size() < needed) return std::nullopt;
      needed = 5 + std::size_t{b[4]} + 2;
      if (b.size() < needed) return std::nullopt;
      a.to.host.assign(reinterpret_cast<const char*>(b.data()) + 5, b[4]);
      a.to.port = static_cast<std::uint16_t>(b[5 + b[4]] << 8 | b[6 + b[4]]);
      break;
    }
    case 4: {  // IPv6
      needed = 4 + 16 + 2;
      if (b.size() < needed) return std::nullopt;
      net::address ip;
      ip.v6 = true;
      std::copy_n(b.begin() + 4, 16, ip.bytes.begin());
      a.to.host = net::address_text(ip);
      a.to.port = static_cast<std::uint16_t>(b[20] << 8 | b[21]);
      break;
    }
    default:
      a.reply = 8;  // address type not supported
      return a;
  }
  if (a.to.port == 0 || !relay::forward_host_ok(a.to.host)) a.reply = 4;  // host unreachable
  return a;
}

std::uint8_t socks_reply_for(relay::forward_answer a) noexcept {
  switch (a) {
    case relay::forward_answer::ok: return 0;
    case relay::forward_answer::port:
    case relay::forward_answer::not_public: return 2;  // not allowed by ruleset
    case relay::forward_answer::unreachable: return 4;  // host unreachable
    default: return 1;                                  // general failure
  }
}

std::string http_reply_for(relay::forward_answer a) {
  const char* line = "HTTP/1.1 502 Bad Gateway";
  switch (a) {
    case relay::forward_answer::ok: return "HTTP/1.1 200 Connection established\r\n\r\n";
    case relay::forward_answer::port:
    case relay::forward_answer::not_public: line = "HTTP/1.1 403 Forbidden"; break;
    case relay::forward_answer::busy: line = "HTTP/1.1 503 Service Unavailable"; break;
    default: break;
  }
  return std::string(line) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

std::string pac_url(std::uint16_t port) {
  return "http://127.0.0.1:" + std::to_string(port) + std::string(kPacPath);
}

std::string pac_script(std::uint16_t port) {
  const std::string at = "127.0.0.1:" + std::to_string(port);
  // No dnsResolve(): it would ask the network's DNS, the thing a filter
  // watches. Literal private addresses and local names only.
  return "function FindProxyForURL(url, host) {\n"
         "  if (isPlainHostName(host) || host == \"localhost\" || dnsDomainIs(host, \".local\") ||\n"
         "      /^(127\\.|10\\.|192\\.168\\.|169\\.254\\.|172\\.(1[6-9]|2[0-9]|3[01])\\.)/.test(host) ||\n"
         "      /^(::1$|f[cd][0-9a-f]{2}:|fe80:)/i.test(host))\n"
         "    return \"DIRECT\";\n"
         "  return \"SOCKS5 " + at + "; DIRECT\";\n"
         "}\n";
}

bool asks_for_pac(std::string_view head) {
  const auto end = head.find_first_of("\r\n");
  const auto line = head.substr(0, end);
  const std::string want = "GET " + std::string(kPacPath);
  return line.rfind(want, 0) == 0 && line.size() > want.size() &&
         (line[want.size()] == ' ' || line[want.size()] == '?');
}

std::string relay_shown(const std::string& relay_field) {
  if (relay_field.empty()) return {};
  std::string why;
  const auto f = relay_server_of(relay_field, why);
  return f ? f->at : std::string();
}

// ---- the proxy -----------------------------------------------------------------------------

result<std::unique_ptr<proxy>> proxy::start(options o) {
  std::string why;
  const auto server = relay_server_of(o.relay, why);
  if (!server) return err(status::invalid_arg);
  result<std::unique_ptr<net::listener>> l = err(status::io);
  // 127.0.0.1: what HTTPS_PROXY=http://127.0.0.1:<port> and every
  // browser's proxy setting reach.
  if (o.port != 0) l = net::listen(true, o.port);
  if (!l) l = net::listen(true);
  if (!l) return err(l.error());
  std::unique_ptr<proxy> p(new proxy(std::move(o)));
  p->server_ = *server;
  if (p->opts_.tls) {
    if (!tls::available()) return err(status::unsupported_format);
    const auto k = tls::derive_knock_key(server->key);  // Argon2id: once a start
    if (!k) return err(status::out_of_memory);
    p->knock_ = *k;
    std::string host = server->at.substr(0, server->at.rfind(':'));
    if (host.size() > 1 && host.front() == '[') host = host.substr(1, host.size() - 2);
    if (!net::parse_address(host)) p->sni_ = host;  // a name, as a browser sends it
  }
  p->listener_ = std::move(*l);
  p->snap_.port = p->listener_->port();
  proxy* raw = p.get();
  p->accept_thread_ = std::thread([raw] { raw->accept_loop(); });
  return p;
}

proxy::~proxy() {
  stop_.store(true);
  if (listener_) listener_->shutdown();
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<worker> all;
  {
    std::lock_guard lock(m_);
    all = std::move(workers_);
    for (auto& w : all) {
      w.l->shutdown();
      if (w.remote && *w.remote) (*w.remote)->shutdown();
    }
    turn_cv_.notify_all();
  }
  for (auto& w : all) w.t.join();
}

std::uint16_t proxy::port() const noexcept { return listener_ ? listener_->port() : 0; }

proxy::snapshot proxy::state() const {
  std::lock_guard lock(m_);
  return snap_;
}

void proxy::accept_loop() {
  while (!stop_.load()) {
    auto c = listener_->accept(500);
    if (!c) {
      if (c.error() == status::cancelled) return;
      if (c.error() != status::timeout) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    std::shared_ptr<link> l(std::move(*c));
    std::lock_guard lock(m_);
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->done->load()) {
        it->t.join();
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
    if (static_cast<int>(workers_.size()) >= kMaxStreams) {
      l->shutdown();
      continue;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto remote = std::make_shared<std::shared_ptr<link>>();
    try {
      workers_.push_back({std::thread([this, l, done, remote] {
                            try {
                              serve(l, remote);
                            } catch (...) {
                            }
                            l->shutdown();
                            done->store(true);
                          }),
                          done, l, remote});
    } catch (...) {
      l->shutdown();
    }
  }
}

void proxy::note(relay::forward_answer a, bool opened) {
  {
    std::lock_guard lock(m_);
    if (opened) {
      ++snap_.streams;
      snap_.problem.clear();
    } else {
      ++snap_.failed;
      switch (a) {
        case relay::forward_answer::key:
          snap_.problem = "Your relay server did not take its key: check what follows # in Your own relay server.";
          break;
        case relay::forward_answer::off:
          snap_.problem = "Your relay server does not forward: update it (driftwire-relay), and do not run it with "
                          "--forward-ports none.";
          break;
        case relay::forward_answer::failed:
          snap_.problem = "Your relay server did not answer.";
          break;
        default:
          break;  // the site's own: not the tunnel's problem
      }
    }
  }
  if (opts_.changed) opts_.changed();
}

result<std::unique_ptr<link>> proxy::open(const target& to, relay::forward_answer& said,
                                          const std::shared_ptr<std::shared_ptr<link>>& remote) {
  said = relay::forward_answer::failed;
  {
    std::unique_lock lock(m_);
    turn_cv_.wait(lock, [&] { return stop_.load() || opening_ < kOpensAtOnce; });
    if (stop_.load()) return err(status::cancelled);
    ++opening_;
  }
  struct turn {
    proxy& p;
    ~turn() {
      std::lock_guard lock(p.m_);
      --p.opening_;
      p.turn_cv_.notify_one();
    }
  } given{*this};
  std::optional<net::endpoint> at;
  {
    std::lock_guard lock(m_);
    at = resolved_;
  }
  if (!at) {
    at = relay_endpoint(server_.at);  // a name: resolved here, once
    if (!at) return err(status::io);
    std::lock_guard lock(m_);
    resolved_ = at;
  }
  net::endpoint to_server = *at;
  if (opts_.tls) to_server.port = opts_.tls_port;
  auto c = opts_.connect ? opts_.connect(to_server, kReachConnectMs) : net::connect(to_server, kReachConnectMs);
  if (!c) {
    std::lock_guard lock(m_);
    resolved_.reset();  // its address may have changed
    return err(c.error());
  }
  std::shared_ptr<link> raw(std::move(*c));
  {
    std::lock_guard lock(m_);
    *remote = raw;
    if (stop_.load()) {
      raw->shutdown();
      return err(status::cancelled);
    }
  }
  std::unique_ptr<link> hop = std::make_unique<shared_link>(raw);
  if (opts_.tls) {
    // Slice 4: TLS with Chrome's ClientHello, and the knock, first.
    auto t = tls::client(std::move(hop), sni_, &knock_);
    if (!t) {
      said = relay::forward_answer::failed;
      return err(t.error());
    }
    hop = std::move(*t);
  }
  return relay::forward(std::move(hop), server_.key, to.host, to.port, relay::kForwardWaitMs, said);
}

void proxy::serve(std::shared_ptr<link> local, const std::shared_ptr<std::shared_ptr<link>>& remote) {
  const auto deadline = clock::now() + std::chrono::milliseconds(kRequestMs);
  std::vector<std::uint8_t> buf;
  // Reads until `buf` holds at least `n` bytes.
  const auto fill = [&](std::size_t n) -> bool {
    std::array<std::uint8_t, 4096> chunk{};
    while (buf.size() < n) {
      if (stop_.load()) return false;
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()).count();
      if (left <= 0) return false;
      auto got = local->read_some(chunk, static_cast<int>(std::min<long long>(left, 250)));
      if (!got) {
        if (got.error() == status::timeout) continue;
        return false;
      }
      if (*got == 0) return false;
      buf.insert(buf.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*got));
    }
    return true;
  };
  if (!fill(1)) return;
  std::unique_ptr<link> stream;
  if (buf[0] == 5) {
    // SOCKS5: the methods, "no authentication" chosen, then the request.
    if (!fill(2) || !fill(2 + std::size_t{buf[1]})) return;
    const bool none = std::find(buf.begin() + 2, buf.begin() + 2 + buf[1], std::uint8_t{0}) != buf.begin() + 2 + buf[1];
    const std::uint8_t choice[2] = {5, static_cast<std::uint8_t>(none ? 0 : 0xFF)};
    if (!local->write_all(choice) || !none) return;
    buf.erase(buf.begin(), buf.begin() + 2 + buf[1]);
    std::optional<socks_ask> ask;
    for (;;) {
      std::size_t needed = 0;
      ask = parse_socks_request(buf, needed);
      if (ask || needed == 0 || needed <= buf.size() || !fill(needed)) break;
    }
    if (!ask) return;
    std::uint8_t reply[10] = {5, ask->reply, 0, 1, 0, 0, 0, 0, 0, 0};
    if (ask->reply != 0) {
      (void)local->write_all(reply);
      return;
    }
    relay::forward_answer said{};
    auto s = open(ask->to, said, remote);
    reply[1] = s ? 0 : socks_reply_for(said == relay::forward_answer::ok ? relay::forward_answer::failed : said);
    if (stop_.load()) return;
    note(said, static_cast<bool>(s));
    if (!local->write_all(reply) || !s) return;
    stream = std::move(*s);
  } else {
    // HTTP: the head, through its blank line.
    static constexpr std::string_view kEnd = "\r\n\r\n";
    std::size_t end = std::string_view::npos;
    while (end == std::string_view::npos) {
      const std::string_view seen(reinterpret_cast<const char*>(buf.data()), buf.size());
      end = seen.find(kEnd);
      if (end != std::string_view::npos) break;
      if (buf.size() >= kMaxHeadBytes || !fill(buf.size() + 1)) return;
    }
    const std::string_view head(reinterpret_cast<const char*>(buf.data()), end + kEnd.size());
    if (asks_for_pac(head)) {
      // The system proxy's script (sysproxy.h): served here, nothing forwarded.
      const auto script = pac_script(port());
      (void)write_text(*local, "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\n"
                               "Cache-Control: no-store\r\nContent-Length: " +
                                   std::to_string(script.size()) + "\r\nConnection: close\r\n\r\n" + script);
      return;
    }
    const auto ask = parse_http_head(head);
    if (!ask) {
      (void)write_text(*local, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    const std::vector<std::uint8_t> rest(buf.begin() + static_cast<std::ptrdiff_t>(head.size()), buf.end());
    relay::forward_answer said{};
    auto s = open(ask->to, said, remote);
    if (stop_.load()) return;
    note(said, static_cast<bool>(s));
    if (!s) {
      (void)write_text(*local, http_reply_for(said == relay::forward_answer::ok ? relay::forward_answer::failed : said));
      return;
    }
    stream = std::move(*s);
    if (ask->connect) {
      if (!write_text(*local, http_reply_for(relay::forward_answer::ok))) return;
    } else if (!write_text(*stream, ask->head)) {
      return;
    }
    if (!rest.empty() && !stream->write_all(rest)) return;
  }
  {
    std::lock_guard lock(m_);
    ++snap_.open;
  }
  if (opts_.changed) opts_.changed();
  relay::limits lim;
  lim.max_bytes = ~std::uint64_t{0} >> 1;
  lim.idle_ms = 10 * 60 * 1000;
  (void)relay::splice(*local, *stream, lim, [this] { return stop_.load(); });
  {
    std::lock_guard lock(m_);
    --snap_.open;
  }
  if (opts_.changed && !stop_.load()) opts_.changed();
}

}  // namespace mv::driftwire::tunnel
