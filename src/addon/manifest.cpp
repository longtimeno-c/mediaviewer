// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/manifest.h"

#include <sodium.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::addon {
namespace {

// UpdateKeys.ProductionPublicKeyHex (src.managed/MediaViewer.Updater).
[[maybe_unused]] constexpr std::uint8_t kPinnedKey[32] = {
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
    case rejection::over_ceiling: return "over_ceiling";
    case rejection::too_large: return "too_large";
    case rejection::bad_package: return "bad_package";
    case rejection::code_not_allowed: return "code_not_allowed";
    case rejection::other_publisher: return "other_publisher";
    case rejection::downgrade: return "downgrade";
    case rejection::not_approved: return "not_approved";
    case rejection::changed: return "changed";
    case rejection::invalid_theme: return "invalid_theme";
  }
  return "unknown";
}

std::uint32_t negotiated_host_api(const manifest& m, std::uint32_t host_api) noexcept {
  return std::min(host_api, m.host_api_max);
}

std::string_view current_arch() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return "other";
#endif
}

std::uint64_t family_ceiling(std::string_view family) noexcept {
  // plan/17 "The AI pack": Core + the selected vendor piece + Faces <= 3 GB.
  // Decimal GB, the unit the Settings page shows.
  if (family == "ai") return 3'000'000'000ull;
  return 0;
}

#if defined(MV_ADDON_DEV_PUBLIC_KEY_HEX)
// A developer build (CMake MV_ADDON_DEV_PUBLIC_KEY, never set by the release
// workflow) trusts a development key instead, so a locally signed pack can be
// sideloaded and the whole install / load / run path exercised without the
// release key (Milestone H validation). The warning at configure time says so.
namespace {
constexpr std::uint8_t nibble(char c) {
  return static_cast<std::uint8_t>(c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0));
}
constexpr char kDevHex[] = MV_ADDON_DEV_PUBLIC_KEY_HEX;
static_assert(sizeof(kDevHex) == 65, "MV_ADDON_DEV_PUBLIC_KEY must be 64 lowercase hex digits");
struct dev_key {
  std::uint8_t b[32]{};
  constexpr dev_key() {
    for (int i = 0; i < 32; ++i) b[i] = static_cast<std::uint8_t>(nibble(kDevHex[2 * i]) << 4 | nibble(kDevHex[2 * i + 1]));
  }
};
constexpr dev_key kDevKey{};
}  // namespace

