// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The few things the AI agents add-on asks of the OS (D9: a _posix and a _win
// half): its own executable, running the `claude` command line, Claude
// Desktop's settings file, handing a version to Final Cut Pro, and the stdio
// pipes the MCP client started `MediaViewer --mcp` with.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::agent::platform {

// The running executable (MediaViewer itself, when loaded by the app).
[[nodiscard]] std::string self_executable();

// Runs `exe` with `args`, stdout and stderr together into `output` (at most
// 64 KiB), killed after `timeout_ms`. True when it ran to an exit.
[[nodiscard]] bool run(const std::string& exe, const std::vector<std::string>& args, std::string& output,
                       int& exit_code, int timeout_ms);

// The `claude` command line (Claude Code), "" when it is not installed.
[[nodiscard]] std::string find_claude();
// Claude Desktop's claude_desktop_config.json, and whether Claude Desktop is installed.
[[nodiscard]] std::string desktop_config_path();
[[nodiscard]] bool desktop_installed();

// Asks Final Cut Pro to import an FCPXML file. False with why where there is none.
[[nodiscard]] bool open_in_final_cut(const std::string& fcpxml, std::string* why);

// The serve pipes. read_line: false at the end of input.
[[nodiscard]] bool read_line(std::intptr_t in, std::string& pending, std::string& line);
[[nodiscard]] bool write_all(std::intptr_t out, std::string_view data);

// Reads / writes a whole small file (UTF-8 path).
[[nodiscard]] bool read_file(const std::string& path, std::string& out);
[[nodiscard]] bool write_file_atomic(const std::string& path, const std::string& text);

}  // namespace mv::agent::platform
