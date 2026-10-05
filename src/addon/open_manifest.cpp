// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/open_manifest.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <set>

#include "core/json.h"

namespace mv::addon {
namespace {

constexpr std::size_t kManifestMaxBytes = 256u << 10;

bool is_lower_hex(std::string_view s, std::size_t length) noexcept {
  if (s.size() != length) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::uint8_t nibble(char c) noexcept {
  return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}

bool id_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

bool id_part(std::string_view part) noexcept {
  if (part.empty() || part.front() == '-' || part.back() == '-') return false;
  if (!std::all_of(part.begin(), part.end(), id_char)) return false;
  // Device names Windows keeps, with or without an extension after them.
  static constexpr std::array<std::string_view, 4> kDevices{"con", "prn", "aux", "nul"};
  if (std::find(kDevices.begin(), kDevices.end(), part) != kDevices.end()) return false;
  if (part.size() == 4 && (part.substr(0, 3) == "com" || part.substr(0, 3) == "lpt") &&
      part[3] >= '0' && part[3] <= '9') {
    return false;
  }
  return true;
}

// One line a person reads in a consent sheet: text, no control characters,
// nothing that reorders or hides what is beside it (bidi controls, zero
// width), bounded. UTF-8 validity is the parser's.
bool display_text(std::string_view s, std::size_t max_bytes, bool may_be_empty) noexcept {
  if (s.empty()) return may_be_empty;
  if (s.size() > max_bytes || s.front() == ' ' || s.back() == ' ') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) return false;
    if (c == 0xE2 && i + 2 < s.size()) {
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      const auto c2 = static_cast<unsigned char>(s[i + 2]);
      // U+200B..U+200F, U+2028..U+202E, U+2060..U+206F.
      if (c1 == 0x80 && ((c2 >= 0x8B && c2 <= 0x8F) || (c2 >= 0xA8 && c2 <= 0xAE))) return false;
      if (c1 == 0x81 && c2 >= 0xA0 && c2 <= 0xAF) return false;
    }
    // U+FEFF, a zero-width no-break space.
    if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBB &&
        static_cast<unsigned char>(s[i + 2]) == 0xBF) {
      return false;
    }
  }
  return true;
}

bool version_text(std::string_view v) noexcept {
  int parts = 0;
  std::size_t digits = 0;
  for (char c : v) {
    if (c == '.') {
      if (digits == 0) return false;
      ++parts;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      if (++digits > 9) return false;
    } else {
      return false;
    }
  }
  return parts == 2 && digits > 0;
}

// Keys a manifest may carry under each API. One this host knows nothing of
// is a mistake when the manifest claims no API newer than the host's, and
// something a newer host understands when it does.
constexpr std::array<std::string_view, 12> kTopLevelKeys{
    "schema", "id", "name", "version", "description", "licence", "publisher",
    "update_url", "api", "installed_size", "files", "contributes"};
constexpr std::array<std::string_view, 1> kContributesKeys{"themes"};
// Code, by any name the plan gives it: never loaded from a schema 2 add-on by
// a host that runs none (docs/design/25 "Code").
constexpr std::array<std::string_view, 4> kCodeKeys{"native", "chrome", "scripts", "main"};

template <std::size_t N>
bool known(const std::array<std::string_view, N>& keys, std::string_view key) noexcept {
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

}  // namespace

bool valid_open_id(std::string_view id) noexcept {
  if (id.size() < 3 || id.size() > 64) return false;
  int parts = 0;
  std::string_view rest = id;
  for (;;) {
    const auto dot = rest.find('.');
    const std::string_view part = rest.substr(0, dot);
    if (!id_part(part)) return false;
    if (parts == 0 && part == "mediaviewer") return false;
    ++parts;
    if (dot == std::string_view::npos) break;
    rest.remove_prefix(dot + 1);
  }
  return parts >= 2;
}

bool valid_https_url(std::string_view url) noexcept {
  constexpr std::string_view kScheme = "https://";
  if (url.size() > 2048 || url.substr(0, kScheme.size()) != kScheme) return false;
  const std::string_view rest = url.substr(kScheme.size());
  const std::string_view host = rest.substr(0, rest.find_first_of("/?#"));
  if (host.empty() || host.find('@') != std::string_view::npos) return false;
  for (const char ch : url) {
    const auto c = static_cast<unsigned char>(ch);
    // Printable ASCII only: anything else is percent-encoded by its author.
    if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') {
      return false;
    }
  }
  for (const char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
    if (!ok) return false;
  }
  return true;
}

