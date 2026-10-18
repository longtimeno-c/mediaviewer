// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #452 (docs/design/28 "In the background"): MediaViewer starts at sign-in while an
// add-on keeps the process (Sharepoint's Receive when MediaViewer is closed), so a
// restart does not leave the machine unreachable until the app is opened by hand.
//
// One per-user Run value, HKCU\Software\Microsoft\Windows\CurrentVersion\Run\
// MediaViewer = "<root>\MediaViewer.exe" --background (the install's stub, which outlives
// updates, as the jump list's entries do). Written when the keep-alive list turns
// non-empty, removed when it turns empty, when a --background start finds nothing to
// keep (the add-on was removed, or its toggle was turned off in another window), and
// by the uninstaller hook. Settings > Apps > Startup lists it, so a person can turn
// it off there too. Installed builds only: a dev build's path does not outlive its
// build folder. Registry I/O: call from a worker, never the UI thread (rule 1).
// Windows host only. The Mac twin is SMAppService.mainApp in main_mac.mm.
#pragma once

#include <string>
#include <string_view>

namespace mv::shell::login_item {

// The value's data for `exe`: the quoted path and --background.
[[nodiscard]] std::wstring command_for(std::wstring_view exe);

// On: the Run value starts `exe` (written only if it differs). Off: the value is
// removed if it is there (`exe` unused). False when the registry refused.
bool set(bool on, std::wstring_view exe) noexcept;

}  // namespace mv::shell::login_item
