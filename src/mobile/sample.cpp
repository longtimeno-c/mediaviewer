// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/sample.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <sodium.h>

#include "addons/driftwire/pair_check.h"
#include "addons/driftwire/serve_wire.h"
#include "addons/driftwire/wire.h"
#include "core/json.h"

namespace mv::mobile::sample {

namespace {

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

namespace {

std::string hex_of(const unsigned char* p, std::size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out += digits[p[i] >> 4];
    out += digits[p[i] & 15];
  }
  return out;
}

bool lower_hex(std::string_view s, std::size_t n) {
  return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

constexpr std::uint64_t kMaxListBytes = 256 * 1024;
constexpr std::uint64_t kMaxListedFile = 512ull * 1024 * 1024;

bool catalogue_signed(std::string_view rel) {
  return rel == "current.json" || rel.starts_with("gen/") || rel.starts_with("models/");
}

}  // namespace

std::optional<file_list> parse_file_list(std::string_view text, std::string_view signature_hex, const key32& key,
                                         std::string_view library_id) {
  if (sodium_init() < 0 || text.size() > kMaxListBytes || !lower_hex(signature_hex, crypto_sign_BYTES * 2)) return std::nullopt;
  unsigned char sig[crypto_sign_BYTES];
  for (std::size_t i = 0; i < crypto_sign_BYTES; ++i) {
    const auto nib = [](char c) { return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10); };
    sig[i] = static_cast<unsigned char>(nib(signature_hex[i * 2]) << 4 | nib(signature_hex[i * 2 + 1]));
  }
  std::string signed_bytes(kFilesContext);
  signed_bytes.append(text);
  if (crypto_sign_verify_detached(sig, reinterpret_cast<const unsigned char*>(signed_bytes.data()), signed_bytes.size(),
                                  key.data()) != 0) {
    return std::nullopt;
  }
  const auto doc = json::parse(text, 6);
  if (!doc || doc->integer("format") != 1) return std::nullopt;
  const std::string* id = doc->str("library_id");
  if (!id || *id != library_id) return std::nullopt;
  const json::value* roots = doc->find("roots");
  const json::value* files = doc->find("files");
  if (!roots || roots->k != json::kind::array || !files || files->k != json::kind::array) return std::nullopt;
  file_list out;
  for (const json::value& r : roots->a) {
    const std::string* rid = r.str("id");
    const std::string* label = r.str("label");
    if (!rid || !label || !driftwire::valid_id(*rid) || label->size() > 256) return std::nullopt;
    if (*label == "Catalogue") out.catalogue_root = *rid;
    out.roots.push_back({*rid, *label});
  }
  if (out.catalogue_root.empty()) return std::nullopt;
  for (const json::value& f : files->a) {
    const std::string* name = f.str("name");
    const std::string* sha = f.str("sha256");
    const auto size = f.integer("size");
    if (!name || name->empty() || name->size() > 2048 || name->find('/') != std::string::npos || !sha ||
        !lower_hex(*sha, 64) || !size || *size < 0 || static_cast<std::uint64_t>(*size) > kMaxListedFile) {
      return std::nullopt;
    }
    out.files[*name] = {static_cast<std::uint64_t>(*size), *sha};
  }
  return out;
}

static_remote::static_remote(std::string base, fetch_fn fetch, key32 key, std::string library_id,
                             std::string verified_dir, std::uint64_t verified_budget)
    : base_(std::move(base)), fetch_(std::move(fetch)), key_(key), library_id_(std::move(library_id)),
      verified_dir_(std::move(verified_dir)), verified_budget_(verified_budget) {
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

result<std::string> static_remote::small(const std::string& name, std::uint64_t cap, const cancel_flag* cancel) {
  if (is_shut() || !fetch_) return err(status::io);
  std::string text;
  bool too_big = false;
  auto got = fetch_(
      base_ + name, 0, cap + 1,
      [&](std::uint64_t, std::span<const std::uint8_t> bytes) {
        if (text.size() + bytes.size() > cap) {
          too_big = true;
          return false;
        }
        text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return !is_shut() && !(cancel && cancel->stopped());
      },
      cancel);
  if (too_big) return err(status::corrupt);
  if (!got) return err(got.error() == status::cancelled ? status::cancelled : status::io);
  return text;
}

result<file_list> static_remote::read_list(const cancel_flag* cancel) {
  MV_TRY(std::string text, small("files.json", kMaxListBytes, cancel));
  MV_TRY(std::string sig, small("files.sig", 256, cancel));
  while (!sig.empty() && std::isspace(static_cast<unsigned char>(sig.back()))) sig.pop_back();
  auto list = parse_file_list(text, sig, key_, library_id_);
  if (!list) return err(status::permission_denied);  // not signed by the pinned key, or not this library
  return std::move(*list);
}

result<file_list> static_remote::list_now(const cancel_flag* cancel) {
  {
    std::lock_guard lock(m_);
    if (shut_) return err(status::io);
    if (list_) return *list_;
  }
  MV_TRY(file_list list, read_list(cancel));
  std::lock_guard lock(m_);
  list_ = list;
  connected_ = true;
  return list;
}

expected static_remote::connect(const cancel_flag* cancel) {
  auto list = read_list(cancel);
  std::lock_guard lock(m_);
  if (shut_) return err(status::io);
  if (!list) {
    connected_ = false;
    // permission_denied: the list is not signed by the pinned key, or is another library's.
    if (list.error() == status::cancelled || list.error() == status::permission_denied) return err(list.error());
    return err(status::io);
  }
  list_ = std::move(*list);
  connected_ = true;
  return {};
}

result<std::vector<driftwire::serve::root_info>> static_remote::roots() {
  MV_TRY(file_list list, list_now(nullptr));
  return list.roots;
}

result<std::vector<driftwire::serve::list_entry>> static_remote::list(const std::string&, const std::string&) {
  return err(status::unsupported_format);  // a static folder lists nothing; sync and the viewer never ask
}

result<driftwire::serve::stat_info> static_remote::stat(const std::string&, const std::string&) {
  return err(status::unsupported_format);
}

std::uint64_t static_remote::verified_bytes() const {
  namespace fs = std::filesystem;
  std::uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(verified_dir_, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const auto size = it->file_size(e2);
    if (!e2) total += size;
  }
  return total;
}

void static_remote::trim_verified(const std::string& keep) {
  namespace fs = std::filesystem;
  std::vector<std::pair<fs::file_time_type, std::pair<fs::path, std::uint64_t>>> files;
  std::uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(verified_dir_, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const auto size = it->file_size(e2);
    const auto when = it->last_write_time(e2);
    if (e2) continue;
    total += size;
    if (it->path().filename() != keep) files.push_back({when, {it->path(), size}});
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [when, f] : files) {
    if (total <= verified_budget_) break;
    // Never one another reader is downloading or reading: its lock is held.
    std::shared_ptr<std::timed_mutex> busy;
    {
      std::lock_guard lock(fetching_m_);
      std::string name = f.first.filename().string();
      if (const auto dot = name.find(".part."); dot != std::string::npos) name.resize(dot);
      if (auto it = fetching_.find(name); it != fetching_.end()) busy = it->second;
    }
    if (busy && !busy->try_lock()) continue;
    std::error_code e3;
    if (fs::remove(f.first, e3)) total -= f.second;
    if (busy) busy->unlock();
  }
}

result<std::string> static_remote::verified(const std::string& name, const listed_file& f, const cancel_flag* cancel) {
  namespace fs = std::filesystem;
  std::shared_ptr<std::timed_mutex> mine;
  {
    std::lock_guard lock(fetching_m_);
    auto& slot = fetching_[name];
    if (!slot) slot = std::make_shared<std::timed_mutex>();
    mine = slot;
  }
  // Another reader may be bringing the same file: wait for it, but not past our own cancel.
  while (!mine->try_lock_for(std::chrono::milliseconds(100))) {
    if (is_shut()) return err(status::io);
    if (cancel && cancel->stopped()) return err(status::cancelled);
  }
  std::unique_lock one(*mine, std::adopt_lock);

  const fs::path path = fs::path(verified_dir_) / name;
  std::error_code ec;
  // Written here only after its hash matched; a different size is an older list's file.
  if (fs::file_size(path, ec) == f.size && !ec) {
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // read now: the budget's order
    return path.string();
  }
  fs::create_directories(verified_dir_, ec);
  // Named by the hash the list gives, so what a cancelled reader kept is only ever resumed for
  // the same file.
  const fs::path part = path.string() + ".part." + f.sha256.substr(0, 16);

  // What arrived before: hashed again (it is on this phone's own disk), then the rest asked for.
  crypto_hash_sha256_state st;
  crypto_hash_sha256_init(&st);
  std::uint64_t have = 0;
  {
    std::ifstream kept(part, std::ios::binary);
    std::vector<char> buf(1 << 20);
    while (kept) {
      kept.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const auto n = static_cast<std::uint64_t>(kept.gcount());
      if (n == 0) break;
      crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()), n);
      have += n;
    }
  }
  if (have > f.size) {
    fs::remove(part, ec);
    crypto_hash_sha256_init(&st);
    have = 0;
  }
  std::ofstream out(part, std::ios::binary | std::ios::app);
  if (!out) return err(status::io);
  bool too_big = false;
  expected got;
  if (have < f.size) {
    const std::uint64_t from = have;
    got = fetch_(
        base_ + name, from, f.size - from,
        [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
          if (is_shut() || (cancel && cancel->stopped())) return false;
          if (from + offset != have || have + bytes.size() > f.size) {
            too_big = true;
            return false;
          }
          crypto_hash_sha256_update(&st, bytes.data(), bytes.size());
          out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          have += bytes.size();
          return static_cast<bool>(out);
        },
        cancel);
  }
  out.close();
  if (is_shut()) return err(status::io);
  if (!got || have < f.size) {
    // Kept for the next reader, who goes on from here; nothing is handed over yet.
    if ((cancel && cancel->stopped()) || (!got && got.error() == status::cancelled)) return err(status::cancelled);
    if (too_big) {
      fs::remove(part, ec);
      return err(status::corrupt);
    }
    return err(!got && got.error() == status::not_found ? status::not_found : status::io);
  }
  unsigned char h[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&st, h);
  if (too_big || !out || hex_of(h, sizeof h) != f.sha256) {
    fs::remove(part, ec);
    return err(status::corrupt);  // not the file the list signed for: refused, never shown
  }
  fs::rename(part, path, ec);
  if (ec) return err(status::io);
  one.unlock();
  trim_verified(path.filename().string());
  return path.string();
}