std::string key_fingerprint(std::string_view key_hex) {
  if (!is_lower_hex(key_hex, 64) || sodium_init() < 0) return {};
  std::uint8_t key[32];
  for (std::size_t i = 0; i < 32; ++i) {
    key[i] = static_cast<std::uint8_t>(nibble(key_hex[2 * i]) << 4 | nibble(key_hex[2 * i + 1]));
  }
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, key, sizeof(key));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < 8; ++i) {
    if (i != 0 && i % 2 == 0) out.push_back('-');
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0xF]);
  }
  return out;
}

open_decision check_open_manifest(std::span<const std::uint8_t> bytes,
                                  std::span<const std::uint8_t> signature, std::uint32_t api,
                                  std::uint32_t api_oldest) {
  open_decision d;
  if (bytes.empty() || bytes.size() > kManifestMaxBytes) return d;
  if (signature.empty()) {
    d.why = rejection::missing_signature;
    return d;
  }

  // 1. Shape. These bytes are a stranger's whether or not they are signed
  // (the key is theirs too), so the parser is the strict one manifests have
  // always used and nothing is acted on until the signature below holds.
  const auto doc = json::parse(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  if (!doc || doc->k != json::kind::object) return d;
  const auto schema = doc->integer("schema");
  if (!schema) return d;
  if (*schema != kOpenManifestSchema) {
    d.why = rejection::unsupported_schema;
    return d;
  }
  const json::value* publisher = doc->find("publisher");
  if (!publisher || publisher->k != json::kind::object) return d;
  const std::string* key_hex = publisher->str("key");
  if (!key_hex || !is_lower_hex(*key_hex, 64)) return d;

  // 2. The publisher's signature over the exact bytes.
  std::uint8_t key[crypto_sign_PUBLICKEYBYTES];
  for (std::size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<std::uint8_t>(nibble((*key_hex)[2 * i]) << 4 |
                                       nibble((*key_hex)[2 * i + 1]));
  }
  // MediaViewer's own key signs MediaViewer's own add-ons (schema 1, the
  // release channel). A schema 2 manifest under it would borrow that name.
  const auto pinned = pinned_public_key();
  if (std::equal(pinned.begin(), pinned.end(), key)) {
    d.why = rejection::bad_signature;
    return d;
  }
  if (sodium_init() < 0 || signature.size() != crypto_sign_BYTES ||
      crypto_sign_verify_detached(signature.data(), bytes.data(), bytes.size(), key) != 0) {
    d.why = rejection::bad_signature;
    return d;
  }

  // 3. Fields.
  open_manifest& m = d.m;
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* version = doc->str("version");
  const std::string* licence = doc->str("licence");
  const std::string* publisher_name = publisher->str("name");
  const json::value* api_v = doc->find("api");
  const json::value* files = doc->find("files");
  const auto size = doc->integer("installed_size");
  if (!id || !name || !version || !licence || !publisher_name || !api_v ||
      api_v->k != json::kind::object || !files || files->k != json::kind::array ||
      files->a.empty() || !size || *size < 0) {
    return d;
  }
  const auto api_min = api_v->integer("min");
  const auto api_max = api_v->integer("max");
  if (!api_min || !api_max || *api_min < 1 || *api_max < *api_min || *api_max > 1'000'000) {
    return d;
  }
  if (!valid_open_id(*id) || !version_text(*version) || !display_text(*name, 64, false) ||
      !display_text(*publisher_name, 64, false) || !display_text(*licence, 64, false)) {
    return d;
  }
  const auto optional_text = [&](const json::value& from, std::string_view key, std::string& out,
                                 std::size_t max_bytes) {
    const json::value* v = from.find(key);
    if (!v) return true;
    if (v->k != json::kind::string || !display_text(v->s, max_bytes, true)) return false;
    out = v->s;
    return true;
  };
  const auto optional_url = [&](const json::value& from, std::string_view key, std::string& out) {
    const json::value* v = from.find(key);
    if (!v) return true;
    if (v->k != json::kind::string || (!v->s.empty() && !valid_https_url(v->s))) return false;
    out = v->s;
    return true;
  };
  if (!optional_text(*doc, "description", m.description, 280) ||
      !optional_url(*publisher, "url", m.publisher_url) ||
      !optional_url(*doc, "update_url", m.update_url)) {
    return d;
  }
  m.id = *id;
  m.name = *name;
  m.version = *version;
  m.licence = *licence;
  m.publisher_name = *publisher_name;
  m.publisher_key = *key_hex;
  m.api_min = static_cast<std::uint32_t>(*api_min);
  m.api_max = static_cast<std::uint32_t>(*api_max);
  m.installed_size = static_cast<std::uint64_t>(*size);

  // Code is refused by name before anything else is weighed: no API range
  // and no later key turns a data add-on into one that runs.
  for (const auto& [key_name, value] : doc->o) {
    (void)value;
    if (known(kCodeKeys, key_name)) {
      d.why = rejection::code_not_allowed;
      return d;
    }
  }
  const json::value* contributes = doc->find("contributes");
  if (contributes) {
    if (contributes->k != json::kind::object) return d;
    for (const auto& [key_name, value] : contributes->o) {
      (void)value;
      if (known(kCodeKeys, key_name)) {
        d.why = rejection::code_not_allowed;
        return d;
      }
    }
  }

  std::set<std::string> seen;
  std::uint64_t total = 0;
  for (const json::value& f : files->a) {
    if (f.k != json::kind::object) return d;
    const std::string* path = f.str("path");
    const std::string* sha = f.str("sha256");
    const auto file_size = f.integer("size");
    const std::string* file_licence = f.str("licence");
    if (!path || !sha || !file_size || *file_size < 0 || !is_lower_hex(*sha, 64) ||
        !file_licence || !display_text(*file_licence, 64, false)) {
      return d;
    }
    if (!safe_relative_path(*path) || path->back() == '/') {
      d.why = rejection::unsafe_path;
      return d;
    }
    std::string folded = *path;
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    // The two names the package keeps for itself are never a listed file.
    if (folded == "manifest.json" || folded == "manifest.json.sig") return d;
    if (!seen.insert(folded).second) return d;
    total += static_cast<std::uint64_t>(*file_size);
    m.files.push_back(manifest_file{*path, *sha, static_cast<std::uint64_t>(*file_size),
                                    *file_licence});
  }
  if (total != m.installed_size) return d;

  // 4. Policy: the API range, then what it contributes under that API.
  if (m.api_min > api || m.api_max < api_oldest) {
    d.why = rejection::needs_update;
    return d;
  }
  // A manifest that claims nothing newer than this host is held to what this
  // host knows; one that reaches past it may carry keys for a later host.
  const bool strict = m.api_max <= api;
  if (strict) {
    for (const auto& [key_name, value] : doc->o) {
      (void)value;
      if (!known(kTopLevelKeys, key_name)) return d;
    }
  }
  if (contributes) {
    for (const auto& [key_name, value] : contributes->o) {
      if (key_name == "themes") {
        if (value.k != json::kind::array || value.a.empty() || value.a.size() > 32) return d;
        std::set<std::string> theme_ids;
        for (const json::value& t : value.a) {
          if (t.k != json::kind::object) return d;
          const std::string* theme_id = t.str("id");
          const std::string* theme_name = t.str("name");
          const std::string* theme_path = t.str("path");
          if (!theme_id || !theme_name || !theme_path || theme_id->empty() ||
              theme_id->size() > 32 || !id_part(*theme_id) ||
              !display_text(*theme_name, 64, false)) {
            return d;
          }
          if (!theme_ids.insert(*theme_id).second) return d;
          const bool listed = std::any_of(m.files.begin(), m.files.end(),
                                          [&](const manifest_file& f) { return f.path == *theme_path; });
          if (!listed) {
            d.why = rejection::unsafe_path;
            return d;
          }
          m.themes.push_back(theme_ref{*theme_id, *theme_name, *theme_path});
        }
      } else if (strict && !known(kContributesKeys, key_name)) {
        return d;
      }
    }
  }
  d.why = rejection::none;
  return d;
}

}  // namespace mv::addon
