// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Packages for the open add-on tests (plan/25): a publisher's key, a schema 2
// manifest, and the strict ZIP a `.mvaddon` is, written field by field so a
// test can break exactly one of them.
#pragma once

#include <sodium.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "addon/manifest.h"
#include "core/json.h"

namespace mv::test {

struct publisher_key {
  std::vector<std::uint8_t> pub = std::vector<std::uint8_t>(crypto_sign_PUBLICKEYBYTES);
  std::vector<std::uint8_t> sec = std::vector<std::uint8_t>(crypto_sign_SECRETKEYBYTES);
  publisher_key() {
    (void)sodium_init();
    crypto_sign_keypair(pub.data(), sec.data());
  }
  [[nodiscard]] std::string hex() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : pub) {
      out.push_back(kHex[b >> 4]);
      out.push_back(kHex[b & 0xF]);
    }
    return out;
  }
  [[nodiscard]] std::vector<std::uint8_t> sign(const std::string& text) const {
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(text.data()),
                         text.size(), sec.data());
    return sig;
  }
};

struct packed_file {
  std::string path;
  std::string text;
};

inline std::vector<std::uint8_t> bytes_from(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

inline const char* good_theme() {
  return R"({"schema":1,
"dark":{"canvas":"#1c1b1a","surface":"#262523","title":"#f2efe9","body":"#b9b4aa",
        "disabled":"#6f6b64","hairline":"#3a3835","accent":"#e0793a"},
"light":{"canvas":"#f4f1ea","surface":"#ffffff","title":"#1c1b1a","body":"#57534c",
         "disabled":"#9a958c","hairline":"#d6d1c6","accent":"#b5542d"},
"font":"Avenir Next"})";
}

struct manifest_options {
  std::string id = "acme.film-tones";
  std::string name = "Film Tones";
  std::string version = "1.2.0";
  std::string publisher = "Acme Pictures";
  std::string publisher_url = "https://acme.example";
  std::string update_url = "https://acme.example/film-tones.mvaddon";
  int schema = 2;
  int api_min = 1;
  int api_max = 1;
  std::string extra_key;          // a top-level key this host does not know, with value true
  std::string extra_contributes;  // the same, under "contributes"
  bool themes = true;             // list themes/dusk.json as a theme
};

inline std::string open_manifest_json(const publisher_key& k, const std::vector<packed_file>& files,
                                      const manifest_options& o = {}) {
  json::writer w;
  w.begin_object();
  w.key("schema").integer(o.schema);
  w.key("id").string(o.id);
  w.key("name").string(o.name);
  w.key("version").string(o.version);
  w.key("description").string("Warm and cool chrome themes.");
  w.key("licence").string("MIT");
  w.key("publisher").begin_object();
  w.key("name").string(o.publisher);
  w.key("url").string(o.publisher_url);
  w.key("key").string(k.hex());
  w.end_object();
  w.key("update_url").string(o.update_url);
  w.key("api").begin_object().key("min").integer(o.api_min).key("max").integer(o.api_max).end_object();
  std::int64_t total = 0;
  for (const packed_file& f : files) total += static_cast<std::int64_t>(f.text.size());
  w.key("installed_size").integer(total);
  w.key("files").begin_array();
  for (const packed_file& f : files) {
    w.begin_object();
    w.key("path").string(f.path);
    w.key("sha256").string(mv::addon::sha256_hex(bytes_from(f.text)));
    w.key("size").integer(static_cast<std::int64_t>(f.text.size()));
    w.key("licence").string("MIT");
    w.end_object();
  }
  w.end_array();
  w.key("contributes").begin_object();
  if (o.themes) {
    w.key("themes").begin_array();
    w.begin_object().key("id").string("dusk").key("name").string("Dusk").key("path").string(
        "themes/dusk.json").end_object();
    w.end_array();
  }
  if (!o.extra_contributes.empty()) w.key(o.extra_contributes).boolean(true);
  w.end_object();
  if (!o.extra_key.empty()) w.key(o.extra_key).boolean(true);
  w.end_object();
  return w.take();
}

