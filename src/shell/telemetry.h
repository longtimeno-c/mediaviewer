// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PR 8 telemetry (plan/13 Part 3). Default off, opt-in once, honestly.
//
// This app is pointed at people's private photos, so the design here starts
// from rule 6 and works backwards: nothing about a user's files leaves the
// machine. The strongest way to hold that line is not a careful caller - it is
// a sink that cannot express a path.
//
//   * There is no free-text field. An event is an id from a fixed table plus
//     up to four named integers, and a whitelisted short string chosen from a
//     fixed vocabulary (a format name, a decoder name, a GPU vendor). None of
//     those can carry a filename, a folder, EXIF, or a hash of any of them.
//   * `record()` returns false and drops the event if the caller tries
//     anything else. `looks_like_user_data()` is the gate, and it is pure so
//     the tests can hammer it.
//   * While consent is off, `record()` is inert before it looks at anything:
//     no id is generated, no file is opened, no buffer grows.
//
// There is no upload endpoint in PR 8, exactly as there is none for crash
// reports (plan/13 Part 2 "no URL"). Consented events are appended to a local
// spool so the schema is exercised and reviewable on a real machine; the
// sender is a later change, and it reads this spool.
//
// Windows host only (D9): lives in shell/, like settings and update_guard.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::shell::telemetry {

// Bits in the chrome settings word (chrome_flags_args), above the view bits and
// the updater's. Mirrored by the island as SettingFlag.Telemetry / .Asked.
inline constexpr std::int32_t kChromeFlagTelemetry = 1 << 9;
// Whether the first-run choice has been made at all. The island shows the
// opt-in screen exactly while this is clear; it is never a "consent" value.
inline constexpr std::int32_t kChromeFlagTelemetryAsked = 1 << 10;

// The whole vocabulary. plan/13 "What is genuinely worth collecting" - and
// nothing else. Adding a row is a deliberate change with a schema-check run,
// not an afterthought at a call site.
enum class event : std::int32_t {
  // Decode failures by format (+ camera model, which is whitelisted and
  // extracted deliberately - never a forwarded metadata blob).
  decode_failed = 1,
  // Crash-free session rate, by version: gates the staged rollout.
  session_started = 2,
  session_ended_clean = 3,
  // p99 frame time by GPU vendor + driver version.
  frame_pacing = 4,
  // Hardware decode availability and the silent-fallback rate. "mode", not
  // "path": the schema check bans the word "path" in a schema name outright,
  // rather than trying to tell a code path from a file path.
  video_decode_mode = 5,
  // Feature reach: which panes and tools are opened at all.
  feature_used = 6,
};

// A named integer. The name comes from the call site as a literal; the value
// is a number. Neither can be a path.
struct metric {
  std::string_view name;
  std::int64_t value = 0;
};

// Pure, and the whole privacy gate. True when a string must never be sent:
// it contains a path separator, a drive-letter prefix, a '%' environment
// reference, a '~', an '@', a dot that could be a file extension, a NUL, or
// anything outside printable ASCII. A camera model ("ILCE-7M3"), a format
// ("heif"), a decoder ("libde265"), and a GPU vendor ("NVIDIA") all pass; a
// filename, a folder, a URL, a username and a hash of a path do not.
[[nodiscard]] bool looks_like_user_data(std::string_view s) noexcept;

// Pure. A tag is accepted only if it is short, non-empty, and passes
// looks_like_user_data. Length is capped so a caller cannot smuggle a path in
// by stripping its separators.
inline constexpr std::size_t kMaxTagLength = 32;
[[nodiscard]] bool tag_allowed(std::string_view tag) noexcept;

// ---- consent ---------------------------------------------------------------

// Read from settings.ini [telemetry]. Default off. `asked` is false until the
// user has answered the first-run screen either way.
[[nodiscard]] bool enabled() noexcept;
[[nodiscard]] bool asked() noexcept;

// The user's answer from the first-run screen, or a later Settings change.
// Turning it OFF also drops the install id and deletes the spool: "a setting
// that turns it off later and actually does" (plan/13).
void set_enabled(bool on) noexcept;

// A random, rotatable, non-reversible install id (plan/13): 128 bits of
// system randomness, hex. Never a machine id, MAC, SID, or anything derived
// from the user. Generated lazily on the first consented event, so a machine
// that never opts in never has one. Empty while consent is off.
[[nodiscard]] std::string install_id() noexcept;

// Throw the id away and take a new one on the next event. Offered because a
// stable id the user cannot reset is the thing they would object to.
void rotate_install_id() noexcept;

// ---- recording -------------------------------------------------------------

// Records one event. Returns false when it was dropped, which happens when
// consent is off (the common case), when `tag` is not allowed, when a metric
// name is not allowed, or when the spool cannot be written.
//
// Never blocks on the network - there is no network here. It appends a short
// line to a local file, so call it off the UI and render threads (rule 1).
bool record(event id, std::string_view tag, const std::vector<metric>& metrics) noexcept;

// Pure, and what the CI schema check reads: the exact line `record` would
// write. Exposed so a test can assert the payload rather than the intent.
[[nodiscard]] std::string format_event(event id, std::string_view tag,
                                       const std::vector<metric>& metrics,
                                       std::string_view install, std::string_view version);

// The spool file, or empty when this is not a real install.
[[nodiscard]] std::wstring spool_path();

}  // namespace mv::shell::telemetry
