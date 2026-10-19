// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The timeline agent's MCP server (docs/design/29 "The agent"): JSON-RPC 2.0,
// one message per line, over the stdio pipes an MCP client starts
// `MediaViewer --mcp` with. No socket.
//
// It holds the project the editor dragged onto the Final Cut Pro panel (or an
// FCPXML file named with --timeline), a working copy the `edit` tool changes,
// and the scope (scope.h). Tools:
//
//   get_timeline     the timeline: items in order, times, files, roles, text, markers, the playhead
//   transcript       phrases and estimated words on the timeline, with the silences in the sound
//   search           Local search inside the scope only
//   find_similar     other takes and similar shots, inside the scope only
//   scope_files      the media in the given folders (B-roll, other takes)
//   analyse          asks MediaViewer to index the timeline's media and the folders, and follows it
//   edit             a batch of operations on the working copy (nle/edit.h)
//   preview_changes  what changed against the dragged project, the new length
//   commit           writes "<Project> (Claude N)" and hands it to Final Cut Pro
//   reset            back to the dragged project
//   thumbnail        a frame, for a model that looks (off unless Settings turns it on)
//
// Logs status names only (rule 6). Portable C++.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "addons/agent/environment.h"
#include "addons/agent/scope.h"
#include "core/json.h"
#include "nle/bridge_state.h"
#include "nle/edit.h"
#include "nle/timeline.h"

namespace mv::agent {

struct server_options {
  std::string bridge_dir;              // the panel's Timeline/ folder; "" none
  std::string timeline_file;           // --timeline: work on this FCPXML instead
  std::vector<std::string> folders;    // --folder: added to the panel's
  std::string version = "0.0.0-dev";   // serverInfo
  bool handoff = true;                 // commit asks FCP to open the version
  std::string skills_dir;              // the add-on's skills (SKILL.md folders), served as MCP prompts
};

class server {
 public:
  server(environment& env, server_options options);

  // One message in, its response out ("" for a notification or nothing to say).
  [[nodiscard]] std::string handle(std::string_view line);

  // For tests: a tool by name with JSON arguments; the result object as text.
  [[nodiscard]] std::string call(std::string_view tool, std::string_view arguments_json, bool* is_error = nullptr);

  [[nodiscard]] static const char* instructions();

 private:
  struct outcome {
    std::string text;            // what the client reads
    bool error = false;
    std::string image_base64;    // thumbnail
  };
  std::string respond(const json::value& id, const std::string& result_json);
  std::string fail(const json::value* id, int code, std::string_view message);
  std::string tools_list() const;
  std::string prompts_list() const;
  std::string prompt_get(const json::value& params, std::string* error) const;
  outcome dispatch(std::string_view tool, const json::value& args);

  // The session.
  bool load(std::string* why);              // (re)reads the source when it changed and nothing is pending
  nle::bridge_state bridge() const;
  scope current_scope() const;
  std::string source_path() const;

  outcome get_timeline(const json::value& args);
  outcome transcript(const json::value& args);
  outcome search(const json::value& args, bool similar);
  outcome edit(const json::value& args);
  outcome scope_files(const json::value& args);
  outcome analyse(const json::value& args);
  outcome preview();
  outcome commit(const json::value& args);
  outcome reset();
  outcome thumbnail(const json::value& args);

  environment& env_;
  server_options o_;
  std::unique_ptr<nle::timeline> original_;
  std::unique_ptr<nle::timeline> working_;
  std::vector<nle::change> changes_;
  std::string loaded_from_;
  std::int64_t loaded_stamp_ = 0;   // the source's mtime and size, folded
  bool newer_waiting_ = false;      // a newer drag arrived while edits were pending
  std::vector<std::string> committed_;  // names written this session
  std::string protocol_ = "2025-06-18";
  bool initialized_ = false;
};

// A JSON value as compact text (the strict reader's values back out).
[[nodiscard]] std::string to_text(const json::value& v);

// Base64, for image content.
[[nodiscard]] std::string base64(const std::vector<std::uint8_t>& bytes);

}  // namespace mv::agent
