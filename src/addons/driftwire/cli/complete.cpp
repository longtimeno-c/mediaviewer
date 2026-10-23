// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/complete.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

namespace mv::driftwire::cli {

namespace fs = std::filesystem;

namespace {

// clang-format off
constexpr std::array kCommands{
    command_info{"/send", "PATH...", "Add files to send (or type or drop them)", ""},
    command_info{"/folder", "DIR", "Send a folder", ""},
    command_info{"/files", "", "List the files ready to send", ""},
    command_info{"/clear", "", "Clear the files ready to send", ""},
    command_info{"/code", "", "Receive from anywhere: make a code to give someone", ""},
    command_info{"/to-code", "", "Send these files to a code someone gave you", ""},
    command_info{"/devices", "", "List remembered devices", ""},
    command_info{"/forget", "N", "Forget a remembered device", ""},
    command_info{"/mine", "N on|off", "This is my computer: it may relay for me", ""},
    command_info{"/unfinished", "", "List receives that stopped part way", ""},
    command_info{"/discard", "N", "Drop an unfinished receive", ""},
    command_info{"/save-to", "DIR", "Where received files go", ""},
    command_info{"/internet", "on|off", "Send and receive over the internet", ""},
    command_info{"/dht", "on|off", "Find each other through the public DHT", ""},
    command_info{"/handover", "on|off", "Continue over the internet if Wi-Fi drops", ""},
    command_info{"/relay", "on|off", "Use my other computers to relay", ""},
    command_info{"/relay-server", "", "Your own relay server (the key is typed, never an argument)", ""},
    command_info{"/settings", "", "Show the current settings", ""},
    command_info{"/help", "", "Commands and keys", ""},
    command_info{"/quit", "", "Close Driftwire", "/exit"},
};
// clang-format on

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool starts_with_folded(std::string_view s, std::string_view prefix, bool fold) {
  if (prefix.size() > s.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (fold ? lower(s[i]) != lower(prefix[i]) : s[i] != prefix[i]) return false;
  }
  return true;
}

bool contains_folded(std::string_view s, std::string_view part) {
  if (part.empty()) return true;
  for (std::size_t i = 0; i + part.size() <= s.size(); ++i) {
    if (starts_with_folded(s.substr(i), part, true)) return true;
  }
  return false;
}

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool separator(char c, bool windows) { return c == '/' || (windows && c == '\\'); }

// A shell's special characters, escaped with a backslash in an unquoted word.
std::string escape_bare(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (std::string_view(" \t'\"\\$`*?[]{}()&;|<>!#").find(c) != std::string_view::npos || (i == 0 && c == '~')) {
      out += '\\';
    }
    out += c;
  }
  return out;
}

std::string escape_single(std::string_view s) {
  std::string out;
  for (const char c : s) {
    if (c == '\'') {
      out += "'\\''";  // close, an escaped quote, open again
    } else {
      out += c;
    }
  }
  return out;
}

std::string escape_double(std::string_view s, bool windows) {
  std::string out;
  for (const char c : s) {
    if (!windows && std::string_view("\"\\$`").find(c) != std::string_view::npos) out += '\\';
    out += c;
  }
  return out;
}

// One past the last byte two names share, on a character boundary.
std::size_t common_prefix(std::string_view a, std::string_view b, bool fold) {
  std::size_t n = 0;
  while (n < a.size() && n < b.size() && (fold ? lower(a[n]) == lower(b[n]) : a[n] == b[n])) ++n;
  while (n > 0 && n < a.size() && (static_cast<unsigned char>(a[n]) & 0xC0) == 0x80) --n;
  return n;
}

struct entry_name {
  std::string name;
  bool folder = false;
};

}  // namespace

std::span<const command_info> commands() noexcept { return kCommands; }

std::vector<std::size_t> filter_commands(std::string_view typed) {
  if (!typed.empty() && typed[0] == '/') typed.remove_prefix(1);
  if (const std::size_t sp = typed.find(' '); sp != std::string_view::npos) typed = typed.substr(0, sp);
  std::vector<std::size_t> starts, inside;
  for (std::size_t i = 0; i < kCommands.size(); ++i) {
    const std::string_view name = kCommands[i].name.substr(1);
    const std::string_view alias = kCommands[i].alias.empty() ? std::string_view() : kCommands[i].alias.substr(1);
    if (starts_with_folded(name, typed, true) || (!alias.empty() && starts_with_folded(alias, typed, true))) {
      starts.push_back(i);
    } else if (contains_folded(name, typed)) {
      inside.push_back(i);
    }
  }
  starts.insert(starts.end(), inside.begin(), inside.end());
  return starts;
}

