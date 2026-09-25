// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/open_request.h"

namespace mv::shell {
namespace {

bool is_space(wchar_t c) noexcept { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

// "C:\" and "\\?\C:\" are roots; stripping their separator changes meaning.
bool is_root(std::wstring_view p) noexcept {
  if (p.size() == 3 && p[1] == L':' && p[2] == L'\\') return true;
  if (p.size() == 7 && p.substr(0, 4) == L"\\\\?\\" && p[5] == L':' && p[6] == L'\\') return true;
  return false;
}

}  // namespace

std::wstring normalize_open_path(std::wstring_view raw) {
  std::size_t b = 0;
  std::size_t e = raw.size();
  while (b < e && is_space(raw[b])) ++b;
  while (e > b && is_space(raw[e - 1])) --e;
  std::wstring_view v = raw.substr(b, e - b);
  if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);

  std::wstring out(v);
  const bool long_prefix = out.rfind(L"\\\\?\\", 0) == 0;
  for (std::size_t i = long_prefix ? 4 : 0; i < out.size(); ++i) {
    if (out[i] == L'/') out[i] = L'\\';
  }
  while (out.size() > 1 && out.back() == L'\\' && !is_root(out)) out.pop_back();
  return out;
}

open_request resolve_open(std::span<const path_probe> candidates) {
  if (candidates.empty()) return {};
  for (const path_probe& c : candidates) {
    if (!c.exists || c.path.empty()) continue;
    return {c.is_directory ? open_kind::folder : open_kind::file, c.path};
  }
  return {open_kind::missing, {}};
}

}  // namespace mv::shell
