// SPDX-License-Identifier: GPL-2.0-or-later
// Crashpad client for the macOS host — PR 11's Mac half (plan/10 PR 11, owner
// call 2026-09-24; plan/13 Part 2). The twin of crash_reporter_win.h:
//
//  - crashpad_handler out of process: Contents/Helpers/crashpad_handler in
//    MediaViewer.app, or beside mediaviewer_lab. Crashpad catches the Mach
//    exception (a decoder's bad access, a Swift runtime trap, the abort()
//    after an uncaught NSException) and writes the minidump from the handler,
//    so a smashed stack or heap in the app does not stop the report.
//  - Database: ~/Library/Application Support/MediaViewer/Crashes. No upload
//    URL, ever, from this client, and uploads disabled: there is no endpoint
//    on either platform yet (plan/13). A dump is only ever sendable after the
//    scrub below has rewritten it.
//  - Indirectly-referenced memory OFF, forwarding to Apple's ReportCrash OFF
//    (it would keep an unscrubbed report of its own), no extra ranges.
//  - Annotations: the core's crash-context slots (format / decoder / version /
//    geometry / correlation id) and the last native call id — the same eight
//    mv_decode_N slots and mv_last_call_cid as Windows. Nothing else.
//  - A background thread scrubs every finished, unscrubbed .dmp in the
//    database (shell/minidump_scrub.h — the Windows scrub, which now knows
//    POSIX paths) before any send could be offered.
//
// The second capture path (plan/13 "two capture paths"): Swift / AppKit. An
// uncaught NSException is recorded as a scrubbed text report in Crashes/chrome/
// carrying the correlation id of the native call in flight, then left to
// abort — so Crashpad writes the minidump with the same id in
// mv_last_call_cid. NSApplicationCrashOnExceptions is set, so AppKit does not
// swallow an exception thrown inside event handling. A Swift runtime trap is a
// Mach exception: Crashpad records it, with the Swift runtime's own message in
// the dump's crash-info stream (scrubbed like every other string).
//
// Handler missing → the app runs without crash reporting and logs once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mv::shell::crash {

struct start_result {
  bool handler_found = false;
  bool started = false;
  double elapsed_ms = 0.0;  // on the calling (main) thread
};

// Call once, early in main(), before NSApplication runs. Also installs the
// NSException capture. Never waits on the handler's first report scan.
start_result start() noexcept;

// ~/Library/Application Support/MediaViewer/Crashes ("" if unavailable).
std::string crashes_dir();

// Short user name, full name, computer name and host name, UTF-8, for the scrub.
std::vector<std::string> local_identities();

// Scrubs every .dmp under `dir` (recursively: Crashpad's new/, pending/,
// completed/) that is not already marked. Returns the number rewritten.
std::size_t scrub_reports_in(const std::string& dir,
                             const std::vector<std::string>& identities) noexcept;

// [main thread] A native call is starting (a routed command, a bridge call
// from the SwiftUI chrome, an open). Stamps a fresh correlation id into the
// core's crash context (mv_last_call_cid) and returns it, so a chrome
// exception and a native crash can be tied to the same call (plan/13).
std::uint64_t note_native_call() noexcept;

// The deliberate crashes of the verify, armed only by MV_CRASH_TEST:
//   nsexception — raise an NSException from inside AppKit event handling
//   swift_trap  — a Swift runtime trap in the chrome (index out of range)
// `decode` stays the core's (codec/crash_test_hook.h). Returns the armed kind
// ("" when none), for main_mac.mm to schedule after launch.
std::string armed_chrome_test();

}  // namespace mv::shell::crash
