// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The NAS service updating itself (docs/plans/nas-setup.md §6, owner call 3,
// 2026-11-12). An internet-facing service cannot wait for someone to notice a
// notice, and DSM checks no package signature, so the service checks its own:
//
//   The offer   <site>/nas/latest.json: {"version", "arch": {"x86_64": {"url",
//               "sha256", "size", "sig"}, "aarch64": {...}}}. Each arch's `sig`
//               is Ed25519, by the key pinned in this binary, over
//               signed_message(): the version, the arch, the URL, the hash and
//               the size, each with its length. Nothing in the file is
//               believed before its signature is.
//   Staging     the binary is fetched to <var>/stage/, its size and SHA-256
//               checked, made executable, and run once with --self-test; only
//               then is it renamed to <var>/bin/driftwire, the one before kept
//               as driftwire.prev, and a trial started (<var>/state/trial).
//   Rollback    start-stop-status counts the starts of a trial; three without
//               the service clearing it (trial_passed, after five minutes up)
//               put driftwire.prev back and skip that version.
//   Never       touches <var>/serve (keys, serve.json, devices: pairings
//               survive), runs anything it fetched but the self-test, or sends
//               more than its address, version and architecture (rule 6).
//
// Pure apart from the files under <var> and the injected fetch and self-test,
// so the tests need no network and no Synology. Portable (D9).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::driftwire::cli::nas_update {

struct offer {
  std::string version;
  std::string arch;
  std::string url;
  std::string sha256;  // lowercase hex
  std::uint64_t size = 0;
};

// The bytes a signature covers.
[[nodiscard]] std::string signed_message(const offer& o);

// latest.json's offer for `arch`, signed by `pinned` and newer than `current`.
// status::not_found: no newer version for this arch. status::permission_denied:
// the signature does not hold. status::corrupt: not an offer.
[[nodiscard]] result<offer> verify_latest(std::string_view json, std::string_view arch,
                                          std::span<const std::uint8_t, 32> pinned, std::string_view current);

struct io {
  // Fetches `url` whole into `to` (a new file); any failure is an error.
  std::function<expected(const std::string& url, const std::string& to)> fetch;
  // Runs the staged binary once with --self-test; true when it says all is well.
  std::function<bool(const std::string& binary)> self_test;
};

// Stages, checks and installs `o` under `var` (the package's var/). On any
// failure nothing under var/bin changes. status::corrupt: the download is
// not what was signed. status::cancelled: the self-test failed.
[[nodiscard]] expected install(const offer& o, const std::string& var, const io& io);

// The service, five minutes up on a trial version: the trial is over.
void trial_passed(const std::string& var);

// A version rolled back from is not offered again until a newer one is.
[[nodiscard]] bool skipped(const std::string& var, std::string_view version);

// ---- the daily check (run by `driftwire serve` in the package) ----

// The service's exit code after installing: the package's start-stop-status
// starts the new binary at once (and counts it as a start of the trial).
inline constexpr int kRestartForUpdate = 75;
// A trial is over once the service has been up this long.
inline constexpr std::chrono::minutes kTrialSettles{5};

// This build's architecture as latest.json names it; "" for one the NAS
// package is not built for (then nothing is checked).
[[nodiscard]] std::string_view this_arch();

// The pinned key, from 64 hex digits (the build's MV_NAS_UPDATE_KEY); nullopt
// when the build has none (a build of one's own: it never updates itself).
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> pinned_key(std::string_view hex);

// How long until the next check: the first a while after starting (so a
// restart loop does not hammer the site), then daily, each with jitter.
[[nodiscard]] std::chrono::seconds next_delay(bool first, std::uint32_t random);

enum class outcome : std::uint8_t {
  nothing,    // no newer version for this arch, or it was rolled back from
  installed,  // staged, checked, self-tested and in var/bin: restart now
  refused,    // latest.json was not signed by the pinned key, or not an offer
  failed,     // the site, the download or the self-test failed: try tomorrow
};
struct check_io {
  // latest.json's text; nullopt when it could not be fetched.
  std::function<std::optional<std::string>(const std::string& url)> get_json;
  io install;
};
// One check: <site>/nas/latest.json, verified, and installed when newer.
[[nodiscard]] outcome check_once(const std::string& site, const std::string& var, std::string_view arch,
                                 std::span<const std::uint8_t, 32> pinned, std::string_view current, const check_io& c);

}  // namespace mv::driftwire::cli::nas_update
