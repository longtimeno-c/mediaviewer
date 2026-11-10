// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Moving a library's signing key to another desktop (docs/plans/mobile.md §5
// rule 5), and the local copy of a generation a nearby phone reads (§7.5).
//
// library.json holds the current key, its epoch, and a chain of records: each
// is signed by the key it moves away from, names the next key and raises the
// epoch. A phone walks that chain from the key it pinned. It never trusts a
// new key because a file on the share says so: with the old desktop gone there
// is nothing to sign with, and each phone scans a new code.
//
// Portable (D9); no thread of its own, files only.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/ai/catalog.h"
#include "core/result.h"

namespace mv::ai::catalog {

struct library_info {
  std::string library_id;
  std::string public_key;  // hex
  std::uint64_t epoch = 1;
  bool restarted = false;  // the last key came with no signature from the one before
};
// not_found when the folder has no library.json; corrupt when it cannot be read.
[[nodiscard]] result<library_info> read_library(const std::string& destination);

struct followed_key {
  std::vector<std::uint8_t> public_key;
  std::uint64_t epoch = 0;
};
// The key that now signs this library, reached from `pinned` at `pinned_epoch`
// through records each signed by the key before. status::permission_denied:
// the chain does not lead from the pinned key to library.json's (a new code
// must be scanned). status::corrupt: not a library.json.
[[nodiscard]] result<followed_key> follow_chain(std::string_view library_json, std::string_view library_id,
                                                std::span<const std::uint8_t> pinned, std::uint64_t pinned_epoch);

// The old desktop's act: a signed record naming `to_public`, epoch + 1.
// status::permission_denied: `secret` is not the key the library has now.
[[nodiscard]] result<library_info> hand_over(const std::string& destination, std::span<const std::uint8_t> secret,
                                             std::span<const std::uint8_t> to_public);
// The old desktop is gone: the new key with no record. Every phone must scan.
[[nodiscard]] result<library_info> restart_publisher(const std::string& destination,
                                                     std::span<const std::uint8_t> to_public);

// The new desktop asks, on the share; the old one sees it and its person decides.
struct takeover_request {
  std::string public_key;  // hex
  std::string name;        // that desktop's name
};
[[nodiscard]] expected request_takeover(const std::string& destination, std::span<const std::uint8_t> public_key,
                                        const std::string& name);
// not_found: nobody is asking.
[[nodiscard]] result<takeover_request> read_takeover(const std::string& destination);
void clear_takeover(const std::string& destination);

// `dir` becomes what a nearby phone reads, the same layout as .mediaviewer/:
// current.json signed for this generation, gen/NNNNNN/, the text tower. The
// generation goes in whole before current.json names it; the one before stays
// for a phone still copying it. Thumbnail packs are the NAS's (§7.5).
struct nearby_options {
  std::string dir;
  std::string gen_dir;  // a finished generation from build()
  std::string library_id;
  std::uint64_t epoch = 1;
  std::uint64_t generation = 0;
  std::vector<std::uint8_t> public_key, secret_key;
  std::vector<shared_file> shared;  // only models/ ones are taken
  std::string library_json;         // as on the share; written as it is
};
[[nodiscard]] expected prepare_nearby(const nearby_options& o);

// The QR (§7.4): the string a phone reads, `mediaviewer://pair#` then
// base64url (no padding) of the JSON.
struct pair_payload {
  std::string library_id, library_name;
  std::vector<std::uint8_t> publisher_key;
  std::string service_local, service_public;
  std::string service_identity_hex, access_key_hex;
  std::string code;
  std::int64_t expires = 0;
};
[[nodiscard]] std::string pair_url(const pair_payload& p);


}  // namespace mv::ai::catalog