std::span<const std::uint8_t, 32> pinned_public_key() noexcept {
  return std::span<const std::uint8_t, 32>(kDevKey.b);
}
#else
std::span<const std::uint8_t, 32> pinned_public_key() noexcept {
  return std::span<const std::uint8_t, 32>(kPinnedKey);
}
#endif

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
                        std::string_view expected_platform, std::uint32_t host_api_oldest) {
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
  // Optional (Milestone H). Absent is "", present must be a string.
  const json::value* part_of_v = doc->find("part_of");
  const json::value* arch_v = doc->find("arch");
  if ((part_of_v && part_of_v->k != json::kind::string) ||
      (arch_v && arch_v->k != json::kind::string)) {
    return d;
  }
  if (!id || !name || !version || !platform || !native || !chrome || !size || *size < 0 || !host ||
      host->k != json::kind::object || !files || files->k != json::kind::array || files->a.empty() ||
      !archive) {
    return d;
  }
  const auto host_min = host->integer("min");
  const auto host_max = host->integer("max");
  if (!host_min || !host_max || *host_min < 1 || *host_max < *host_min ||
      *host_max > std::int64_t{UINT32_MAX} || *schema < 0 || *schema > 1000) {
    return d;
  }
  if (id->empty() || id->size() > 32 ||
      !std::all_of(id->begin(), id->end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; })) {
    return d;
  }
  // The name is a folder on the Mac (Add-ons/<name>/<version>, store.cpp): one
  // plain, visible path component, never "." or "..".
  if (!parse_version(*version) || !safe_relative_path(*name) ||
      name->find('/') != std::string::npos || name->front() == '.' || name->size() > 64) {
    return d;
  }
  m.id = *id;
  m.name = *name;
  m.version = *version;
  m.platform = *platform;
  m.native = *native;
  m.chrome = *chrome;
  m.part_of = part_of_v ? part_of_v->s : std::string();
  m.arch = arch_v ? arch_v->s : std::string();
  if (!m.part_of.empty() &&
      (m.part_of.size() > 32 || m.part_of == m.id ||
       !std::all_of(m.part_of.begin(), m.part_of.end(),
                    [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; }))) {
    return d;
  }
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
  if (m.part_of.empty()) {
    if (!safe_relative_path(m.native) || !safe_relative_path(m.chrome) || !listed(m.native) ||
        !listed(m.chrome)) {
      d.why = rejection::unsafe_path;
      return d;
    }
  } else if (!m.native.empty() || !m.chrome.empty()) {
    // A piece runs nothing of its own: the parent loads it.
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
  // Only this build's own platform knows its architecture; the packing
  // tool's cross-check (another platform) does not judge it.
  if (!m.arch.empty() && expected_platform == kPlatform && m.arch != current_arch()) {
    d.why = rejection::wrong_platform;
    return d;
  }
  if (m.host_api_min > host_api || m.host_api_max < host_api_oldest) {
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

namespace {

// Hashed once per process (Milestone H): the AI pack is gigabytes, and a
// start lists the add-ons, loads one, and asks for its pieces, each of which
// verified every byte again (~6 GB hashed per launch with the whole pack).
// The first check of a version folder hashes every file, as before; a later
// one in the same process still stats every file and walks for extras, and
// hashes again only if any size or modification time moved. Keyed by the
// folder and the signed hashes, so a new version or manifest verifies afresh.
struct verified_snapshot {
  std::vector<std::pair<std::uint64_t, std::int64_t>> files;  // size, mtime_ns
};
std::mutex g_verified_m;
std::map<std::string, verified_snapshot> g_verified;
// Folders being hashed now (keys as above). A second caller for the same
// folder waits for the first and takes its answer instead of hashing the same
// gigabytes beside it: at launch the pack's load and Settings' installed-state
// read both verified the AI pack at once, each taking the full time
// (2026-09-27, "Settings forgets what is installed").
std::condition_variable g_verified_cv;
std::set<std::string> g_hashing;

}  // namespace

void note_verified_move(const std::string& from_dir, const std::string& to_dir) {
  // A rename keeps every file's size and modification time, so what was
  // hashed in staging is what now sits in the version folder (store::install).
  std::lock_guard lock(g_verified_m);
  const std::string prefix = from_dir + '\n';
  std::vector<std::pair<std::string, verified_snapshot>> moved;
  for (auto it = g_verified.lower_bound(prefix);
       it != g_verified.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
    moved.emplace_back(to_dir + it->first.substr(from_dir.size()), std::move(it->second));
    it = g_verified.erase(it);
  }
  for (auto& [key, snap] : moved) g_verified[key] = std::move(snap);
}

rejection verify_files(const std::string& dir, const manifest& m) {
  return verify_files(dir, std::span<const manifest_file>(m.files));
}

rejection verify_files(const std::string& dir, std::span<const manifest_file> files) {
  std::set<std::string> listed;
  std::string key = dir;
  verified_snapshot now;
  now.files.reserve(files.size());
  for (const manifest_file& f : files) {
    const std::string full = io::join_path(dir, io::native_relative(f.path));
    auto st = io::stat_path(full);
    if (!st || st->is_directory) return rejection::file_missing;
    if (st->size != f.size) return rejection::file_mismatch;
    now.files.emplace_back(st->size, st->mtime_ns);
    key += '\n';
    key += f.path;
    key += ':';
    key += f.sha256;
  }
  bool hashed = false;
  {
    std::unique_lock lock(g_verified_m);
    // Another thread hashing this folder: its answer is this one's.
    g_verified_cv.wait(lock, [&] { return g_hashing.count(key) == 0; });
    const auto it = g_verified.find(key);
    hashed = it != g_verified.end() && it->second.files == now.files;
    if (!hashed) g_hashing.insert(key);
  }
  if (!hashed) {
    struct done_hashing {
      const std::string& key;
      ~done_hashing() {
        {
          std::lock_guard lock(g_verified_m);
          g_hashing.erase(key);
        }
        g_verified_cv.notify_all();
      }
    } const release{key};
    for (const manifest_file& f : files) {
      if (sha256_file(io::join_path(dir, io::native_relative(f.path))) != f.sha256) {
        std::lock_guard lock(g_verified_m);
        g_verified.erase(key);
        return rejection::file_mismatch;
      }
    }
    // Recorded before the waiters wake so they find it; the walk for extra
    // files below still runs for every caller and takes it back out.
    std::lock_guard lock(g_verified_m);
    g_verified[key] = now;
  }
  for (const manifest_file& f : files) listed.insert(f.path);
  // Nothing else may sit beside them: a dropped-in DLL would otherwise ride
  // along with a valid signature. The walk skips nothing (a hidden DLL loads
  // as well as a visible one) and follows no link: a link, a junction, or a
  // folder nested deeper than any add-on needs is itself unexpected.
  // The shells' own folder metadata is the one exception: Finder and Explorer
  // write it into any folder a user opens, and nothing ever loads it as code.
  const auto shell_metadata = [](std::string_view rel) {
    const std::string_view leaf = rel.substr(rel.find_last_of('/') + 1);
    std::string lower(leaf);
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return leaf == ".DS_Store" || lower == "thumbs.db" || lower == "desktop.ini";
  };
  rejection extra = rejection::none;
  const auto walked = io::walk_all_entries(dir, 16, [&](std::string_view rel, io::entry_kind kind) {
    if (kind == io::entry_kind::directory) return true;
    if (kind == io::entry_kind::file &&
        (rel == "manifest.json" || rel == "manifest.json.sig" || listed.count(std::string(rel)) ||
         shell_metadata(rel))) {
      return true;
    }
    extra = rejection::unexpected_file;
    return false;
  });
  if (extra == rejection::none && !walked) extra = rejection::file_missing;
  if (extra != rejection::none) {
    std::lock_guard lock(g_verified_m);
    g_verified.erase(key);
  }
  return extra;
}

}  // namespace mv::addon
