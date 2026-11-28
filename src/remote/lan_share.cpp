// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/lan_share.h"

#include <array>
#include <charconv>
#include <cstdint>

#include "wire/share_users.h"

namespace mv::remote::lan_share {
namespace {

namespace su = driftwire::serve::share_users;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower(c);
  return out;
}

bool same_nocase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

std::optional<std::array<int, 4>> ipv4(std::string_view s) {
  std::array<int, 4> out{};
  for (int i = 0; i < 4; ++i) {
    const auto dot = s.find('.');
    const std::string_view part = i < 3 ? s.substr(0, dot) : s;
    if ((i < 3 && dot == std::string_view::npos) || part.empty() || part.size() > 3) return std::nullopt;
    int v = 0;
    auto [p, ec] = std::from_chars(part.data(), part.data() + part.size(), v);
    if (ec != std::errc{} || p != part.data() + part.size() || v > 255) return std::nullopt;
    out[i] = v;
    if (i < 3) s.remove_prefix(dot + 1);
  }
  return out;
}

bool private_ipv4(const std::array<int, 4>& a) {
  return a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
         (a[0] == 169 && a[1] == 254);
}

// A Bonjour name: letters, digits and '-' in dot-separated labels, ending ".local".
bool bonjour_name(std::string_view host) {
  static constexpr std::string_view kLocal = ".local";
  if (host.size() <= kLocal.size() || !same_nocase(host.substr(host.size() - kLocal.size()), kLocal)) return false;
  for (char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) return false;
  }
  return host.find("..") == std::string_view::npos && host.front() != '.';
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percent_decoded(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex_digit(s[i + 1]);
      const int lo = hex_digit(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

}  // namespace

std::string lan_host(std::string_view address) {
  std::string_view host;
  if (!address.empty() && address.front() == '[') {
    const auto close = address.find(']');
    if (close == std::string_view::npos) return {};
    host = address.substr(1, close - 1);
    // fc00::/7: the first byte is fc or fd. No zone ("%en0"): a URL cannot carry it well.
    if (host.size() < 3 || host.find('%') != std::string_view::npos) return {};
    const char a = lower(host[0]);
    const char b = lower(host[1]);
    if (a != 'f' || (b != 'c' && b != 'd')) return {};
    for (char c : host) {
      if (hex_digit(c) < 0 && c != ':') return {};
    }
    return lowered(host);
  }
  const auto colon = address.rfind(':');
  host = colon == std::string_view::npos ? address : address.substr(0, colon);
  if (host.empty()) return {};
  if (auto v4 = ipv4(host)) return private_ipv4(*v4) ? std::string(host) : std::string();
  return bonjour_name(host) ? lowered(host) : std::string();
}

std::optional<mounted> parse_mounted_from(std::string_view from) {
  if (from.size() < 2 || from.substr(0, 2) != "//") return std::nullopt;
  from.remove_prefix(2);
  const auto slash = from.find('/');
  if (slash == std::string_view::npos || slash == 0) return std::nullopt;
  std::string_view server = from.substr(0, slash);
  std::string_view share = from.substr(slash + 1);
  if (const auto at = server.rfind('@'); at != std::string_view::npos) server.remove_prefix(at + 1);
  // A workgroup or domain prefix ("WG;user@host") is before the '@', gone already.
  if (server.empty() || share.empty()) return std::nullopt;
  if (const auto more = share.find('/'); more != std::string_view::npos) share = share.substr(0, more);
  std::string host;
  if (server.front() == '[') {
    const auto close = server.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = lowered(server.substr(1, close - 1));
  } else {
    // smbfs may add a port ("host:445").
    host = lowered(server.substr(0, server.find(':')));
  }
  mounted m;
  m.host = std::move(host);
  m.share = percent_decoded(share);
  if (m.host.empty() || m.share.empty()) return std::nullopt;
  return m;
}

std::string share_url(std::string_view host, std::string_view label) {
  if (host.empty() || label.empty()) return {};
  const std::string h = host.find(':') != std::string_view::npos ? "[" + std::string(host) + "]" : std::string(host);
  return su::smb_url(h, su::share_name(label));
}

std::vector<mounted> mounted_from(std::string_view host, const std::vector<mounted>& already) {
  std::vector<mounted> out;
  if (host.empty()) return out;
  for (const auto& m : already) {
    if (same_nocase(m.host, host)) out.push_back(m);
  }
  return out;
}

plan plan_mounts(const input& in) {
  plan p;
  if (!in.enabled) {
    p.why = skip::off;
    return p;
  }
  // Never SMB over the internet (self-host-users-smb.md §6): only the LAN route that answered.
  if (in.over_internet || in.relayed) {
    p.why = skip::away;
    return p;
  }
  p.host = lan_host(in.address);
  if (p.host.empty()) {
    p.why = skip::away;
    return p;
  }
  if (in.labels.empty()) {
    p.why = skip::no_folders;
    return p;
  }
  const auto here = mounted_from(p.host, in.already);
  for (const auto& label : in.labels) {
    const std::string share = su::share_name(label);
    bool done = false;
    for (const auto& m : here) done = done || same_nocase(m.share, share);
    std::string url = share_url(p.host, label);
    bool twice = false;
    for (const auto& u : p.urls) twice = twice || same_nocase(u, url);
    if (!done && !twice && !url.empty()) p.urls.push_back(std::move(url));
  }
  if (p.urls.empty()) p.why = skip::all_mounted;
  return p;
}

std::string notice_words(std::string_view server, const outcome& o) {
  const std::string name(server.empty() ? std::string_view("The server") : server);
  if (o.needs_sign_in && o.mounted == 0) {
    return o.asked ? "Didn't mount " + name + "'s shares: the sign-in didn't go through."
                   : name + "'s shares need a sign-in once: File > Mount Server Shares in Finder.";
  }
  if (o.failed > 0 && o.mounted == 0) {
    return "Couldn't mount " + name + "'s shares in the Finder." +
           (o.asked ? std::string() : std::string(" File > Mount Server Shares in Finder tries again."));
  }
  if (o.mounted == 0) return {};
  std::string out = o.mounted == 1 ? name + "'s share is in the Finder." : name + "'s shares are in the Finder.";
  if (o.failed > 0) out += " " + std::to_string(o.failed) + (o.failed == 1 ? " other didn't mount." : " others didn't mount.");
  return out;
}

std::string skip_words(std::string_view server, skip why) {
  const std::string name(server.empty() ? std::string_view("The server") : server);
  switch (why) {
    case skip::away: return name + "'s shares mount only at home, on the same network.";
    case skip::no_folders: return name + " shares no folders.";
    case skip::all_mounted: return name + "'s shares are already in the Finder.";
    case skip::off:
    case skip::none: break;
  }
  return {};
}

}  // namespace mv::remote::lan_share