// ---- the ZIP, by hand --------------------------------------------------------

struct zip_entry {
  std::string name;
  std::vector<std::uint8_t> bytes;
  std::uint16_t method = 0;
  std::uint16_t flags = 0;
  std::vector<std::uint8_t> extra;  // in both headers
};

struct zip_options {
  std::string comment;                       // after the end record
  std::vector<std::uint8_t> gap;             // between the last entry and the directory
  std::vector<std::uint8_t> trailing;        // after everything
  bool zip64_marker = false;                 // 0xFFFF entries, as a ZIP64 archive says
};

inline zip_entry stored(std::string name, std::vector<std::uint8_t> bytes) {
  zip_entry e;
  e.name = std::move(name);
  e.bytes = std::move(bytes);
  return e;
}

inline std::uint32_t crc32_of(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::uint8_t b : bytes) {
    crc ^= b;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  put16(out, v & 0xFFFF);
  put16(out, v >> 16);
}

inline std::vector<std::uint8_t> make_zip(const std::vector<zip_entry>& entries,
                                          const zip_options& o = {}) {
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> directory;
  for (const zip_entry& e : entries) {
    const auto at = static_cast<std::uint32_t>(out.size());
    const std::uint32_t crc = crc32_of(e.bytes);
    const auto size = static_cast<std::uint32_t>(e.bytes.size());
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, e.flags);
    put16(out, e.method);
    put16(out, 0);
    put16(out, 0x21);
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<std::uint32_t>(e.name.size()));
    put16(out, static_cast<std::uint32_t>(e.extra.size()));
    out.insert(out.end(), e.name.begin(), e.name.end());
    out.insert(out.end(), e.extra.begin(), e.extra.end());
    out.insert(out.end(), e.bytes.begin(), e.bytes.end());

    put32(directory, 0x02014b50);
    put16(directory, 20);
    put16(directory, 20);
    put16(directory, e.flags);
    put16(directory, e.method);
    put16(directory, 0);
    put16(directory, 0x21);
    put32(directory, crc);
    put32(directory, size);
    put32(directory, size);
    put16(directory, static_cast<std::uint32_t>(e.name.size()));
    put16(directory, static_cast<std::uint32_t>(e.extra.size()));
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put32(directory, 0);
    put32(directory, at);
    directory.insert(directory.end(), e.name.begin(), e.name.end());
    directory.insert(directory.end(), e.extra.begin(), e.extra.end());
  }
  out.insert(out.end(), o.gap.begin(), o.gap.end());
  const auto directory_at = static_cast<std::uint32_t>(out.size());
  out.insert(out.end(), directory.begin(), directory.end());
  const std::uint32_t count = o.zip64_marker ? 0xFFFFu : static_cast<std::uint32_t>(entries.size());
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, count);
  put16(out, count);
  put32(out, static_cast<std::uint32_t>(directory.size()));
  put32(out, directory_at);
  put16(out, static_cast<std::uint32_t>(o.comment.size()));
  out.insert(out.end(), o.comment.begin(), o.comment.end());
  out.insert(out.end(), o.trailing.begin(), o.trailing.end());
  return out;
}

// A whole package: the files, the manifest that lists them, its signature.
inline std::vector<std::uint8_t> make_package(const publisher_key& k,
                                              const std::vector<packed_file>& files,
                                              const manifest_options& o = {}) {
  const std::string manifest = open_manifest_json(k, files, o);
  std::vector<zip_entry> entries;
  entries.push_back(stored("manifest.json", bytes_from(manifest)));
  entries.push_back(stored("manifest.json.sig", k.sign(manifest)));
  for (const packed_file& f : files) entries.push_back(stored(f.path, bytes_from(f.text)));
  return make_zip(entries);
}

inline std::vector<packed_file> theme_files() {
  return {packed_file{"themes/dusk.json", good_theme()}};
}

}  // namespace mv::test
