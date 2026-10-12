// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/rename_stop.h"

#include <algorithm>
#include <array>

#include "io/file_port.h"

namespace mv::io {
namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// ASCII case-insensitive: the same file on a case-insensitive volume (APFS,
// NTFS as both platforms ship them), so a case-only rename is not a collision.
bool same_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// ".CR2" from "IMG_1234.CR2"; "" with no extension (a leading dot is the name).
std::string_view extension_of(std::string_view name) noexcept {
  const auto dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return {};
  return name.substr(dot);
}

bool is_device_name(std::string_view stem) noexcept {
  // Windows reserves these with any extension; compare the part before a dot.
  const auto dot = stem.find('.');
  const std::string_view base = dot == std::string_view::npos ? stem : stem.substr(0, dot);
  static constexpr std::array<std::string_view, 4> plain = {"con", "prn", "aux", "nul"};
  for (const auto d : plain) {
    if (same_ignoring_case(base, d)) return true;
  }
  if (base.size() == 4 && base[3] >= '1' && base[3] <= '9') {
    const std::string_view head = base.substr(0, 3);
    if (same_ignoring_case(head, "com") || same_ignoring_case(head, "lpt")) return true;
  }
  return false;
}

// One file of the group: where it is, and how its new name is made from a stem.
struct member {
  std::string from;
  std::string suffix;  // what follows the stem: ".CR2", ".xmp", ".CR2.xmp"
};

}  // namespace

stem_problem check_stem(std::string_view stem) noexcept {
  if (stem.empty()) return stem_problem::empty;
  if (stem.find_first_not_of(' ') == std::string_view::npos) return stem_problem::empty;
  if (stem.size() > kMaxStemBytes) return stem_problem::too_long;
  for (const char c : stem) {
    if (c == '/' || c == '\\') return stem_problem::separator;
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return stem_problem::reserved;
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
      return stem_problem::reserved;
    }
  }
  if (stem == "." || stem == ".." || stem.back() == '.' || stem.back() == ' ') return stem_problem::dots;
  if (is_device_name(stem)) return stem_problem::device;
  return stem_problem::ok;
}

const char* stem_problem_text(stem_problem p) noexcept {
  switch (p) {
    case stem_problem::ok: return "";
    case stem_problem::empty: return "A name cannot be empty";
    case stem_problem::separator: return "A name cannot contain / or \\";
    case stem_problem::reserved: return "A name cannot contain < > : \" | ? *";
    case stem_problem::dots: return "A name cannot end with a dot or a space";
    case stem_problem::device: return "That name is reserved by Windows";
    case stem_problem::too_long: return "That name is too long";
  }
  return "";
}

std::string_view stem_of(std::string_view utf8_path) noexcept {
  const std::string_view name = file_name_of(utf8_path);
  const std::string_view ext = extension_of(name);
  return name.substr(0, name.size() - ext.size());
}

std::vector<rename_move> plan_rename(std::string_view primary, std::string_view secondary,
                                     std::string_view new_stem,
                                     const std::function<bool(std::string_view)>& exists) {
  if (primary.empty() || new_stem.empty() || !exists(primary)) return {};
  const std::string_view dir = parent_of(primary);

  std::vector<member> group;
  const auto add = [&](std::string from, std::string suffix) {
    for (const auto& m : group) {
      if (m.from == from) return;  // IMG_1234.xmp is shared by the pair's halves
    }
    if (!exists(from)) return;
    group.push_back({std::move(from), std::move(suffix)});
  };
  const std::string_view sources[] = {primary, secondary};
  for (const std::string_view src : sources) {
    if (src.empty()) continue;
    const std::string_view ext = extension_of(file_name_of(src));
    add(std::string(src), std::string(ext));
  }
  for (const std::string_view src : sources) {
    if (src.empty()) continue;
    const std::string_view ext = extension_of(file_name_of(src));
    add(join_path(parent_of(src), std::string(stem_of(src)) + ".xmp"), ".xmp");  // ours, Lightroom's
    add(std::string(src) + ".xmp", std::string(ext) + ".xmp");                  // darktable's
  }

  const auto is_source = [&](std::string_view path) {
    return std::any_of(group.begin(), group.end(),
                       [&](const member& m) { return same_ignoring_case(m.from, path); });
  };
  for (int n = 1; n <= 9999; ++n) {
    std::string stem(new_stem);
    if (n > 1) stem += " (" + std::to_string(n) + ")";
    std::vector<rename_move> moves;
    moves.reserve(group.size());
    bool free = true;
    bool changes = false;
    for (const auto& m : group) {
      std::string to = join_path(dir, stem + m.suffix);
      if (to != m.from) changes = true;
      if (to != m.from && exists(to) && !is_source(to)) {
        free = false;
        break;
      }
      moves.push_back({m.from, std::move(to)});
    }
    if (!free) continue;
    if (!changes) return {};
    // Members already named so stay put; only what changes moves.
    std::erase_if(moves, [](const rename_move& mv) { return mv.from == mv.to; });
    return moves;
  }
  return {};
}

result<rename_result> rename_stop(std::string_view primary, std::string_view secondary,
                                  std::string_view new_stem, const rename_before_fn& before) noexcept {
  try {
    if (check_stem(new_stem) != stem_problem::ok) return err(status::invalid_arg);
    const auto exists = [](std::string_view p) { return static_cast<bool>(stat_path(p)); };
    if (!exists(primary)) return err(status::not_found);
    const std::vector<rename_move> plan = plan_rename(primary, secondary, new_stem, exists);
    if (plan.empty()) {
      // Nothing changes (the same name), or every "(n)" up to the cap is taken.
      return err(std::string(stem_of(primary)) == new_stem ? status::invalid_arg : status::io);
    }

    if (before) {
      for (const auto& m : plan) {
        if (m.from == primary) before(m.from, m.to);
      }
    }
    std::vector<rename_move> done;
    const auto put_back = [&] {
      for (auto it = done.rbegin(); it != done.rend(); ++it) (void)rename_no_replace(it->to, it->from);
    };
    for (const auto& m : plan) {
      if (same_ignoring_case(m.from, m.to)) {
        // Case only: the volume sees one name, so through a free temporary.
        std::string tmp;
        for (int n = 0; n < 100 && tmp.empty(); ++n) {
          std::string c = m.to + ".mvrename" + std::to_string(n);
          if (!exists(c)) tmp = std::move(c);
        }
        auto a = tmp.empty() ? result<rename_outcome>(err(status::io)) : rename_no_replace(m.from, tmp);
        if (!a || *a != rename_outcome::renamed) {
          put_back();
          return err(status::io);
        }
        auto b = rename_no_replace(tmp, m.to);
        if (!b || *b != rename_outcome::renamed) {
          (void)rename_no_replace(tmp, m.from);
          put_back();
          return err(status::io);
        }
      } else {
        auto r = rename_no_replace(m.from, m.to);
        if (!r || *r != rename_outcome::renamed) {
          put_back();  // a race took the name, or the file would not move
          return err(status::io);
        }
      }
      done.push_back(m);
    }

    rename_result out;
    out.primary = std::string(primary);
    out.secondary = std::string(secondary);
    out.moved = done.size();
    for (const auto& m : done) {
      if (m.from == primary) out.primary = m.to;
      if (!secondary.empty() && m.from == secondary) out.secondary = m.to;
    }
    return out;
  } catch (...) {
    return err(status::internal);
  }
}

}  // namespace mv::io
