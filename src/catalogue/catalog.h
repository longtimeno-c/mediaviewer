// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue a phone reads, generic half (docs/plans/mobile.md §5, §6;
// docs/plans/phone-in-the-app.md): the publisher's keys, current.json and its
// signature, delivering a finished generation to a NAS folder so a reader
// sees the previous generation or this one, never a mix, retention of shared
// files, and the readers of the thumbnail formats. Building a generation from
// the index is Local search's (addons/ai/catalog.h). Moving the key to
// another desktop is catalog_chain.h.
//
// A generation is trusted through its Ed25519 signature over the library id,
// the epoch, the generation and the manifest's hash (verify_current), and the
// manifest names the hash of every file a reader will use, shared packs and
// model files included. Portable (D9); the namespace is still Local search's,
// where this began.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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

// What a retained generation no longer names stays this long (§5 rule 4).
inline constexpr std::int64_t kUnusedSeconds = 30 * 24 * 3600;
// A pack under this is folded into the next one, so a superseded small pack is
// routine, not a loss: it goes as soon as no retained generation needs it.
inline constexpr std::uint64_t kFoldBytes = 16u << 20;
inline constexpr std::int64_t kRollingUnusedSeconds = 2 * 3600;

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

// The whole current.json for `c`, signed with `secret_key`; "" when it cannot sign.
[[nodiscard]] std::string signed_current_json(const current_info& c, std::span<const std::uint8_t> secret_key);

struct control {
  std::function<bool()> cancelled;
  std::function<void(double)> report;  // 0..1 of this step
  std::function<void()> yield;         // between tables, while the viewer needs the machine
};

// The files a reader takes from the share beside the generation, content
// addressed and kept across generations.
struct shared_file {
  std::string source;  // here; deliver() copies it when the share lacks it
  std::string rel;     // under .mediaviewer/: "thumbs/t-….pack", "models/<spec>/…"
  std::uint64_t size = 0;
  std::string sha256;
};

struct deliver_options {
  std::string gen_dir;      // a finished generation from build()
  std::string destination;  // the chosen folder; its .mediaviewer/ is ours
  std::string library_id;
  std::uint64_t generation = 0;
  std::int64_t now_unix = 0;
  std::uint64_t epoch = 1;
  std::vector<std::uint8_t> public_key;
  std::vector<std::uint8_t> secret_key;
  std::int64_t keep_seconds = 2 * 3600;  // an older generation, once its successor has been out this long
  std::vector<shared_file> shared;  // copied first; those the share already has are left
  std::string retention_file;       // here: when each unused shared file was last named
  std::string arrivals_file;        // here: when each generation became current, by this clock
  // Verified copy to a new file in an existing folder.
  std::function<expected(const std::string& from, const std::string& to)> copy;
  std::function<expected(const std::string& dir)> make_directories;
};

// status::permission_denied: that folder holds another library's catalogue,
// or this library under another key. status::not_found: a shared file is on
// neither side (the share lost it and it was not kept here).
[[nodiscard]] expected deliver(const deliver_options& o, const control& c);

// The shared files a built generation names, from the list written beside it
// (<gen_dir>.shared.json: [{source, rel, size}]); empty when unreadable.
[[nodiscard]] std::vector<shared_file> read_shared_list(const std::string& file);

// What the folder has now: the newest generation, 0 when it has none.
// status::permission_denied as above; status::io when it cannot be read.
[[nodiscard]] result<std::uint64_t> published_generation(const std::string& destination,
                                                         const std::string& library_id,
                                                         std::span<const std::uint8_t> public_key);

// Shared files no retained generation names and unused for 30 days go (§5
// rule 4). `state_file` (here) remembers since when each has been unused; an
// unreadable manifest deletes nothing. Failing to tidy never fails a publish.
void retain_shared(const std::string& base, const std::string& state_file, std::int64_t now_unix,
                   std::int64_t unused_seconds = kUnusedSeconds);

// The thumbnail formats, written by Local search's build() and read below.
inline constexpr char kTinyMagic[8] = {'M', 'V', 'T', 'I', 'N', 'Y', '0', '1'};
inline constexpr char kIdxMagic[8] = {'M', 'V', 'T', 'I', 'D', 'X', '0', '1'};
inline constexpr std::size_t kPackNameBytes = 40;  // "t-" + 32 hex + ".pack", NUL padded
inline constexpr std::size_t kTinyRow = 24;        // asset, offset, length, 0
inline constexpr std::size_t kIdxRow = 24;         // asset, pack, length, offset

// The reader's side, in the format build() writes (little-endian, sorted by
// asset id, so a phone can binary-search a mapping). A bad file is an error.
struct thumb_location {
  std::string pack;
  std::uint64_t offset = 0;
  std::uint32_t length = 0;
};
[[nodiscard]] bool valid_pack_name(std::string_view name);
// Whether a tile lies inside a pack of this size: check before reading it.
[[nodiscard]] bool within_pack(const thumb_location& at, std::uint64_t pack_size);
[[nodiscard]] result<std::optional<thumb_location>> find_thumb(std::span<const std::uint8_t> thumbs_idx,
                                                               std::int64_t asset);
[[nodiscard]] result<std::optional<std::span<const std::uint8_t>>> find_tiny(std::span<const std::uint8_t> tiny_pack,
                                                                             std::int64_t asset);

}  // namespace mv::ai::catalog
