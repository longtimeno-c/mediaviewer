// SPDX-License-Identifier: GPL-2.0-or-later
// Persisted view preferences for the Windows host.
//
// These are host chrome preferences, not core state: which strips the shell
// puts on screen. They live in shell/ because that is where the Win32 host
// lives (D9) — a Mac host keeps its own defaults rather than reading this file.
//
// Storage is `%LocalAppData%\MediaViewer\settings.ini`. Nothing about the
// user's files is written here (rule 6) — only the toggles below.
#pragma once

#include <cstdint>

namespace mv::shell {

inline constexpr std::int32_t kSettingFilmstripFolder = 1 << 0;
inline constexpr std::int32_t kSettingFilmstripImage = 1 << 1;

// A folder open is an explicit "show me this folder", so the filmstrip earns
// its 112 DIP. Opening one image is a viewing intent: the folder is still
// listed behind it (so arrows and the gallery work), but the strip does not
// take a slice of the canvas until asked.
struct view_settings {
  bool filmstrip_for_folder = true;
  bool filmstrip_for_image = false;

  [[nodiscard]] std::int32_t flags() const noexcept {
    return (filmstrip_for_folder ? kSettingFilmstripFolder : 0) |
           (filmstrip_for_image ? kSettingFilmstripImage : 0);
  }

  [[nodiscard]] static view_settings from_flags(std::int32_t flags) noexcept {
    view_settings s;
    s.filmstrip_for_folder = (flags & kSettingFilmstripFolder) != 0;
    s.filmstrip_for_image = (flags & kSettingFilmstripImage) != 0;
    return s;
  }
};

// Never fails loudly: a missing or unreadable file is the defaults above.
[[nodiscard]] view_settings load_view_settings() noexcept;
void save_view_settings(const view_settings& settings) noexcept;

}  // namespace mv::shell
