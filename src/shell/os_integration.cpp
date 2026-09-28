// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/os_integration.h"

#include <algorithm>

namespace mv::shell {
namespace {

bool is_separator(char c) noexcept { return c == '/' || c == '\\'; }

// "D:\" and "/" keep their separator; anything longer loses a trailing one.
std::string_view trim_separator(std::string_view p) noexcept {
  while (p.size() > 1 && is_separator(p.back())) {
    if (p.size() == 3 && p[1] == ':') break;  // "D:\"
    p.remove_suffix(1);
  }
  return p;
}

char fold(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  if (c == '\\') return '/';
  return c;
}

bool same_folder(std::string_view a, std::string_view b) noexcept {
  a = trim_separator(a);
  b = trim_separator(b);
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return fold(x) == fold(y); });
}

// `parent` is `home` or inside it. Case-insensitive (ASCII) and either
// separator, as same_folder is: Windows paths ("C:\Users\Ana" against
// "c:\users\ana\Pictures") and the Mac's case-insensitive volumes alike.
bool under_home(std::string_view parent, std::string_view home) noexcept {
  if (home.size() <= 1 || parent.size() < home.size()) return false;
  if (!std::equal(home.begin(), home.end(), parent.begin(),
                  [](char x, char y) { return fold(x) == fold(y); })) {
    return false;
  }
  return parent.size() == home.size() || is_separator(parent[home.size()]);
}

}  // namespace

std::vector<std::string> push_recent_folder(std::vector<std::string> list, std::string_view utf8_dir,
                                            std::size_t max) {
  std::string dir(trim_separator(utf8_dir));
  if (dir.empty() || max == 0) return list;
  if (dir.size() == 2 && dir[1] == ':') dir.push_back('\\');  // "D:" is the drive's current folder; the root is "D:\".
  std::erase_if(list, [&](const std::string& p) { return p.empty() || same_folder(p, dir); });
  list.insert(list.begin(), std::move(dir));
  if (list.size() > max) list.resize(max);
  return list;
}

std::string folder_display_name(std::string_view utf8_dir) {
  const std::string_view dir = trim_separator(utf8_dir);
  const std::size_t sep = dir.find_last_of("/\\");
  if (sep == std::string_view::npos || sep + 1 == dir.size()) return std::string(dir);
  return std::string(dir.substr(sep + 1));
}

std::vector<std::string> recent_folder_labels(std::span<const std::string> utf8_dirs) {
  std::vector<std::string> labels;
  labels.reserve(utf8_dirs.size());
  for (const std::string& d : utf8_dirs) labels.push_back(folder_display_name(d));
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const std::string name = folder_display_name(utf8_dirs[i]);
    const bool shared = std::count_if(utf8_dirs.begin(), utf8_dirs.end(), [&](const std::string& d) {
                          return folder_display_name(d) == name;
                        }) > 1;
    if (!shared) continue;
    const std::string_view dir = trim_separator(utf8_dirs[i]);
    const std::size_t sep = dir.find_last_of("/\\");
    if (sep == std::string_view::npos || sep == 0) continue;
    const std::string parent = folder_display_name(dir.substr(0, sep));
    if (!parent.empty() && parent != "/") labels[i] = name + " \u2014 " + parent;
  }
  return labels;
}

namespace {

// `src` into `dst` (`cap` bytes), NUL-terminated, cut before a character that
// would not fit.
void copy_utf8(std::string_view src, char* dst, std::size_t cap) noexcept {
  if (cap == 0) return;
  std::size_t n = std::min(src.size(), cap - 1);
  if (n < src.size()) {
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u) --n;
  }
  std::copy_n(src.data(), n, dst);
  dst[n] = '\0';
}

}  // namespace

void fill_welcome_recents(std::span<const std::string> utf8_dirs, std::string_view home,
                          welcome_recents& out) noexcept {
  out.count = 0;
  out.hover = -1;
  home = trim_separator(home);
  for (const std::string& d : utf8_dirs) {
    if (out.count >= welcome_recents::kMax) break;
    const std::string_view dir = trim_separator(d);
    if (dir.empty()) continue;
    const int i = out.count++;
    // A root ("/", "D:\") is its own label and lives nowhere.
    const std::size_t sep = dir.find_last_of("/\\");
    if (sep == std::string_view::npos || sep + 1 == dir.size()) {
      copy_utf8(dir, out.label[i], sizeof(out.label[i]));
      out.where[i][0] = '\0';
      continue;
    }
    copy_utf8(dir.substr(sep + 1), out.label[i], sizeof(out.label[i]));
    std::string_view parent = dir.substr(0, sep == 0 ? 1 : sep);
    if (parent.size() == 2 && parent[1] == ':') parent = dir.substr(0, 3);  // "D:\"
    if (under_home(parent, home)) {
      out.where[i][0] = '~';
      copy_utf8(parent.substr(home.size()), out.where[i] + 1, sizeof(out.where[i]) - 1);
    } else {
      copy_utf8(parent, out.where[i], sizeof(out.where[i]));
    }
  }
}

std::string paths_as_text(std::span<const std::string> utf8_paths, std::string_view newline) {
  std::string out;
  for (const std::string& p : utf8_paths) {
    if (p.empty()) continue;
    if (!out.empty()) out += newline;
    out += p;
  }
  return out;
}

std::vector<std::string> parse_shellext_file_list(std::string_view text) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (line == "." || line == ".." || line.find_first_of("/\\:") != std::string_view::npos) return {};
    out.emplace_back(line);
  }
  return out;
}

shellext_plan plan_shellext_install(std::string_view version, std::span<const std::string> existing) {
  shellext_plan plan;
  const bool plain = !version.empty() && version != "." && version != ".." &&
                     std::all_of(version.begin(), version.end(), [](char c) {
                       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              c == '.' || c == '-' || c == '+';
                     });
  if (!plain) return plan;
  plan.version_dir = std::string(version);
  for (const std::string& e : existing) {
    if (!e.empty() && e != plan.version_dir) plan.prune.push_back(e);
  }
  return plan;
}

}  // namespace mv::shell
