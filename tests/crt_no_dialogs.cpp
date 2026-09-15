// SPDX-License-Identifier: GPL-2.0-or-later
//
// Keep a failing test loud instead of silent.
//
// A Debug CRT or debug-STL check that fails — inverted std::clamp bounds, an
// out-of-range iterator, a bad CRT argument — reports through _CrtDbgReport,
// whose default mode in a Debug build is a modal message box. On a CI runner
// nobody clicks it: the process blocks, ctest waits, and the step is killed by
// its timeout with the log ending mid-test and no reason given. That is how a
// one-line bounds bug burned fifteen minutes per Debug leg and reported
// nothing.
//
// Linked into every test executable. Reports go to stderr, abort() keeps its
// message but not the Windows Error Reporting dialog, so a violated check
// fails the test with its text in --output-on-failure.

#if defined(_MSC_VER)

#include <stdlib.h>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

struct crt_reports_to_stderr {
  crt_reports_to_stderr() noexcept {
#if defined(_DEBUG)
    for (const int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
      _CrtSetReportMode(report, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
      _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif
    // Keep the abort message, drop the crash dialog.
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
  }
};

[[maybe_unused]] const crt_reports_to_stderr install_before_main;

}  // namespace

#endif  // _MSC_VER
