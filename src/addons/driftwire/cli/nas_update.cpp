// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/nas_update.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "addons/driftwire/cli/update.h"
#include "core/json.h"

namespace mv::driftwire::cli::nas_update {

namespace fs = std::filesystem;

namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string read_file(const std::string& path) {
  std::ifstream in(fs_path(path), std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool write_file(const std::string& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(fs_path(path).parent_path(), ec);
  const std::string part = path + ".part";
  {
    std::ofstream out(fs_path(part), std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    if (!out) return false;
  }
  fs::rename(fs_path(part), fs_path(path), ec);
  return !ec;
}

bool hex_ok(std::string_view h, std::size_t bytes) {
  if (h.size() != bytes * 2) return false;
  for (char c : h) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::vector<std::uint8_t> from_hex(std::string_view h) {
  std::vector<std::uint8_t> out(h.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    out[i] = static_cast<std::uint8_t>(v(h[2 * i]) * 16 + v(h[2 * i + 1]));
  }
  return out;
}

// SHA-256 of a file, lowercase hex; "" when it cannot be read.
std::string sha256_file(const std::string& path, std::uint64_t& size) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return {};
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  std::vector<char> buf(1 << 16);
  size = 0;
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n <= 0) break;
    crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()), static_cast<unsigned long long>(n));
    size += static_cast<std::uint64_t>(n);
  }
  unsigned char out[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, out);
  char hex[2 * crypto_hash_sha256_BYTES + 1];
  sodium_bin2hex(hex, sizeof hex, out, sizeof out);
  return hex;
}

}  // namespace

std::string signed_message(const offer& o) {
  std::string m = "mediaviewer-nas-update-v1\n";
  for (const std::string& f : {o.version, o.arch, o.url, o.sha256, std::to_string(o.size)}) {
    m += std::to_string(f.size());
    m += ':';
    m += f;
    m += '\n';
  }
  return m;
}

result<offer> verify_latest(std::string_view json, std::string_view arch, std::span<const std::uint8_t, 32> pinned,
                            std::string_view current) {
  if (sodium_init() < 0) return err(status::internal);
  const auto doc = json::parse(json, 6);
  if (!doc || doc->k != json::kind::object) return err(status::corrupt);
  const std::string* version = doc->str("version");
  const json::value* arches = doc->find("arch");
  if (!version || !parse_version(*version) || !arches || arches->k != json::kind::object) return err(status::corrupt);
  const json::value* entry = arches->find(arch);
  if (!entry || entry->k != json::kind::object) return err(status::not_found);
  const std::string* url = entry->str("url");
  const std::string* sha = entry->str("sha256");
  const std::string* sig = entry->str("sig");
  const auto size = entry->integer("size");
  if (!url || !url->starts_with("https://") || !sha || !hex_ok(*sha, 32) || !sig || !hex_ok(*sig, crypto_sign_BYTES) ||
      !size || *size <= 0 || *size > (1LL << 30)) {
    return err(status::corrupt);
  }
  offer o{*version, std::string(arch), *url, *sha, static_cast<std::uint64_t>(*size)};
  const std::string m = signed_message(o);
  const auto s = from_hex(*sig);
  if (crypto_sign_verify_detached(s.data(), reinterpret_cast<const unsigned char*>(m.data()), m.size(),
                                  pinned.data()) != 0) {
    return err(status::permission_denied);
  }
  if (!newer_version(o.version, current)) return err(status::not_found);
  return o;
}

expected install(const offer& o, const std::string& var, const io& io) {
  if (!io.fetch || !io.self_test) return err(status::invalid_arg);
  const std::string stage = var + "/stage";
  const std::string staged = stage + "/driftwire";
  std::error_code ec;
  fs::remove_all(fs_path(stage), ec);
  fs::create_directories(fs_path(stage), ec);
  if (ec) return err(status::io);
  const auto clean = [&] {
    std::error_code e;
    fs::remove_all(fs_path(stage), e);
  };
  if (const expected got = io.fetch(o.url, staged); !got) {
    clean();
    return got;
  }
  std::uint64_t size = 0;
  if (sha256_file(staged, size) != o.sha256 || size != o.size) {
    clean();
    return err(status::corrupt);  // not what was signed: never run, never kept
  }
  fs::permissions(fs_path(staged), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                       fs::perms::others_read | fs::perms::others_exec, ec);
  if (ec || !io.self_test(staged)) {
    clean();
    return err(status::cancelled);
  }
  const std::string bin = var + "/bin";
  fs::create_directories(fs_path(bin), ec);
  if (ec) {
    clean();
    return err(status::io);
  }
  // The one running now goes aside first, so a crash between the renames
  // leaves the package's own binary (target/bin), which start-stop-status uses.
  const std::string now = bin + "/driftwire";
  if (fs::exists(fs_path(now), ec)) {
    fs::rename(fs_path(now), fs_path(bin + "/driftwire.prev"), ec);
    if (ec) {
      clean();
      return err(status::io);
    }
  }
  fs::rename(fs_path(staged), fs_path(now), ec);
  clean();
  if (ec) return err(status::io);
  // start-stop-status counts this trial's starts; the service clears it once up.
  if (!write_file(var + "/state/trial", o.version + "\n0\n")) return err(status::io);
  return {};
}

void trial_passed(const std::string& var) {
  std::error_code ec;
  fs::remove(fs_path(var + "/state/trial"), ec);
}

bool skipped(const std::string& var, std::string_view version) {
  std::string text = read_file(var + "/state/skip");
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return !text.empty() && !newer_version(version, text);
}

std::string_view this_arch() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#else
  return "";
#endif
}

std::optional<std::array<std::uint8_t, 32>> pinned_key(std::string_view hex) {
  if (!hex_ok(hex, 32)) return std::nullopt;
  const auto bytes = from_hex(hex);
  std::array<std::uint8_t, 32> out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

std::chrono::seconds next_delay(bool first, std::uint32_t random) {
  using std::chrono::seconds;
  // First: 10 to 70 minutes after starting. Then: 24 hours, give or take 2.
  if (first) return seconds(600 + random % 3600);
  return seconds(22 * 3600 + random % (4 * 3600));
}

outcome check_once(const std::string& site, const std::string& var, std::string_view arch,
                   std::span<const std::uint8_t, 32> pinned, std::string_view current, const check_io& c) {
  if (arch.empty() || site.empty() || !c.get_json) return outcome::nothing;
  const auto text = c.get_json(site + "/nas/latest.json");
  if (!text) return outcome::failed;
  const auto o = verify_latest(*text, arch, pinned, current);
  if (!o) return o.error() == status::not_found ? outcome::nothing : outcome::refused;
  if (skipped(var, o->version)) return outcome::nothing;
  return install(o.value(), var, c.install) ? outcome::installed : outcome::failed;
}

}  // namespace mv::driftwire::cli::nas_update
