// SPDX-License-Identifier: GPL-2.0-or-later
#include "addon/manifest.h"

#include <sodium.h>

#include <algorithm>
#include <set>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::addon {
namespace {

// UpdateKeys.ProductionPublicKeyHex (src.managed/MediaViewer.Updater).
constexpr std::uint8_t kPinnedKey[32] = {
    0x04, 0x51, 0xbf, 0xec, 0xfb, 0x6a, 0x26, 0xd9, 0x05, 0x8f, 0xb0, 0x9c, 0xfa, 0x7a, 0x93, 0x05,
    0xdc, 0xf1, 0xce, 0xa7, 0xc8, 0x72, 0x21, 0xe9, 0x18, 0x5b, 0x00, 0x38, 0xb1, 0xcc, 0x90, 0x8c,
};

bool sodium_ready() noexcept {
  static const bool ready = sodium_init() >= 0;
  return ready;
}

bool is_hex64(std::string_view s) noexcept {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool parse_version(std::string_view v) noexcept {
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

bool read_file(const json::value& v, manifest_file& out, bool need_licence) {
  if (v.k != json::kind::object) return false;
  const std::string* path = v.str("path");
  const std::string* sha = v.str("sha256");
  const auto size = v.integer("size");
  const std::string* licence = v.str("licence");
  if (!path || !sha || !size || *size < 0 || !is_hex64(*sha)) return false;
  if (need_licence && (!licence || licence->empty())) return false;
  out.path = *path;
  out.sha256 = *sha;
  out.size = static_cast<std::uint64_t>(*size);
  out.licence = licence ? *licence : std::string();
  return true;
}

}  // namespace

const char* rejection_name(rejection r) noexcept {
  switch (r) {
    case rejection::none: return "ok";
    case rejection::key_not_configured: return "key_not_configured";
    case rejection::missing_signature: return "missing_signature";
    case rejection::bad_signature: return "bad_signature";
    case rejection::malformed: return "malformed";
    case rejection::unsupported_schema: return "unsupported_schema";
    case rejection::wrong_platform: return "wrong_platform";
    case rejection::unsafe_path: return "unsafe_path";
    case rejection::file_missing: return "file_missing";
    case rejection::file_mismatch: return "file_mismatch";
    case rejection::unexpected_file: return "unexpected_file";
    case rejection::needs_update: return "needs_update";
  }
  return "unknown";
}

std::span<const std::uint8_t, 32> pinned_public_key() noexcept {
  return std::span<const std::uint8_t, 32>(kPinnedKey);
}

bool safe_relative_path(std::string_view path) noexcept {
  if (path.empty() || path.size() > 512) return false;
  if (path.front() == '/' || path.front() == '\\') return false;
  if (path.find('\\') != std::string_view::npos) return false;  // '/' only, on every OS
  if (path.find(':') != std::string_view::npos) return false;   // drive letters, ADS
  for (char c : path) {
    if (static_cast<unsigned char>(c) < 0x20) return false;
  }
  std::string_view rest = path;
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
  }
  return true;
}

decision check_manifest(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> signature,
                        std::span<const std::uint8_t> public_key, std::uint32_t host_api,
                        std::string_view expected_platform) {
  decision d;
  // 1. Signature first. Nothing below runs on unauthenticated bytes.
  if (public_key.size() != 32 ||
      std::all_of(public_key.begin(), public_key.end(), [](std::uint8_t b) { return b == 0; })) {
    d.why = rejection::key_not_configured;
    return d;
  }
  if (signature.empty()) {
    d.why = rejection::missing_signature;
    return d;
  }
  if (!sodium_ready() || signature.size() != crypto_sign_BYTES ||
      crypto_sign_verify_detached(signature.data(), bytes.data(), bytes.size(),
                                  public_key.data()) != 0) {
    d.why = rejection::bad_signature;
    return d;
  }

  // 2. Shape.
  const auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                bytes.size()));
  if (!doc || doc->k != json::kind::object) return d;
  const auto schema = doc->integer("schema");
  if (!schema) return d;
  manifest& m = d.m;
  m.schema = static_cast<int>(*schema);
  const std::string* id = doc->str("id");
  const std::string* name = doc->str("name");
  const std::string* version = doc->str("version");
  const std::string* platform = doc->str("platform");
  const std::string* native = doc->str("native");
  const std::string* chrome = doc->str("chrome");
  const auto size = doc->integer("installed_size");
  const json::value* host = doc->find("host_api");
  const json::value* files = doc->find("files");
  const json::value* archive = doc->find("archive");
  if (!id || !name || !version || !platform || !native || !chrome || !size || *size < 0 || !host ||
      host->k != json::kind::object || !files || files->k != json::kind::array || files->a.empty() ||
      !archive) {
    return d;
  }
  const auto host_min = host->integer("min");
  const auto host_max = host->integer("max");
  if (!host_min || !host_max || *host_min < 1 || *host_max < *host_min) return d;
  if (id->empty() || id->size() > 32 ||
      !std::all_of(id->begin(), id->end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; })) {
    return d;
  }
  if (!parse_version(*version) || name->empty() || name->find_first_of("/\\:") != std::string::npos) {
    return d;
  }
  m.id = *id;
  m.name = *name;
  m.version = *version;
  m.platform = *platform;
  m.native = *native;
  m.chrome = *chrome;
  m.installed_size = static_cast<std::uint64_t>(*size);
  m.host_api_min = static_cast<std::uint32_t>(*host_min);
  m.host_api_max = static_cast<std::uint32_t>(*host_max);
  if (!read_file(*archive, m.archive, false)) return d;
  std::set<std::string> seen;
  for (const json::value& f : files->a) {
    manifest_file mf;
    if (!read_file(f, mf, true)) return d;
    if (!safe_relative_path(mf.path)) {
      d.why = rejection::unsafe_path;
      return d;
    }
    // Case-folded, so two entries cannot name one file on Windows or macOS.
    std::string folded = mf.path;
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (!seen.insert(folded).second) return d;
    m.files.push_back(std::move(mf));
  }
  if (!safe_relative_path(m.archive.path) || m.archive.path.find('/') != std::string::npos) {
    d.why = rejection::unsafe_path;
    return d;
  }
  const auto listed = [&m](const std::string& p) {
    return std::any_of(m.files.begin(), m.files.end(), [&p](const manifest_file& f) {
      return f.path == p || f.path.rfind(p + "/", 0) == 0;
    });
  };
  if (!safe_relative_path(m.native) || !safe_relative_path(m.chrome) || !listed(m.native) ||
      !listed(m.chrome)) {
    d.why = rejection::unsafe_path;
    return d;
  }

  // 3. Policy.
  if (m.schema != kManifestSchema) {
    d.why = rejection::unsupported_schema;
    return d;
  }
  if (m.platform != expected_platform) {
    d.why = rejection::wrong_platform;
    return d;
  }
  if (host_api < m.host_api_min || host_api > m.host_api_max) {
    d.why = rejection::needs_update;
    return d;
  }
  d.why = rejection::none;
  return d;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  if (!sodium_ready()) return {};
  unsigned char out[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(out, bytes.data(), bytes.size());
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    s[2 * i] = kHex[out[i] >> 4];
    s[2 * i + 1] = kHex[out[i] & 0xF];
  }
  return s;
}

