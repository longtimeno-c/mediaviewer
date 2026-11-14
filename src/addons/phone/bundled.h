// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The `driftwire` program the Phone access add-on carries (owner, 2026-11-14:
// "ship it inside"; docs/plans/phone-access-addon.md §8), so "This computer"
// needs no Homebrew or install.ps1 line.
//
// The add-on's own folder changes with every update (a version folder), so the
// program is installed once more into a stable place, `<feature dir>/bin`,
// which the start-at-login entry points at and which survives add-on updates.
// An update never writes over the program in place: the new one is copied
// beside it (`driftwire.new`) and renamed into place. On Windows, where a
// running .exe cannot be replaced or deleted but can be renamed, the running
// one is renamed aside first (`driftwire.old-N.exe`, removed at a later
// install once nothing runs it). The running host's supervisor sees its
// program change and ends 75 (cli/host_cmd.h), and the login entry starts the
// new one: the phones see one reconnect.
//
// Worker threads only (file copies). Nothing here logs a path (rule 6).
// Portable (D9): plain std::filesystem, the Windows rename-aside chosen at run
// time so every OS's tests run it.
#pragma once

#include <string>

#include "core/result.h"

namespace mv::phone::bundled {

// "driftwire.exe" on Windows, "driftwire" elsewhere.
[[nodiscard]] const char* program_name() noexcept;
// <feature dir>/bin, and the program in it.
[[nodiscard]] std::string bin_dir(const std::string& feature_dir);
[[nodiscard]] std::string program_in(const std::string& bin_dir);

struct installed {
  std::string program;    // the stable program, ready to run
  bool replaced = false;  // a different one was there (a running host restarts)
};
// Installs the program in `source_dir` (the add-on's `host` folder: the program
// and, on Windows, the DLLs it loads beside it) into bin_dir, recorded as
// `version` (bin/installed.json): nothing is copied when that version of that
// size is already there. `rename_aside`: move each old file out of the way
// before renaming the new one in (Windows' way; the tests run both).
// status::not_found with no program in source_dir; status::io when it cannot be written.
[[nodiscard]] result<installed> install(const std::string& source_dir, const std::string& bin_dir,
                                        const std::string& version, bool rename_aside);
// The Windows way on Windows.
[[nodiscard]] bool rename_aside_here() noexcept;
// Everything in bin_dir that this installed (the program, its record, old
// copies); a file still running stays and is removed at a later install.
void remove(const std::string& bin_dir) noexcept;

}  // namespace mv::phone::bundled
