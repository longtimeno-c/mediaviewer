// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Adding MediaViewer's timeline agent to an MCP client (docs/design/29 "Install"):
// the command Claude Code takes, the entry Claude Desktop's settings file
// needs, and the snippet for any other client. The add-on's AI agents window
// runs the first and, after asking, writes the second; nothing here runs or
// writes on its own. Portable C++.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mv::agent {

inline constexpr std::string_view kServerName = "mediaviewer";

// claude mcp add --scope user mediaviewer -- <exe> --mcp
[[nodiscard]] std::vector<std::string> claude_code_add_args(const std::string& exe);
[[nodiscard]] std::vector<std::string> claude_code_remove_args();
[[nodiscard]] std::vector<std::string> claude_code_get_args();
// The same as one line a person can paste into a terminal (quoted for the platform's shell).
[[nodiscard]] std::string claude_code_command_line(const std::string& exe);

// {"mcpServers":{"mediaviewer":{"command":"<exe>","args":["--mcp"]}}}
[[nodiscard]] std::string client_config_snippet(const std::string& exe);

// Claude Desktop's claude_desktop_config.json with MediaViewer added (or
// updated to this executable), every other key kept. `old_text` empty: a new
// file. False when the file is not JSON (it is not touched then).
[[nodiscard]] bool desktop_config_with(std::string_view old_text, const std::string& exe, std::string& out,
                                       std::string* why);
// The same file without MediaViewer.
[[nodiscard]] bool desktop_config_without(std::string_view old_text, std::string& out, std::string* why);
// Whether the file names MediaViewer, and with which executable.
[[nodiscard]] bool desktop_config_has(std::string_view text, std::string* command = nullptr);

}  // namespace mv::agent
