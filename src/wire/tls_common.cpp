// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The TLS layer's parts that need no TLS library (tls.h): the knock, the web
// site a prober sees, and the JA4 of a ClientHello. Portable.
#include "wire/tls.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>


namespace mv::driftwire::tls {

namespace {

constexpr char kKnockSalt[crypto_pwhash_SALTBYTES + 1] = "driftwire-knock1";  // 16 bytes
constexpr std::uint64_t kMaxPageBytes = 4ull << 20;

constexpr std::string_view kBuiltinPage =
    "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>Welcome</title>\n"
    "<style>body{font-family:system-ui,sans-serif;max-width:36em;margin:4em auto;color:#333}</style>\n"
    "</head><body><h1>Welcome</h1><p>This site is being set up. Please check back later.</p></body></html>\n";

std::string answer(const char* status, std::string_view type, std::string_view body, bool head_only) {
  std::string out = "HTTP/1.1 ";
  out += status;
  out += "\r\nServer: nginx\r\nContent-Type: ";
  out += type;
  out += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  if (!head_only) out += body;
  return out;
}

std::string_view type_of(std::string_view path) {
  const auto dot = path.rfind('.');
  const auto ext = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css";
  if (ext == "js") return "text/javascript";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "ico") return "image/x-icon";
  if (ext == "txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// The relay program links no file port: the standard library reads its pages
// (a UTF-8 path on every OS).
std::filesystem::path utf8_path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::optional<std::string> read_page(const std::string& dir, const std::string& rel) {
  std::error_code ec;
  const auto path = utf8_path(dir) / utf8_path(rel);
  if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxPageBytes) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  std::string out(static_cast<std::size_t>(size), '\0');
  if (!in.read(out.data(), static_cast<std::streamsize>(out.size()))) return std::nullopt;
  return out;
}

bool grease(std::uint16_t v) noexcept { return (v & 0x0f0f) == 0x0a0a && (v >> 8) == (v & 0xff); }

std::string hex4(std::uint16_t v) {
  char b[5];
  std::snprintf(b, sizeof b, "%04x", static_cast<unsigned>(v));
  return b;
}

std::string sha12(const std::string& s) {
  if (s.empty()) return "000000000000";
  std::uint8_t h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(h, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  char out[13];
  for (int i = 0; i < 6; ++i) std::snprintf(out + 2 * i, 3, "%02x", h[i]);
  return out;
}

struct reader {
  std::span<const std::uint8_t> b;
  std::size_t at = 0;
  bool ok = true;
  std::size_t left() const { return at <= b.size() ? b.size() - at : 0; }
  std::uint32_t u(int n) {
    if (left() < static_cast<std::size_t>(n)) {
      ok = false;
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | b[at++];
    return v;
  }
  std::span<const std::uint8_t> take(std::size_t n) {
    if (left() < n) {
      ok = false;
      return {};
    }
    auto s = b.subspan(at, n);
    at += n;
    return s;
  }
};

}  // namespace

std::optional<knock_key> derive_knock_key(std::string_view server_key) {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  knock_key k{};
  if (crypto_pwhash(k.data(), k.size(), server_key.data(), server_key.size(),
                    reinterpret_cast<const std::uint8_t*>(kKnockSalt), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return std::nullopt;
  }
  return k;
}

std::array<std::uint8_t, kKnockBytes> knock_of(const knock_key& k, std::span<const std::uint8_t> exporter) {
  std::array<std::uint8_t, kKnockBytes> out{};
  crypto_generichash(out.data(), out.size(), exporter.data(), exporter.size(), k.data(), k.size());
  return out;
}

std::string decoy_response(std::string_view head, const decoy& d) {
  const auto end = head.find_first_of("\r\n");
  const auto line = head.substr(0, end);
  const auto sp1 = line.find(' ');
  const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.substr(sp2 + 1, 5) != "HTTP/") {
    return answer("400 Bad Request", "text/html", "<html><body><h1>400 Bad Request</h1></body></html>\n", false);
  }
  const auto method = line.substr(0, sp1);
  const bool head_only = method == "HEAD";
  if (method != "GET" && !head_only) {
    return answer("405 Not Allowed", "text/html", "<html><body><h1>405 Not Allowed</h1></body></html>\n", false);
  }
  auto path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (const auto q = path.find('?'); q != std::string_view::npos) path = path.substr(0, q);
  const auto not_found = [&] {
    return answer("404 Not Found", "text/html", "<html><body><h1>404 Not Found</h1></body></html>\n", head_only);
  };
  if (path.empty() || path.front() != '/') return not_found();
  std::string file(path == "/" ? std::string_view("/index.html") : path);
  if (file.back() == '/') file += "index.html";
  if (d.dir.empty()) {
    if (file != "/index.html") return not_found();
    return answer("200 OK", "text/html; charset=utf-8", kBuiltinPage, head_only);
  }
  // A plain path inside the folder only.
  if (file.find("..") != std::string::npos) return not_found();
  for (const char c : file) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
                    c == '.' || c == '-' || c == '_';
    if (!ok) return not_found();
  }
  const auto page = read_page(d.dir, file.substr(1));
  if (!page) return not_found();
  return answer("200 OK", type_of(file), *page, head_only);
}

std::optional<std::string> ja4_of(std::span<const std::uint8_t> record) {
  reader r{record};
  if (r.u(1) != 0x16) return std::nullopt;  // handshake
  r.u(2);
  const auto rec_len = r.u(2);
  if (!r.ok || r.left() < rec_len) return std::nullopt;
  if (r.u(1) != 0x01) return std::nullopt;  // ClientHello
  r.u(3);
  const auto legacy = static_cast<std::uint16_t>(r.u(2));
  r.take(32);
  r.take(r.u(1));
  std::vector<std::uint16_t> ciphers;
  {
    reader c{r.take(r.u(2))};
    while (c.ok && c.left() >= 2) {
      const auto v = static_cast<std::uint16_t>(c.u(2));
      if (!grease(v)) ciphers.push_back(v);
    }
  }
  r.take(r.u(1));
  if (!r.ok) return std::nullopt;
  std::vector<std::uint16_t> exts, sigalgs;
  bool sni = false;
  std::string alpn;
  std::uint16_t version = legacy;
  if (r.left() >= 2) {
    reader e{r.take(r.u(2))};
    while (e.ok && e.left() >= 4) {
      const auto type = static_cast<std::uint16_t>(e.u(2));
      reader body{e.take(e.u(2))};
      if (!e.ok) return std::nullopt;
      if (grease(type)) continue;
      exts.push_back(type);
      if (type == 0x0000) sni = true;
      if (type == 0x0010 && body.left() > 3) {  // ALPN: the first protocol
        body.u(2);
        const auto n = body.u(1);
        const auto p = body.take(n);
        if (body.ok) alpn.assign(p.begin(), p.end());
      }
      if (type == 0x002b && body.left() > 1) {  // supported_versions: the highest
        reader v{body.take(body.u(1))};
        std::uint16_t best = 0;
        while (v.ok && v.left() >= 2) {
          const auto x = static_cast<std::uint16_t>(v.u(2));
          if (!grease(x)) best = std::max(best, x);
        }
        if (best) version = best;
      }
      if (type == 0x000d && body.left() > 2) {  // signature_algorithms, in order
        reader s{body.take(body.u(2))};
        while (s.ok && s.left() >= 2) {
          const auto x = static_cast<std::uint16_t>(s.u(2));
          if (!grease(x)) sigalgs.push_back(x);
        }
      }
    }
  }
  const char* ver = version == 0x0304 ? "13" : version == 0x0303 ? "12" : version == 0x0302 ? "11" : "10";
  char a[32];
  std::snprintf(a, sizeof a, "t%s%c%02zu%02zu", ver, sni ? 'd' : 'i', std::min<std::size_t>(ciphers.size(), 99),
                std::min<std::size_t>(exts.size(), 99));
  std::string out = a;
  out += alpn.empty() ? std::string("00") : std::string{alpn.front(), alpn.back()};
  std::sort(ciphers.begin(), ciphers.end());
  std::string cs;
  for (const auto c : ciphers) cs += (cs.empty() ? "" : ",") + hex4(c);
  std::vector<std::uint16_t> sorted;
  for (const auto x : exts) {
    if (x != 0x0000 && x != 0x0010) sorted.push_back(x);
  }
  std::sort(sorted.begin(), sorted.end());
  std::string es;
  for (const auto x : sorted) es += (es.empty() ? "" : ",") + hex4(x);
  if (!sigalgs.empty()) {
    es += "_";
    bool first = true;
    for (const auto x : sigalgs) {
      es += (first ? "" : ",") + hex4(x);
      first = false;
    }
  }
  return out + "_" + sha12(cs) + "_" + sha12(es);
}

}  // namespace mv::driftwire::tls
