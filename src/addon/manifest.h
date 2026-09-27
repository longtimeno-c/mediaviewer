// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Add-on manifests: signed, verified, then loaded (plan/18 "Add-ons").
//
// manifest.json is signed with the update-manifest key (tools/package/
// update-signing.md): a 64-byte raw Ed25519 signature, detached, over the
// exact bytes, in manifest.json.sig. It lists every file of the add-on with
// its SHA-256, size and licence, the host API range the add-on supports, and
// the archive it is downloaded as. Nothing below reads an unauthenticated
// byte: the signature is checked before the JSON is parsed. Portable (D9).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mv::addon {

inline constexpr int kManifestSchema = 1;

#if defined(_WIN32)
inline constexpr std::string_view kPlatform = "win-x64";
#elif defined(__APPLE__)
// One universal (arm64 + x86_64) add-on, like the app (D9 amended 2026-09-24).
inline constexpr std::string_view kPlatform = "macos";
#else
inline constexpr std::string_view kPlatform = "linux-test";
#endif

struct manifest_file {
  std::string path;     // relative, '/'-separated, no "..", no leading '/'
  std::string sha256;   // 64 lowercase hex
  std::uint64_t size = 0;
  std::string licence;  // SPDX
};

struct manifest {
  int schema = 0;
  std::string id;       // "import"
  std::string name;     // "Import"
  std::string version;  // x.y.z
  std::string platform; // kPlatform
  std::uint32_t host_api_min = 0;
  std::uint32_t host_api_max = 0;
  std::uint64_t installed_size = 0;
  std::string native;   // the shared library, one of `files`
  std::string chrome;   // the chrome assembly / bundle entry, one of `files` (may be a bundle dir)
  manifest_file archive;  // the download: name, sha256, size (licence unused)
  std::vector<manifest_file> files;
};

enum class rejection : std::uint8_t {
  none = 0,
  key_not_configured,
  missing_signature,
  bad_signature,
  malformed,
  unsupported_schema,
  wrong_platform,
  unsafe_path,
  file_missing,
  file_mismatch,
  unexpected_file,   // a file in the folder the manifest does not list
  needs_update,      // host API outside the add-on's range
};

[[nodiscard]] const char* rejection_name(rejection r) noexcept;

struct decision {
  rejection why = rejection::malformed;
  manifest m;
  [[nodiscard]] bool trusted() const noexcept { return why == rejection::none; }
};

// The pinned public key (32 bytes). Same key as MediaViewer.Updater's
// UpdateKeys.ProductionPublicKeyHex; tools/package/test_addon_pack.py checks
// they agree.
[[nodiscard]] std::span<const std::uint8_t, 32> pinned_public_key() noexcept;

// Signature, then shape, then platform, then the host API range.
// `host_api` is the running host's MV_ADDON_HOST_API.
// `platform` is this build's (kPlatform) except in the packing tool's
// cross-check (tools/addon-verify), which verifies other platforms' packages.
[[nodiscard]] decision check_manifest(std::span<const std::uint8_t> manifest_bytes,
                                      std::span<const std::uint8_t> signature,
                                      std::span<const std::uint8_t> public_key,
                                      std::uint32_t host_api,
                                      std::string_view platform = kPlatform);

// A relative path that stays inside its folder on every OS.
[[nodiscard]] bool safe_relative_path(std::string_view path) noexcept;

// SHA-256 of a whole file, lowercase hex; empty on read failure.
[[nodiscard]] std::string sha256_file(const std::string& path);
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

// Every listed file present with its size and SHA-256, and nothing else in
// `dir` besides manifest.json and manifest.json.sig.
[[nodiscard]] rejection verify_files(const std::string& dir, const manifest& m);

}  // namespace mv::addon
