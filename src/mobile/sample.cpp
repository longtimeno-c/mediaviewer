// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/sample.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "addons/driftwire/pair_check.h"
#include "addons/driftwire/serve_wire.h"

namespace mv::mobile::sample {

namespace {

constexpr std::uint64_t kMaxRootsBytes = 4096;
constexpr std::size_t kMaxRel = 1024;

std::string_view trimmed(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

bool hex_key(std::string_view hex, std::array<std::uint8_t, 32>& out) {
  if (hex.size() != 64) return false;
  for (std::size_t i = 0; i < 32; ++i) {
    unsigned v = 0;
    for (std::size_t j = 0; j < 2; ++j) {
      const char c = hex[i * 2 + j];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else return false;
    }
    out[i] = static_cast<std::uint8_t>(v);
  }
  return true;
}

// One path component into the flat name; false when it holds anything else.
bool append_component(std::string& out, std::string_view part) {
  if (part.empty() || part == "." || part == "..") return false;
  for (char c : part) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                       c == '_' || c == '-';
    if (plain) out += c;
    else if (c == ' ') out += '_';
    else return false;
  }
  return true;
}

}  // namespace

bool is_code(std::string_view text) {
  const std::string_view t = trimmed(text);
  return same_ignoring_case(t, kCode) || same_ignoring_case(t, kLink) ||
         (t.size() == kLink.size() + 1 && t.back() == '/' && same_ignoring_case(t.substr(0, kLink.size()), kLink));
}

pair_payload payload() {
  pair_payload p;
  p.library_id = std::string(kLibraryID);
  p.library_name = std::string(kLibraryName);
  (void)hex_key(kPublisherKey, p.publisher_key);  // a constant: checked by the tests
  p.code = std::string(kCode);
  p.expires = 0;
  // No service: identity and access key stay zero, so the check code covers the
  // library and its publisher alone.
  p.fingerprint = driftwire::pairing_check(p.library_id, p.publisher_key, p.identity, p.access_key);
  return p;
}

std::optional<std::string> flat_name(std::string_view root, std::string_view rel) {
  if (root.empty() || rel.empty() || rel.size() > kMaxRel) return std::nullopt;
  std::string out;
  if (!append_component(out, root) || root.find('/') != std::string_view::npos) return std::nullopt;
  std::size_t at = 0;
  while (at <= rel.size()) {
    const std::size_t end = std::min(rel.find('/', at), rel.size());
    out += "__";
    if (!append_component(out, rel.substr(at, end - at))) return std::nullopt;
    at = end + 1;
  }
  return out;
}

static_remote::static_remote(std::string base, fetch_fn fetch) : base_(std::move(base)), fetch_(std::move(fetch)) {
  if (!base_.empty() && base_.back() != '/') base_ += '/';
}

bool static_remote::is_shut() const {
  std::lock_guard lock(m_);
  return shut_;
}

void static_remote::shut_down() noexcept {
  std::lock_guard lock(m_);
  shut_ = true;
  connected_ = false;
}

bool static_remote::connected() const {
  std::lock_guard lock(m_);
  return connected_;
}

void static_remote::disconnect() noexcept {
  std::lock_guard lock(m_);
  connected_ = false;
}

std::string static_remote::host() const {
  std::string_view b = base_;
  if (const auto scheme = b.find("://"); scheme != std::string_view::npos) b.remove_prefix(scheme + 3);
  return std::string(b.substr(0, b.find('/')));
}

result<std::vector<driftwire::serve::root_info>> static_remote::read_roots(const cancel_flag* cancel) {
  if (is_shut() || !fetch_) return err(status::io);
  std::string text;
  bool too_big = false;
  auto got = fetch_(
      base_ + "roots.json", 0, kMaxRootsBytes + 1,
      [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
        if (text.size() + bytes.size() > kMaxRootsBytes) {
          too_big = true;
          return false;
        }
        text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return !is_shut() && !(cancel && cancel->stopped());
      },
      cancel);
  if (too_big) return err(status::corrupt);
  if (!got) return err(got.error() == status::cancelled ? status::cancelled : status::io);
  auto roots = driftwire::serve::parse_roots(text);
  if (!roots) return err(status::corrupt);
  return std::move(*roots);
}

expected static_remote::connect(const cancel_flag* cancel) {
  auto roots = read_roots(cancel);
  std::lock_guard lock(m_);
  if (shut_) return err(status::io);
  if (!roots) {
    connected_ = false;
    return err(roots.error() == status::cancelled ? status::cancelled : status::io);
  }
  roots_ = std::move(*roots);
  connected_ = true;
  return {};
}

result<std::vector<driftwire::serve::root_info>> static_remote::roots() {
  {
    std::lock_guard lock(m_);
    if (shut_) return err(status::io);
    if (roots_) return *roots_;
  }
  auto roots = read_roots(nullptr);
  if (!roots) return err(roots.error());
  std::lock_guard lock(m_);
  roots_ = *roots;
  connected_ = true;
  return std::move(*roots);
}

result<std::vector<driftwire::serve::list_entry>> static_remote::list(const std::string&, const std::string&) {
  return err(status::unsupported_format);  // a static folder lists nothing; sync and the viewer never ask
}

result<driftwire::serve::stat_info> static_remote::stat(const std::string&, const std::string&) {
  return err(status::unsupported_format);
}

expected static_remote::read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                   std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel, bool) {
  if (is_shut() || !fetch_) return err(status::io);
  const auto name = flat_name(root, rel);
  if (!name) return err(status::not_found);  // nothing the publisher could have named
  bool stopped_by_sink = false;
  auto got = fetch_(
      base_ + *name, at, length,
      [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
        // Forget stops a read at its next chunk, as it cuts the link's.
        if (is_shut()) return false;
        if (!sink(offset, bytes)) {
          stopped_by_sink = true;
          return false;
        }
        return true;
      },
      cancel);
  if (got) return {};
  if (is_shut()) return err(status::io);
  if (stopped_by_sink || got.error() == status::cancelled) return err(status::cancelled);
  if (got.error() == status::not_found) return err(status::not_found);
  {
    std::lock_guard lock(m_);
    connected_ = false;
  }
  return err(status::io);
}

}  // namespace mv::mobile::sample
