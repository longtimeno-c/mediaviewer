// SPDX-License-Identifier: GPL-2.0-or-later
// PR 8 updater, native half: Velopack hook exit, start-attempt guard, rollback,
// and the restart-state arguments (plan/13 Part 1, "Rollback and the kill
// switch", "Preserve state across the restart").
//
// Why native: a version that fails to start usually fails before .NET or the
// chrome is loaded, so only the host exe can count its own starts. The managed
// updater (src.managed/MediaViewer.Updater) arms the record; this file counts,
// clears and rolls back. Windows host only (D9) — lives in shell/.
//
// Record: <install root>\updater\trial.ini (not settings.ini)
//   [trial]  version, prior_version, prior_package, attempts
//   [failed] versions (comma list), unreported
// Versions and one package filename only (rule 6).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::shell::update {

// Bit in the chrome settings word (chrome_flags_args) for [update] auto_check,
// the "disable automatic update checks" switch (plan/13 Part 3). Above the
// view_settings bits; stored in settings.ini through the settings store, not
// in view_settings. The island mirrors it as SettingFlag.UpdateAutoCheck.
inline constexpr std::int32_t kChromeFlagUpdateAutoCheck = 1 << 8;

// A version that has failed to start this many times is rolled back on the
// next start (plan/13: "fails to start twice").
inline constexpr int kMaxFailedStarts = 2;

// Velopack runs the main exe with --veloapp-install/-updated/-obsolete/
// -uninstall <version> and kills it after 15-30 s. The viewer has nothing to
// do in any of them: exit before a window, a session or .NET exists.
[[nodiscard]] bool is_velopack_hook(std::wstring_view first_arg) noexcept;

struct trial_record {
  std::string version;        // armed version; empty = no trial
  std::string prior_version;
  std::string prior_package;  // filename in updater\rollback\, or empty
  int attempts = 0;           // starts counted, none confirmed
};

enum class start_action {
  normal,    // no trial for this version
  counted,   // write attempts, then start
  rollback,  // reinstall the prior package, report, exit
  abandon,   // clear the record, then start (stale, or nothing to roll back to)
};

struct start_decision {
  start_action action = start_action::normal;
  int attempts = 0;  // value to write for `counted`
};

// Pure.
[[nodiscard]] start_decision decide_start(const trial_record& trial,
                                          std::string_view running_version) noexcept;

// Pure. The <version> of a Velopack sq.version (nuspec) document, or empty.
[[nodiscard]] std::string parse_manifest_version(std::string_view xml);

// Pure. "a,b" + "c" -> "a,b,c" without duplicates.
[[nodiscard]] std::string append_version_list(std::string_view list, std::string_view version);

// What the viewer is showing, for the restart the user asked for.
struct view_restore {
  std::wstring path;        // selected file, or the folder; empty = nothing open
  unsigned zoom_percent = 0;  // 0 = fit (leave the default)
  bool fullscreen = false;
  bool gallery = false;
};

// Pure. Arguments the host already parses (parse_options): --restore-zoom N,
// --restore-fullscreen, --restore-gallery, then the path. Playback position is
// not an argument: the player saves its resume point when the clip closes.
[[nodiscard]] std::vector<std::wstring> restart_arguments(const view_restore& view);

// Pure. NUL-separated UTF-16 argument blob for the managed updater.
[[nodiscard]] std::wstring join_arguments(const std::vector<std::wstring>& args);

// ---- Win32 -----------------------------------------------------------------

struct install_layout {
  std::wstring root;       // %LocalAppData%\MediaViewer (or wherever installed)
  std::wstring update_exe;
  std::string version;     // from current\sq.version
  [[nodiscard]] bool installed() const noexcept { return !root.empty() && !version.empty(); }
};

// Our exe in <root>\current\ with <root>\Update.exe and sq.version beside it.
// A dev build returns an empty layout and every call below is a no-op.
[[nodiscard]] install_layout locate_install() noexcept;

[[nodiscard]] std::wstring trial_file(const install_layout& layout);
[[nodiscard]] trial_record load_trial(const install_layout& layout) noexcept;

// [startup, before the window] Applies decide_start. True when the process
// must exit now because a rollback was handed to Update.exe.
[[nodiscard]] bool run_start_guard(const install_layout& layout) noexcept;

// The version started properly (chrome attached, and it survived a few
// seconds). Clears [trial]. Any thread; small file write, call it off the UI
// thread.
void confirm_started(const install_layout& layout) noexcept;

}  // namespace mv::shell::update
