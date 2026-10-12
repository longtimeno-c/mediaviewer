// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/ai_resume.h"

#include "core/json.h"
#include "io/file.h"

namespace mv::addon {

std::vector<unfinished_root> parse_ai_resume(std::string_view text) {
  std::vector<unfinished_root> out;
  const auto doc = json::parse(text);
  if (!doc || doc->integer("version").value_or(0) != 1) return out;
  const json::value* roots = doc->find("roots");
  if (!roots || roots->k != json::kind::array) return out;
  for (const json::value& r : roots->a) {
    const auto id = r.integer("id");
    const std::string* path = r.str("path");
    if (!id || *id <= 0 || !path || path->empty()) continue;
    unfinished_root u;
    u.id = *id;
    u.path = *path;
    const std::string* kind = r.str("kind");
    u.photos = kind && *kind == "photos";
    u.done = r.integer("done").value_or(0);
    u.total = r.integer("total").value_or(0);
    out.push_back(std::move(u));
  }
  return out;
}

std::vector<unfinished_root> read_ai_resume(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  std::string path = data_dir;
  if (path.back() != '/' && path.back() != '\\') path += '/';
  path += "resume.json";
  auto bytes = io::read_all(path);
  if (!bytes || bytes->empty() || bytes->size() > (1u << 20)) return {};
  return parse_ai_resume(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

std::string unfinished_name(const unfinished_root& root) {
  if (root.photos) return "Photos";
  std::string_view p = root.path;
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.remove_suffix(1);
  const std::size_t slash = p.find_last_of("/\\");
  std::string_view name = slash == std::string_view::npos ? p : p.substr(slash + 1);
  return std::string(name.empty() ? p : name);
}

std::string resume_question(const std::vector<unfinished_root>& roots) {
  if (roots.empty()) return {};
  std::string names = unfinished_name(roots[0]);
  if (roots.size() == 2) {
    names += " and " + unfinished_name(roots[1]);
  } else if (roots.size() > 2) {
    names += ", " + unfinished_name(roots[1]) + " and " + std::to_string(roots.size() - 2) + " more";
  }
  return "Resume indexing " + names + "?";
}

}  // namespace mv::addon
