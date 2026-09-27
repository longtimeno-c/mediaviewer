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
  std::string native;   // the shared library, one of `files`; empty for a piece
  std::string chrome;   // the chrome assembly / bundle entry, one of `files` (may be a bundle dir); empty for a piece
  // Milestone H (plan/17 "per-piece Install/Remove"): a piece is data or
  // provider libraries the parent add-on loads itself ("ai-faces" is
  // part_of "ai"). A piece has no native entry and no chrome; the parent
  // reaches its verified folder through the host table's piece_dir.
  std::string part_of;
  // Optional CPU architecture ("arm64", "x86_64"); empty means every one the
  // platform runs. The Mac AI pack is arm64-only: ONNX Runtime ships no
  // x86_64 macOS build (plan/17, 2026-09-26).
  std::string arch;
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
  over_ceiling,      // the family would exceed its installed-size ceiling (plan/17: 3 GB)
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
// `host_api` is the running host's MV_ADDON_HOST_API; the host also serves
// every older table layout down to MV_ADDON_HOST_API_OLDEST (appended-only),
// so an add-on is accepted when its range meets [oldest, host_api].
// `platform` is this build's (kPlatform) except in the packing tool's
// cross-check (tools/addon-verify), which verifies other platforms' packages.
[[nodiscard]] decision check_manifest(std::span<const std::uint8_t> manifest_bytes,
                                      std::span<const std::uint8_t> signature,
                                      std::span<const std::uint8_t> public_key,
                                      std::uint32_t host_api,
                                      std::string_view platform = kPlatform,
                                      std::uint32_t host_api_oldest = 1);

// The table version an add-on is loaded with: the newest both sides know.
[[nodiscard]] std::uint32_t negotiated_host_api(const manifest& m, std::uint32_t host_api) noexcept;

// This process's CPU architecture as a manifest spells it.
[[nodiscard]] std::string_view current_arch() noexcept;

// Installed-size ceilings per family (plan/17: the AI pack's Core, one vendor
// piece and Faces together stay under 3 GB). 0 = no ceiling.
[[nodiscard]] std::uint64_t family_ceiling(std::string_view family) noexcept;
// The family an add-on or piece belongs to: its part_of, else its own id.
[[nodiscard]] inline const std::string& family_of(const manifest& m) noexcept {
  return m.part_of.empty() ? m.id : m.part_of;
}

// A relative path that stays inside its folder on every OS.
[[nodiscard]] bool safe_relative_path(std::string_view path) noexcept;

// SHA-256 of a whole file, lowercase hex; empty on read failure.
[[nodiscard]] std::string sha256_file(const std::string& path);
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

// Every listed file present with its size and SHA-256, and nothing else in
// `dir` besides manifest.json and manifest.json.sig.
// Hashed once per process per folder; a concurrent call for the same folder
// waits for the one hashing it and takes its answer.
[[nodiscard]] rejection verify_files(const std::string& dir, const manifest& m);
// Verified `from_dir` was renamed to `to_dir`: carry the verification over,
// so an install does not hash the same bytes a second time.
void note_verified_move(const std::string& from_dir, const std::string& to_dir);

}  // namespace mv::addon
