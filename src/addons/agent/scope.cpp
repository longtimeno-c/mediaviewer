// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/agent/scope.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>

namespace mv::agent {
namespace {

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const char a = static_cast<char>(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    if (a != prefix[i]) return false;
  }
  return true;
}

bool absolute(std::string_view p) {
  if (p.empty()) return false;
  if (p[0] == '/') return true;
  // C:\ or C:/ ; \\server\share
  if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
      (p[2] == '\\' || p[2] == '/')) {
    return true;
  }
  return p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
}

}  // namespace

std::string comparable(std::string_view path) {
  if (!absolute(path)) return {};
  // UTF-8 in, UTF-8 out (std::filesystem's narrow strings are UTF-8 on POSIX;
  // on Windows u8path / u8string keep it so).
  std::error_code ec;
#if defined(_WIN32)
  // u8path is deprecated in C++20: build from char8_t.
  std::filesystem::path p{std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())};
#else
  std::filesystem::path p{std::string(path)};
#endif
  std::filesystem::path r = std::filesystem::weakly_canonical(p, ec);
  if (ec) r = p.lexically_normal();
#if defined(_WIN32)
  const auto u8 = r.u8string();
  std::string s(u8.begin(), u8.end());
#else
  std::string s = r.string();
#endif
  std::replace(s.begin(), s.end(), '\\', '/');
  while (s.size() > 1 && s.back() == '/') s.pop_back();
#if defined(_WIN32) || defined(__APPLE__)
  // APFS / HFS+ and NTFS fold case by default.
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
#endif
  return s;
}

scope::scope(std::vector<std::string> media, std::vector<std::string> folders)
    : media_(std::move(media)), folders_(std::move(folders)) {
  for (const std::string& m : media_) {
    if (starts_with_ci(m, "photos:")) continue;  // never in scope, even if a timeline named one
    const std::string k = comparable(m);
    if (!k.empty()) media_key_.push_back(k);
  }
  for (const std::string& f : folders_) {
    const std::string k = comparable(f);
    if (k.empty() || k == "/" || (k.size() <= 3 && k.size() >= 2 && k[1] == ':')) continue;  // never a whole disk
    folder_key_.push_back(k);
  }
}

scope::verdict scope::check(std::string_view path) const {
  if (path.empty()) return verdict::empty;
  if (starts_with_ci(path, "photos:")) return verdict::photos;
  const std::string k = comparable(path);
  if (k.empty()) return verdict::relative;
  for (const std::string& m : media_key_) {
    if (k == m) return verdict::inside;
  }
  for (const std::string& f : folder_key_) {
    if (k.size() > f.size() && k.compare(0, f.size(), f) == 0 && k[f.size()] == '/') return verdict::inside;
    if (k == f) return verdict::inside;
  }
  return verdict::outside;
}

std::string scope::refusal(verdict v) {
  switch (v) {
    case verdict::inside: return {};
    case verdict::photos:
      return "refused: the Photos library is not in scope. This agent works only with the timeline's own media "
             "and the folders the editor gave in the MediaViewer panel";
    case verdict::relative: return "refused: give an absolute path to a file on the timeline or in a given folder";
    case verdict::empty: return "refused: no path given";
    case verdict::outside:
      return "refused: that file is outside the scope (the timeline's own media and the folders the editor gave "
             "in the MediaViewer panel). Ask the editor to add its folder in the panel if it should be used";
  }
  return "refused";
}

std::vector<std::string> scope::media_dirs() const {
  std::set<std::string> dirs;
  for (const std::string& m : media_) {
    const std::size_t cut = m.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) dirs.insert(m.substr(0, cut));
  }
  return std::vector<std::string>(dirs.begin(), dirs.end());
}

}  // namespace mv::agent
