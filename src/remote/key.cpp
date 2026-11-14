// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/key.h"

namespace mv::remote {

bool is_hex16(std::string_view s) noexcept {
  if (s.size() != 16) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

namespace {

// '/'-separated, no empty, "." or ".." segment, no NUL or backslash (a
// Windows separator the service would never send in a name).
bool good_rel(std::string_view rel) {
  if (rel.empty()) return true;
  std::size_t start = 0;
  while (start <= rel.size()) {
    const std::size_t slash = rel.find('/', start);
    const std::string_view seg = rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    for (const char c : seg) {
      if (c == '\0' || c == '\\') return false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

}  // namespace

std::optional<key_parts> parse_key(std::string_view text) {
  if (!is_key(text)) return std::nullopt;
  std::string_view rest = text.substr(kKeyPrefix.size());
  key_parts k;
  const std::size_t s1 = rest.find('/');
  if (s1 == std::string_view::npos) return std::nullopt;
  k.server.assign(rest.substr(0, s1));
  if (!is_hex16(k.server)) return std::nullopt;
  rest.remove_prefix(s1 + 1);
  if (rest.empty()) return k;  // the server's top
  const std::size_t s2 = rest.find('/');
  if (s2 == std::string_view::npos) return std::nullopt;  // a root is always followed by '/'
  k.root.assign(rest.substr(0, s2));
  if (!is_hex16(k.root)) return std::nullopt;
  rest.remove_prefix(s2 + 1);
  k.folder = rest.empty() || rest.back() == '/';
  if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
  if (!good_rel(rest)) return std::nullopt;
  k.rel.assign(rest);
  return k;
}

std::string make_key(const key_parts& k) {
  std::string out(kKeyPrefix);
  out += k.server;
  out += '/';
  if (k.root.empty()) return out;
  out += k.root;
  out += '/';
  if (k.rel.empty()) return out;
  out += k.rel;
  if (k.folder) out += '/';
  return out;
}

std::string server_key(std::string_view server) {
  std::string out(kKeyPrefix);
  out += server;
  out += '/';
  return out;
}

std::string parent_key(std::string_view key) {
  auto k = parse_key(key);
  if (!k || k->root.empty()) return {};
  key_parts p;
  p.server = k->server;
  if (k->rel.empty()) return make_key(p);
  p.root = k->root;
  const std::size_t slash = k->rel.rfind('/');
  p.rel = slash == std::string::npos ? std::string{} : k->rel.substr(0, slash);
  p.folder = true;
  return make_key(p);
}

std::string leaf_name(std::string_view key) {
  auto k = parse_key(key);
  if (!k || k->rel.empty()) return {};
  const std::size_t slash = k->rel.rfind('/');
  return slash == std::string::npos ? k->rel : k->rel.substr(slash + 1);
}

}  // namespace mv::remote
