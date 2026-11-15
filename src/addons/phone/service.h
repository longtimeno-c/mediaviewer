// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The process's one Phone access link (link.h), as the Phone access add-on
// (addon_entry.cpp, docs/plans/phone-access-addon.md) drives it: made at the
// first request or state read, its files in the feature's folder (feature_dir,
// where the app kept them before the add-on: a library set up then keeps its
// id), its keys in the OS's store under "MediaViewer Phone access", the desk
// (desk.h) behind it. Items Driftwire kept for phone access before 2026-11-12
// move here on first use, as the publisher key does from Local search
// (catalogue/publisher.h). Every call is [no-block]. Portable (D9).
#pragma once

#include <string>

#include "core/result.h"
#include "addons/phone/link.h"

namespace mv::phone {

// The feature's folder: "MediaViewer/Phone Access" under ~/Library/Application
// Support (macOS, and the same under $HOME elsewhere off Windows),
// "MediaViewer\phone-access" under %LocalAppData% (io::phone_dir's places, kept
// so a library set up by the app before the add-on is found in place). Not
// created here. status::io with no home. MV_PHONE_ACCESS_DIR (the tests) names another.
[[nodiscard]] result<std::string> feature_dir();
// Before the first request or resume: the folder the link uses (the tests', or
// feature_dir()). "" leaves it unset: the link keeps nothing.
void set_app_dir(std::string dir);

// Settings → Phone access: a request, and the card's state.
[[nodiscard]] expected app_request(const std::string& request_json);
[[nodiscard]] std::string app_state_json();
// The host, as Local search loads (a source) or goes (an empty one). Kept for
// a link made later. Any thread.
void set_app_source(catalogue_source s);
// At the add-on's load. The process holding the feature's instance lock
// (`primary`) runs Phone access: a library set up before carries on
// (link::resume). Another window's process runs none of it (two would race on
// the NAS and serve nearby twice): its requests are refused and its state says
// {"secondary":true}. Before either is said nothing runs: requests answer busy,
// the state is {"starting":true}.
void resume_app(bool primary);
// At the add-on's shutdown: Local search is no longer called (sync), and the
// link stops (it never waits on the NAS: a request in flight ends on its own).
// Then the library may go.
void stop_app() noexcept;

}  // namespace mv::phone
