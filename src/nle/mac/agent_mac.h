// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Final Cut Pro search agent's entry point (docs/design/23), in MediaViewer's own
// executable: launchd starts `MediaViewer --search-agent`
// (packaging/macos/fcp/agent.plist.in) and main_mac.mm hands over to this
// before the viewer starts anything. Serves the Mach service until idle, then
// exits the process; never returns while serving.
#pragma once

int MvSearchAgentMain();
