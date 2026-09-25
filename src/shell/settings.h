// SPDX-License-Identifier: GPL-2.0-or-later
// Persisted view preferences for the Windows host.
//
// These are host chrome preferences, not core state: which strips the shell
// puts on screen. They live in shell/ because that is where the Win32 host
// lives (D9) — a Mac host keeps its own defaults rather than reading this file.
//
// Storage is `%LocalAppData%\MediaViewer\settings.ini`, held in memory by
// shell/settings_store.h. Every load_* below reads that in-memory document and
// every save_* queues a coalesced write on the store's persist worker: none of
// them touches the disk on the calling thread (rule 1). The overloads without a
// store use app_settings(). Nothing about the user's files is written here
// (rule 6) other than the F7 / F8 destination folders the user picked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shell/settings_store.h"

namespace mv::shell {

inline constexpr std::int32_t kSettingFilmstripFolder = 1 << 0;
inline constexpr std::int32_t kSettingFilmstripImage = 1 << 1;
inline constexpr std::int32_t kSettingWrap = 1 << 2;  // plan/16: wrap at folder ends
inline constexpr std::int32_t kSettingStickyZoom = 1 << 3;
inline constexpr std::int32_t kSettingBackgroundShift = 4;
inline constexpr std::int32_t kSettingBackgroundMask = 3 << kSettingBackgroundShift;

// A folder open is an explicit "show me this folder", so the filmstrip earns
// its 112 DIP. Opening one image is a viewing intent: the folder is still
// listed behind it (so arrows and the gallery work), but the strip does not
// take a slice of the canvas until asked.
struct view_settings {
  bool filmstrip_for_folder = true;
  bool filmstrip_for_image = false;
  // Arrow keys, Space and the slideshow go round from the last item to the
  // first. On by default (plan/16).
  bool wrap = true;
  bool sticky_zoom = false;
  std::uint8_t background = 0;  // 0 canvas, 1 gray, 2 white, 3 checkerboard
  // PR 9: the folder sort, packed by io::pack_sort (key in bits 0-2, descending
  // in bit 3). Not part of flags(): the chrome gets it beside them.
  std::int32_t sort = 0;

  [[nodiscard]] std::int32_t flags() const noexcept {
    return (filmstrip_for_folder ? kSettingFilmstripFolder : 0) |
           (filmstrip_for_image ? kSettingFilmstripImage : 0) |
           (wrap ? kSettingWrap : 0) |
           (sticky_zoom ? kSettingStickyZoom : 0) |
           ((static_cast<std::int32_t>(background) & 3) << kSettingBackgroundShift);
  }

  [[nodiscard]] static view_settings from_flags(std::int32_t flags) noexcept {
    view_settings s;
    s.filmstrip_for_folder = (flags & kSettingFilmstripFolder) != 0;
    s.filmstrip_for_image = (flags & kSettingFilmstripImage) != 0;
    s.wrap = (flags & kSettingWrap) != 0;
    s.sticky_zoom = (flags & kSettingStickyZoom) != 0;
    s.background = static_cast<std::uint8_t>((flags & kSettingBackgroundMask) >> kSettingBackgroundShift);
    return s;
  }
};

struct key_override {
  int row = 0;
  std::uint16_t k = 0;
  std::uint8_t mods = 0;
};

[[nodiscard]] std::vector<key_override> load_key_overrides(const settings_store& store) noexcept;
void save_key_overrides(settings_store& store, std::span<const key_override> list) noexcept;
[[nodiscard]] std::vector<key_override> load_key_overrides() noexcept;
void save_key_overrides(std::span<const key_override> list) noexcept;

// Never fails loudly: a missing or unreadable file is the defaults above.
[[nodiscard]] view_settings load_view_settings(const settings_store& store) noexcept;
void save_view_settings(settings_store& store, const view_settings& settings) noexcept;
[[nodiscard]] view_settings load_view_settings() noexcept;
void save_view_settings(const view_settings& settings) noexcept;

// F7 / F8 destinations, most recent first, at most five (plan/16). UTF-8
// folder paths the user picked; they stay in the local settings file.
inline constexpr std::size_t kMaxDestinations = 5;
// Read once at startup; the app keeps the list and saves it when it changes.
[[nodiscard]] std::vector<std::string> load_destinations(const settings_store& store) noexcept;
void save_destinations(settings_store& store, const std::vector<std::string>& list) noexcept;
[[nodiscard]] std::vector<std::string> load_destinations() noexcept;
void save_destinations(const std::vector<std::string>& list) noexcept;

// PR 15: the jump list's recent folders, most recent first, at most
// kMaxRecentFolders (os_integration.h, which also owns the MRU rule). Local
// only, like the destinations: a jump list never leaves the machine.
[[nodiscard]] std::vector<std::string> load_recent_folders(const settings_store& store) noexcept;
void save_recent_folders(settings_store& store, const std::vector<std::string>& list) noexcept;
[[nodiscard]] std::vector<std::string> load_recent_folders() noexcept;
void save_recent_folders(const std::vector<std::string>& list) noexcept;

// Pure: the list after `utf8_dir` was used. Moves it to the front, drops an
// earlier spelling of the same folder (Windows paths ignore ASCII case and a
// trailing separator), and keeps at most `max`.
[[nodiscard]] std::vector<std::string> push_destination(std::vector<std::string> list,
                                                        std::string_view utf8_dir,
                                                        std::size_t max = kMaxDestinations);

// Crash reporting (plan/13 Part 2), section [crash]. consent: -1 never asked,
// 0 declined, 1 accepted. upload_url: empty in PR 7 -- no endpoint exists, and
// nothing uploads without both a URL and consent. No path of a user file is
// ever stored here.
struct crash_settings {
  int consent = -1;
  std::string upload_url;
};
[[nodiscard]] crash_settings load_crash_settings(const settings_store& store) noexcept;
void save_crash_consent(settings_store& store, bool accepted) noexcept;
[[nodiscard]] crash_settings load_crash_settings() noexcept;
void save_crash_consent(bool accepted) noexcept;

// Pure. "Ask before the first send, plainly, once": only when there is
// something to send, somewhere to send it, and the user has never answered.
[[nodiscard]] inline bool should_ask_crash_consent(const crash_settings& s,
                                                   std::size_t pending_reports) noexcept {
  return s.consent < 0 && !s.upload_url.empty() && pending_reports > 0;
}

}  // namespace mv::shell
