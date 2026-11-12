// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The publisher's identity (docs/plans/phone-in-the-app.md §2.2–2.3): the
// Ed25519 key that signs a library's catalogue, and the library's start on the
// share. Both belong to the app's Phone access, so a library can be claimed
// and paired before anything is indexed; Local search signs with the same key.
//
// The key lives only in the OS's secret store, under kPublisherService. Until
// 2026-11-12 Local search kept it under kLegacyService: the first read takes
// it from there (written here, read back, then removed there), so a library
// published before keeps its key and every paired phone keeps trusting it.
// Worker threads only: the store may wait on the OS. Portable (D9).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"
#include "wire/secrets.h"

namespace mv::ai::catalog {

inline constexpr const char* kPublisherService = "MediaViewer Phone access";
inline constexpr const char* kLegacyService = "MediaViewer Local search";
inline constexpr const char* kPublisherKeyName = "catalogue-signing-key";

// The secret key, from `store`, else moved from `legacy` (may be null), else
// made when `make`. status::not_found: none and not `make`. status::io: a key
// could not be kept (a store that does not give back what it took is no store).
// status::unsupported_format: no store at all.
[[nodiscard]] result<std::vector<std::uint8_t>> publisher_key(driftwire::secrets::store* store,
                                                              driftwire::secrets::store* legacy, bool make);

// 32 hex digits from the OS's randomness: a new library's id.
[[nodiscard]] std::string new_library_id();

// The library's start on the share: `destination`/.mediaviewer/library.json
// naming `library_id` and `public_key`, written whole (flushed, then renamed
// in) when there is none. One already there that names both: nothing to do.
// status::permission_denied: it names another library or another key (read
// it with read_library to say which). status::io: the folder cannot be written.
[[nodiscard]] expected start_library(const std::string& destination, const std::string& library_id,
                                     std::span<const std::uint8_t> public_key);

}  // namespace mv::ai::catalog
