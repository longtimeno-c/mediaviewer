// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/agent/install.h"

#include "addons/agent/server.h"  // to_text
#include "core/json.h"

namespace mv::agent {
namespace {

json::value str(std::string s) {
  json::value v;
  v.k = json::kind::string;
  v.s = std::move(s);
  return v;
}

json::value entry(const std::string& exe) {
  json::value e;
  e.k = json::kind::object;
  e.o.emplace_back("command", str(exe));
  json::value args;
  args.k = json::kind::array;
  args.a.push_back(str("--mcp"));
  e.o.emplace_back("args", std::move(args));
  return e;
}

std::string quoted(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";
#else
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
#endif
}

}  // namespace

std::vector<std::string> claude_code_add_args(const std::string& exe) {
  return {"mcp", "add", "--scope", "user", std::string(kServerName), "--", exe, "--mcp"};
}
std::vector<std::string> claude_code_remove_args() { return {"mcp", "remove", "--scope", "user", std::string(kServerName)}; }
std::vector<std::string> claude_code_get_args() { return {"mcp", "get", std::string(kServerName)}; }

std::string claude_code_command_line(const std::string& exe) {
  return "claude mcp add --scope user " + std::string(kServerName) + " -- " + quoted(exe) + " --mcp";
}

std::string client_config_snippet(const std::string& exe) {
  json::value root;
  root.k = json::kind::object;
  json::value servers;
  servers.k = json::kind::object;
  servers.o.emplace_back(std::string(kServerName), entry(exe));
  root.o.emplace_back("mcpServers", std::move(servers));
  return to_text(root);
}

bool desktop_config_with(std::string_view old_text, const std::string& exe, std::string& out, std::string* why) {
  json::value root;
  root.k = json::kind::object;
  bool blank = true;
  for (const char c : old_text) blank = blank && (c == ' ' || c == '\n' || c == '\r' || c == '\t');
  if (!blank) {
    auto parsed = json::parse(old_text, 64);
    if (!parsed || parsed->k != json::kind::object) {
      if (why) *why = "Claude Desktop's settings file is not JSON MediaViewer can read; it was left as it is";
      return false;
    }
    root = std::move(*parsed);
  }
  json::value* servers = nullptr;
  for (auto& [k, v] : root.o) {
    if (k == "mcpServers") servers = &v;
  }
  if (!servers) {
    json::value s;
    s.k = json::kind::object;
    root.o.emplace_back("mcpServers", std::move(s));
    servers = &root.o.back().second;
  }
  if (servers->k != json::kind::object) {
    if (why) *why = "Claude Desktop's settings file has an mcpServers that is not an object; it was left as it is";
    return false;
  }
  bool replaced = false;
  for (auto& [k, v] : servers->o) {
    if (k == kServerName) {
      v = entry(exe);
      replaced = true;
    }
  }
  if (!replaced) servers->o.emplace_back(std::string(kServerName), entry(exe));
  out = to_text(root);
  return true;
}

bool desktop_config_without(std::string_view old_text, std::string& out, std::string* why) {
  auto parsed = json::parse(old_text, 64);
  if (!parsed || parsed->k != json::kind::object) {
    if (why) *why = "Claude Desktop's settings file is not JSON MediaViewer can read; it was left as it is";
    return false;
  }
  for (auto& [k, v] : parsed->o) {
    if (k != "mcpServers" || v.k != json::kind::object) continue;
    for (auto it = v.o.begin(); it != v.o.end();) {
      it = it->first == kServerName ? v.o.erase(it) : it + 1;
    }
  }
  out = to_text(*parsed);
  return true;
}

bool desktop_config_has(std::string_view text, std::string* command) {
  const auto parsed = json::parse(text, 64);
  if (!parsed) return false;
  const json::value* servers = parsed->find("mcpServers");
  if (!servers) return false;
  const json::value* me = servers->find(kServerName);
  if (!me) return false;
  if (command) {
    const std::string* c = me->str("command");
    *command = c ? *c : std::string();
  }
  return true;
}

}  // namespace mv::agent
