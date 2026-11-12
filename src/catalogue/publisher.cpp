// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/publisher.h"

#include <sodium.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "catalogue/catalog.h"
#include "core/json.h"

namespace mv::ai::catalog {

namespace fs = std::filesystem;

namespace {

fs::path fs_path(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string trimmed(std::string path) {
#if defined(_WIN32)
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
#else
  while (path.size() > 1 && path.back() == '/') path.pop_back();
#endif
  return path;
}

// Whole, on the disk, then renamed in: a crash leaves no file or this one.
bool write_durable(const std::string& path, const std::string& text) {
  const std::string part = path + ".part";
  bool ok = false;
#if defined(_WIN32)
  FILE* f = _wfopen(fs_path(part).c_str(), L"wb");
#else
  FILE* f = std::fopen(part.c_str(), "wb");
#endif
  if (f) {
    ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(::fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
  }
  std::error_code ec;
  if (!ok) {
    fs::remove(fs_path(part), ec);
    return false;
  }
  fs::rename(fs_path(part), fs_path(path), ec);
  return !ec;
}

}  // namespace

result<std::vector<std::uint8_t>> publisher_key(driftwire::secrets::store* store, driftwire::secrets::store* legacy,
                                                bool make) {
  if (!store) return err(status::unsupported_format);
  if (auto kept = store->get(kPublisherKeyName)) return std::move(*kept);
  if (legacy) {
    if (auto old = legacy->get(kPublisherKeyName); old && old->size() == kSecretKeyBytes) {
      // Copied, and left where it was in this release: a Local search from
      // before the move reads it there and must sign with the same key, and a
      // second reader (the app and Local search at once) finds it either way.
      // A store that will not take it keeps signing with the old one.
      (void)store->put(kPublisherKeyName, *old);
      return std::move(*old);
    }
  }
  if (!make) return err(status::not_found);
  // Looked at again just before making one: the app and Local search may both
  // have missed it a moment ago, and the first to make it wins below.
  if (auto kept = store->get(kPublisherKeyName)) return std::move(*kept);
  auto secret = make_keypair().second;
  if (secret.empty()) return err(status::io);  // no usable libsodium: never store an empty key
  if (!store->put(kPublisherKeyName, secret)) return err(status::io);
  auto back = store->get(kPublisherKeyName);
  if (!back) return err(status::io);
  // Another made one at the same moment and its put landed last: that is the
  // key now, for both. (Nothing has signed with this one yet.)
  return std::move(*back);
}

std::string new_library_id() {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  unsigned char raw[16];
  randombytes_buf(raw, sizeof raw);
  return to_hex(std::span<const std::uint8_t>(raw, sizeof raw));
}

expected start_library(const std::string& destination, const std::string& library_id,
                       std::span<const std::uint8_t> public_key) {
  if (library_id.empty() || public_key.size() != kPublicKeyBytes) return err(status::invalid_arg);
  std::error_code ec;
  if (!fs::is_directory(fs_path(destination), ec)) return err(status::io);
  const std::string base = trimmed(destination) + "/.mediaviewer";
  const std::string file = base + "/library.json";
  if (fs::exists(fs_path(file), ec)) {
    std::ifstream in(fs_path(file), std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    const auto doc = json::parse(s.str(), 4);
    const std::string* id = doc ? doc->str("library_id") : nullptr;
    const std::string* key = doc ? doc->str("public_key") : nullptr;
    if (!id || !key) return err(status::io);  // there and unreadable: never written over
    if (*id != library_id || *key != to_hex(public_key)) return err(status::permission_denied);
    return {};
  }
  fs::create_directories(fs_path(base), ec);
  if (ec) return err(status::io);
  json::writer w;
  w.begin_object();
  w.key("format").integer(kFormat);
  w.key("library_id").string(library_id);
  w.key("public_key").string(to_hex(public_key));
  w.end_object();
  if (!write_durable(file, w.str())) return err(status::io);
  return {};
}

}  // namespace mv::ai::catalog
