// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `MediaViewer --search-agent` (docs/design/23): launchd starts it when the Final Cut
// Pro add-on's extension looks up the agent's Mach service
// (packaging/macos/fcp/agent.plist.in), and main_mac.mm hands over to this
// before the viewer starts anything. It loads the installed, verified Final
// Cut Pro add-on and serves Local search through the AI pack's read-only
// reader until idle, then exits the process. Returns only on failure.
#pragma once

int MvSearchAgentMain();