int find_command(std::string_view name) {
  for (std::size_t i = 0; i < kCommands.size(); ++i) {
    if (kCommands[i].name == name || (!kCommands[i].alias.empty() && kCommands[i].alias == name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

completion complete_path(std::string_view line, std::size_t cursor, const path_options& o, bool folders_only) {
  completion c;
  c.line.assign(line);
  c.cursor = std::min(cursor, line.size());
  const std::string_view before = line.substr(0, c.cursor);
  const std::vector<word> words = split_words(before, o.windows);

  // The word being completed: the last one, unless a space after it starts a new one.
  word w;
  w.begin = w.end = c.cursor;
  if (!words.empty() && (words.back().end == c.cursor || words.back().open_quote)) w = words.back();
  if (std::find(w.glob.begin(), w.glob.end(), true) != w.glob.end()) return c;  // a glob: Enter expands it
  if (w.text.size() >= 5 && (w.text.compare(0, 5, "file:") == 0)) return c;

  const bool tilde = w.tilde && !o.home.empty();
  std::size_t cut = 0;  // after the word's last separator
  for (std::size_t i = 0; i < w.text.size(); ++i) {
    if (separator(w.text[i], o.windows)) cut = i + 1;
  }
  const char sep_char = o.windows ? '\\' : '/';
  if (tilde && cut == 0) {  // "~" alone: the home folder
    c.line = std::string(line.substr(0, w.begin)) + "~" + sep_char + std::string(line.substr(c.cursor));
    c.cursor = w.begin + 2;
    c.changed = true;
    return c;
  }
  const std::string_view stem = std::string_view(w.text).substr(cut);
  const std::string dir_text = tilde ? o.home + w.text.substr(1, cut - 1) : w.text.substr(0, cut);
  fs::path dir = to_path(dir_text.empty() ? std::string_view(".") : std::string_view(dir_text));
  if (!dir.is_absolute()) dir = to_path(o.cwd) / dir;

  std::vector<entry_name> hits;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), stop; !ec && it != stop;
       it.increment(ec)) {
    std::string name = to_utf8(it->path().filename());
    if (name.empty() || (name[0] == '.' && (stem.empty() || stem[0] != '.'))) continue;
    if (!starts_with_folded(name, stem, o.windows)) continue;
    std::error_code dec;
    const bool folder = it->is_directory(dec);
    if (folders_only && !folder) continue;
    hits.push_back({std::move(name), folder});
  }
  if (hits.empty()) return c;
  std::sort(hits.begin(), hits.end(), [](const entry_name& a, const entry_name& b) { return a.name < b.name; });

  std::string name;
  bool finished = false;  // one file: close the word
  if (hits.size() == 1) {
    name = hits[0].name + (hits[0].folder ? std::string(1, sep_char) : std::string());
    finished = !hits[0].folder;
  } else {
    std::size_t n = hits[0].name.size();
    for (const auto& h : hits) n = std::min(n, common_prefix(hits[0].name, h.name, o.windows));
    name = hits[0].name.substr(0, n);
    for (const auto& h : hits) c.choices.push_back(h.name + (h.folder ? std::string(1, sep_char) : std::string()));
    if (name.size() <= stem.size()) return c;  // nothing more in common: the list is the answer
  }

  // The word again, in the quoting it was typed in; a ~ stays bare in front.
  const std::string literal = w.text.substr(0, cut) + name;
  std::string head, rest = literal;
  if (tilde) {
    const std::size_t n = std::min<std::size_t>(2, literal.size());
    head = literal.substr(0, n);
    rest = literal.substr(n);
  }
  std::string out = head;
  if (w.open_quote == '\'') {
    out += "'" + escape_single(rest) + (finished ? "' " : "");
  } else if (w.open_quote == '"') {
    out += "\"" + escape_double(rest, o.windows) + (finished ? "\" " : "");
  } else if (!o.windows) {
    out += escape_bare(rest) + (finished ? " " : "");
  } else if (rest.find_first_of(" &()[]{}^=;!'+,`~") != std::string::npos) {
    out += "\"" + rest + (finished ? "\" " : "");
  } else {
    out += rest + (finished ? " " : "");
  }
  c.line = std::string(line.substr(0, w.begin)) + out + std::string(line.substr(c.cursor));
  c.cursor = w.begin + out.size();
  c.changed = c.line != line;
  return c;
}

}  // namespace mv::driftwire::cli
