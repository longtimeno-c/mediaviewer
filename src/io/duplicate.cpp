// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/duplicate.h"

#include "io/file_port.h"
#include "io/verified_copy.h"

namespace mv::io {
namespace {

bool ascii_iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// meta/write.h sidecar_path_for's rule, kept here because io sits below meta:
// "IMG_1234.CR2" -> "IMG_1234.xmp"; ".hidden" is a name, not an extension.
std::string sidecar_of(std::string_view path) {
  std::string p(path);
  const std::size_t sep = p.find_last_of("/\\");
  const std::size_t name_start = sep == std::string::npos ? 0 : sep + 1;
  const std::size_t dot = p.find_last_of('.');
  if (dot != std::string::npos && dot > name_start) p.resize(dot);
  return p + ".xmp";
}

bool is_file(std::string_view path) {
  const auto st = stat_path(path);
  return st && !st->is_directory;
}

void remove_all(std::span<const std::string> paths) noexcept {
  for (const std::string& p : paths) (void)remove_file(p);
}

}  // namespace

std::string duplicate_name(std::string_view file_name, int n, duplicate_style style) {
  std::size_t dot = file_name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) dot = file_name.size();
  std::string out;
  out.reserve(file_name.size() + 16);
  out.append(file_name.substr(0, dot));
  if (style == duplicate_style::finder) {
    out.append(" copy");
    if (n > 1) {
      out.push_back(' ');
      out.append(std::to_string(n));
    }
  } else {
    out.append(" - Copy");
    if (n > 1) {
      out.append(" (");
      out.append(std::to_string(n));
      out.push_back(')');
    }
  }
  out.append(file_name.substr(dot));
  return out;
}

std::vector<std::string> duplicate_group(std::string_view primary_utf8,
                                         std::string_view secondary_utf8) {
  std::vector<std::string> group;
  if (primary_utf8.empty()) return group;
  group.emplace_back(primary_utf8);
  if (!secondary_utf8.empty()) group.emplace_back(secondary_utf8);
  // One sidecar at most: a pair's two stems normally match, and on a
  // case-insensitive volume "IMG.xmp" and "img.xmp" are the same file.
  std::string sidecar = sidecar_of(primary_utf8);
  if (ascii_iequal(sidecar, primary_utf8) || !is_file(sidecar)) {
    sidecar.clear();
    if (!secondary_utf8.empty()) {
      std::string other = sidecar_of(secondary_utf8);
      if (!ascii_iequal(other, secondary_utf8) && is_file(other)) sidecar = std::move(other);
    }
  }
  if (!sidecar.empty()) group.push_back(std::move(sidecar));
  return group;
}

result<std::vector<std::string>> duplicate_files(std::span<const std::string> group,
                                                 duplicate_style style) {
  if (group.empty()) return err(status::invalid_arg);
  const std::string_view dir = parent_of(group.front());
  if (dir.empty()) return err(status::invalid_arg);
  std::vector<std::string> names;
  names.reserve(group.size());
  for (const std::string& path : group) {
    if (parent_of(path) != dir || !is_file(path)) return err(status::invalid_arg);
    names.emplace_back(file_name_of(path));
  }
  const auto taken = [dir](std::string_view name) {
    return stat_path(join_path(dir, name)).has_value();
  };

  std::vector<std::string> written;
  written.reserve(group.size());
  int first = 1;
  for (;;) {
    const int n = first_free_duplicate(names, style, taken, first);
    if (n == 0) return err(status::io);
    written.clear();
    bool raced = false;
    for (std::size_t i = 0; i < group.size(); ++i) {
      const std::string target = join_path(dir, duplicate_name(names[i], n, style));
      copy_options options;
      copy_profile_for(group[i], target).apply(options);
      const std::string targets[] = {target};
      const auto copied = verified_copy(group[i], targets, options);
      if (copied && copy_succeeded(copied->targets[0].outcome)) {
        written.push_back(target);
        continue;
      }
      remove_all(written);
      // Something took the name after the check: never an overwrite, the
      // whole group moves on to the next number.
      if (copied && copied->targets[0].outcome == copy_target_outcome::name_taken) {
        raced = true;
        break;
      }
      return err(copied ? status::io : copied.error());
    }
    if (!raced) return written;
    first = n + 1;
  }
}

}  // namespace mv::io
