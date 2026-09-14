// SPDX-License-Identifier: GPL-2.0-or-later
// Crashpad client for the Windows host — plan/13 Part 2, PR 7.
//
//  - crashpad_handler.exe beside the exe, started out-of-process and
//    asynchronously (the unhandled-exception filter is live immediately; a
//    crash before the handler is up waits for it).
//  - Database: %LocalAppData%\MediaViewer\Crashes. No upload URL, ever, from
//    this client: a dump is uploaded only after the privacy scrub has run on
//    it, and the handler cannot scrub (see minidump_scrub.h). PR 15 owns the
//    upload path; until then SetUploadsEnabled(false) unless consent is
//    recorded AND an upload URL is configured.
//  - Indirectly-referenced memory OFF, WER forwarding OFF, no extra ranges.
//  - Annotations: the core's crash-context slots (format / decoder / version /
//    geometry / correlation id) and the last ABI call id. Nothing else.
//  - A background thread scrubs every finished, unscrubbed report in the
//    database — before any send is offered.
//
// Handler missing → the app runs without crash reporting and logs once.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mv::shell::crash {

struct start_result {
  bool handler_found = false;
  bool started = false;
  double elapsed_ms = 0.0;  // on the calling (UI) thread
};

// Call once, early in wWinMain, after the core DLL is loaded and before any
// session exists. Never blocks on the handler process.
start_result start() noexcept;

// %LocalAppData%\MediaViewer\Crashes (empty if unavailable).
std::wstring crashes_dir();

// Username and computer name, UTF-8, for the scrub.
std::vector<std::string> local_identities();

// Scrubs every .dmp under `reports_dir` that is not already marked. Returns
// the number rewritten. Used by the background pass and by tests/tools.
std::size_t scrub_reports_in(const std::wstring& reports_dir,
                             const std::vector<std::string>& identities) noexcept;

}  // namespace mv::shell::crash