expected static_remote::read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                   std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel, bool) {
  if (is_shut() || !fetch_) return err(status::io);
  const auto name = flat_name(root, rel);
  if (!name) return err(status::not_found);  // nothing the publisher could have named
  MV_TRY(file_list list, list_now(cancel));

  if (root == list.catalogue_root && catalogue_signed(rel)) {
    // Sync checks these against the pinned key and the manifest's hashes before using them.
    bool stopped_by_sink = false;
    auto got = fetch_(
        base_ + *name, at, length,
        [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
          if (is_shut()) return false;  // Forget stops a read at its next chunk
          if (!sink(at + offset, bytes)) {  // the remote_file contract: offsets in the file
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
    std::lock_guard lock(m_);
    connected_ = false;
    return err(status::io);
  }

  // Anything else is read only from a whole copy whose hash the signed list gave.
  const auto listed = list.files.find(*name);
  if (listed == list.files.end()) return err(status::not_found);
  std::ifstream in;
  for (int attempt = 0; attempt < 2 && !in.is_open(); ++attempt) {
    // A trim for the budget may take it between the check and the open: then it is fetched again.
    MV_TRY(std::string path, verified(*name, listed->second, cancel));
    in.open(path, std::ios::binary);
  }
  if (!in) return err(status::io);
  const std::uint64_t size = listed->second.size;
  const std::uint64_t end = (length == UINT64_MAX || at > size || length > size - at) ? size : at + length;
  std::vector<std::uint8_t> buf(256 * 1024);
  in.seekg(static_cast<std::streamoff>(std::min(at, size)));
  for (std::uint64_t pos = at; pos < end;) {
    if (is_shut()) return err(status::io);
    if (cancel && cancel->stopped()) return err(status::cancelled);
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), end - pos));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in.gcount()) != n) return err(status::io);
    if (!sink(pos, std::span<const std::uint8_t>(buf.data(), n))) return err(status::cancelled);
    pos += n;
  }
  return {};
}

}  // namespace mv::mobile::sample
