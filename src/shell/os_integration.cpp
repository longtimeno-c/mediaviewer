// SPDX-License-Identifier: GPL-2.0-or-later
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

}  // namespace

std::vector<std::string> push_recent_folder(std::vector<std::string> list, std::string_view utf8_dir,
                                            std::size_t max) {
  const std::string_view dir = trim_separator(utf8_dir);
  if (dir.empty() || max == 0) return list;
  std::erase_if(list, [&](const std::string& p) { return p.empty() || same_folder(p, dir); });
  list.insert(list.begin(), std::string(dir));
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
