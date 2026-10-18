// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// `MediaViewer --mcp` (docs/design/29 "The agent"): the app's own executable as
// the AI agents add-on's MCP server, over the stdio pipes the MCP client
// started it with. Both hosts' main() hand over before anything of the viewer
// starts: no window, no chrome, no crash reporter, no add-on host lock.
//
//   MediaViewer --mcp [--timeline FILE.fcpxml] [--folder DIR]... [--no-handoff]
//
// It loads the installed AI agents add-on (verified, host table v3 with the
// viewer's own sound and frame readers) and, at the first search or
// transcript, the Local search pack's read-only reader; then serves until the
// client closes stdin. Nothing it starts opens a socket. Portable C++.
#pragma once

#include <string>
#include <vector>

namespace mv::shell {

// `args` are the arguments after the executable, UTF-8. The exit code.
[[nodiscard]] int run_mcp(const std::vector<std::string>& args);

}  // namespace mv::shell