std::string sha256_file(const std::string& path) {
  if (!sodium_ready()) return {};
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential)) return {};
  io::aligned_buffer buf(1u << 20);
  if (buf.empty()) return {};
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  for (;;) {
    auto got = in.read(std::span<std::uint8_t>(buf.data(), buf.size()));
    if (!got) return {};
    if (*got == 0) break;
    crypto_hash_sha256_update(&st, buf.data(), *got);
    if (*got < buf.size()) break;
  }
  unsigned char out[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, out);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    s[2 * i] = kHex[out[i] >> 4];
    s[2 * i + 1] = kHex[out[i] & 0xF];
  }
  return s;
}

rejection verify_files(const std::string& dir, const manifest& m) {
  std::set<std::string> listed;
  for (const manifest_file& f : m.files) {
    const std::string full = io::join_path(dir, io::native_relative(f.path));
    auto st = io::stat_path(full);
    if (!st || st->is_directory) return rejection::file_missing;
    if (st->size != f.size) return rejection::file_mismatch;
    if (sha256_file(full) != f.sha256) return rejection::file_mismatch;
    listed.insert(f.path);
  }
  // Nothing else may sit beside them: a dropped-in DLL would otherwise ride
  // along with a valid signature.
  rejection extra = rejection::none;
  (void)io::walk_files(dir, 16, [&](const io::tree_entry& e) {
    if (e.relative_utf8 == "manifest.json" || e.relative_utf8 == "manifest.json.sig") return true;
    if (!listed.count(e.relative_utf8)) {
      extra = rejection::unexpected_file;
      return false;
    }
    return true;
  });
  return extra;
}

}  // namespace mv::addon
