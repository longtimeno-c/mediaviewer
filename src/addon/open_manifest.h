// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Manifest schema 2: an add-on from anyone (plan/23 "The manifest").
//
// Schema 1 (manifest.h) is MediaViewer's own add-ons, signed with the pinned
// release key. Schema 2 is signed by its PUBLISHER, whose public key is a
// field of the manifest itself. That signature says two things and no more:
// the files are the ones the publisher packed, and a later version comes from
// the same publisher. It does not say who the publisher is, and nothing here
// pretends it does; the person installing is shown the name and the key's
// fingerprint and decides (open_store.h).
//
// What a schema 2 add-on may hold is set by the contribution API it declares
// (`api`), not by what the package happens to carry. API 1 is data only:
// themes. A manifest that names code (`native`, `chrome`, `scripts`) is
// refused by a host that does not run third-party code, whatever its range.
// Portable (D9).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/manifest.h"

namespace mv::addon {

inline constexpr int kOpenManifestSchema = 2;

// The contribution API this host serves (plan/23 "Contribution API").
//   1  themes.
inline constexpr std::uint32_t kContributionApi = 1;
inline constexpr std::uint32_t kContributionApiOldest = 1;

struct theme_ref {
  std::string id;    // unique in the add-on: [a-z0-9-]{1,32}
  std::string name;  // shown in Settings
  std::string path;  // one of `files`, a theme JSON (theme.h)
};

struct open_manifest {
  std::string id;           // "acme.film-tones": publisher.name, dotted
  std::string name;
  std::string version;      // x.y.z
  std::string description;  // one line for the consent sheet and Settings; may be empty
  std::string licence;      // SPDX of the add-on as a whole
  std::string publisher_name;
  std::string publisher_url;  // https, or empty
  std::string publisher_key;  // 64 lowercase hex: the Ed25519 public key that signed this
  std::string update_url;     // https link to the newest package, or empty
  std::uint32_t api_min = 0;
  std::uint32_t api_max = 0;
  std::uint64_t installed_size = 0;  // the sum of `files`
  std::vector<manifest_file> files;
  std::vector<theme_ref> themes;
};

struct open_decision {
  rejection why = rejection::malformed;
  open_manifest m;
  [[nodiscard]] bool ok() const noexcept { return why == rejection::none; }
};

// Shape, then the publisher's signature over the exact bytes, then policy.
// A manifest whose range starts above `api` is needs_update with `m` filled,
// so the app can say which add-on needs a newer MediaViewer.
[[nodiscard]] open_decision check_open_manifest(std::span<const std::uint8_t> manifest_bytes,
                                                std::span<const std::uint8_t> signature,
                                                std::uint32_t api = kContributionApi,
                                                std::uint32_t api_oldest = kContributionApiOldest);

// "publisher.name": two or more dot-separated parts of [a-z0-9-], each
// starting and ending with a letter or digit, 64 characters at most. The
// dot keeps these apart from MediaViewer's own ids ("import", "ai-faces"),
// `mediaviewer.` is kept for MediaViewer, and no part is a name Windows
// reserves for a device (the id is a folder on disk).
[[nodiscard]] bool valid_open_id(std::string_view id) noexcept;

// An https URL with a host and nothing a person could not read: no
// credentials, no control characters, no spaces, 2048 characters at most.
[[nodiscard]] bool valid_https_url(std::string_view url) noexcept;

// A key as people compare it: the first 8 bytes of SHA-256(key), as four
// groups of four hex digits ("3f9a-02c1-77de-b410").
[[nodiscard]] std::string key_fingerprint(std::string_view key_hex);

}  // namespace mv::addon
