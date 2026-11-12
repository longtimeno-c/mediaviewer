// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The app's one Phone access link (link.h), as the hosts reach it: made at the
// first request or state read, its files in io::phone_dir(), its keys in the
// OS's store under "MediaViewer Phone access", the desk (desk.h) behind it.
// Items Driftwire kept for phone access before 2026-11-12 move here on first
// use, as the publisher key does from Local search (catalogue/publisher.h).
// Every call is [no-block]. Portable (D9).
#pragma once

#include <string>

#include "core/result.h"
#include "phone/link.h"

namespace mv::phone {

// Settings → Phone access: a request, and the card's state.
[[nodiscard]] expected app_request(const std::string& request_json);
[[nodiscard]] std::string app_state_json();
// The host, as Local search loads (a source) or goes (an empty one). Kept for
// a link made later. Any thread.
void set_app_source(catalogue_source s);
// At launch. The first window's process (`primary`) runs Phone access: a
// library set up before carries on (link::resume). A later window's process
// runs none of it (two would race on the NAS and serve nearby twice): its
// requests are refused and its state says {"secondary":true}.
void resume_app(bool primary);
// At quit, from the main thread: Local search is no longer called (sync),
// and the link stops (it never waits on the NAS: a request in flight ends on
// its own). Then the add-ons may unload.
void stop_app() noexcept;

}  // namespace mv::phone
