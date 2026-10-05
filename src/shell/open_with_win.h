// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A document's "Open in <app>" (docs/design/20): a PDF or DOCX has nothing to
// edit here, so the bar's Edit button becomes the apps Windows recommends for
// it (Explorer's Open with list), the default first and MediaViewer left out.
// Both calls talk to the shell and may block: run them on a worker thread,
// never the UI thread (CLAUDE.md rule 1). Each initialises COM for itself.
// Windows host only; the Mac twin is main_mac.mm -openDocumentInApp:.
#pragma once

#include <string>
#include <vector>

namespace mv::shell {

struct open_with_app {
  std::wstring name;  // "Word", as Explorer's Open with shows it
  std::wstring exe;   // the handler's program; identifies it to open_with()
};

// The apps for files with this extension (".docx"), default first. [worker]
[[nodiscard]] std::vector<open_with_app> list_open_with(const std::wstring& extension);

// Opens `file` in the app whose exe is `exe`, or the first of
// list_open_with() when `exe` is empty. False when there is none or the
// shell refused. [worker]
bool open_with(const std::wstring& file, const std::wstring& exe);

}  // namespace mv::shell
