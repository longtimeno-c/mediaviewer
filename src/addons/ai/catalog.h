// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue a phone reads (docs/plans/mobile.md §5, §6): one generation
// built beside the index from a read snapshot, then delivered to a NAS folder
// so a reader sees the previous generation or this one, never a mix.
//
// A generation is trusted through its Ed25519 signature over the library id,
// the epoch, the generation and the manifest's hash (verify_current). Not here
// yet: thumbnails, people, and moving the key to another desktop.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"

namespace mv::ai::catalog {

inline constexpr int kFormat = 1;
inline constexpr std::size_t kPublicKeyBytes = 32;
inline constexpr std::size_t kSecretKeyBytes = 64;
inline constexpr std::size_t kSignatureBytes = 64;

// A new Ed25519 key pair: {public, secret}.
[[nodiscard]] std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> make_keypair();
// The public half of a secret key; empty when `secret` is the wrong size.
[[nodiscard]] std::vector<std::uint8_t> public_of(std::span<const std::uint8_t> secret);
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
// What the person compares on the desktop and the phone: the start of the key's hash.
[[nodiscard]] std::string fingerprint(std::span<const std::uint8_t> public_key);

struct current_info {
  std::string library_id;
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  std::string manifest_sha256;
};
// A current.json, believed only if `public_key` signed exactly these fields.
// status::permission_denied when it did not; status::corrupt when unreadable.
[[nodiscard]] result<current_info> verify_current(std::string_view text, std::span<const std::uint8_t> public_key);

// One indexed folder as a phone knows it: no path of this machine.
struct root_map {
  std::int64_t id = 0;   // in the index
  std::string path;      // here, on this machine; never written out
  std::string uuid;      // stable across generations
  std::string share;     // the share's name, no server and no user
  std::string sub;       // under the share, '/'-separated, "" at its top
};

struct control {
  std::function<bool()> cancelled;
  std::function<void(double)> report;  // 0..1 of this step
  std::function<void()> yield;         // between tables, while the viewer needs the machine
};

struct build_options {
  std::string index_db;      // the live index, read through its own connection
  std::string out_dir;       // created; removed again on failure
  std::string library_id;
  std::uint64_t epoch = 1;
  std::uint64_t generation = 0;
  std::int64_t created_unix = 0;
  std::string active_spec;
  std::string labels_file;   // copied when it exists
  std::string settings_json; // what a reader needs of this machine's settings
  std::vector<root_map> roots;
};

struct build_counts {
  std::uint64_t assets = 0;
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  int schema = 0;
};

[[nodiscard]] result<build_counts> build(const build_options& o, const control& c);

struct deliver_options {
  std::string gen_dir;      // a finished generation from build()
  std::string destination;  // the chosen folder; its .mediaviewer/ is ours
  std::string library_id;
  std::uint64_t generation = 0;
  std::int64_t now_unix = 0;
  std::uint64_t epoch = 1;
  std::vector<std::uint8_t> public_key;
  std::vector<std::uint8_t> secret_key;
  std::int64_t keep_seconds = 7 * 24 * 3600;  // a phone may still be copying an older one
  // Verified copy to a new file in an existing folder.
  std::function<expected(const std::string& from, const std::string& to)> copy;
  std::function<expected(const std::string& dir)> make_directories;
};

// status::permission_denied: that folder holds another library's catalogue,
// or this library under another key.
[[nodiscard]] expected deliver(const deliver_options& o, const control& c);

// What the folder has now: the newest generation, 0 when it has none.
// status::permission_denied as above; status::io when it cannot be read.
[[nodiscard]] result<std::uint64_t> published_generation(const std::string& destination,
                                                         const std::string& library_id,
                                                         std::span<const std::uint8_t> public_key);

}  // namespace mv::ai::catalog
