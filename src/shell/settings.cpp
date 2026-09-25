// SPDX-License-Identifier: GPL-2.0-or-later
// Typed views over settings.ini. Every read here is the in-memory document and
// every save is a coalesced write on the store's persist worker
// (shell/settings_store.h) -- none of them touch the disk on the calling thread.
#include "shell/settings.h"

#include "io/sort_order.h"
#include "shell/os_integration.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace mv::shell {
namespace {

constexpr char kView[] = "view";
constexpr char kDestinations[] = "destinations";
constexpr char kRecent[] = "recent";
constexpr char kKeys[] = "keys";
constexpr char kCrash[] = "crash";

std::string_view trim_separator(std::string_view p) noexcept {
  while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.remove_suffix(1);
  return p;
}

bool same_folder(std::string_view a, std::string_view b) noexcept {
  a = trim_separator(a);
  b = trim_separator(b);
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x == '/') x = '\\';
    if (y == '/') y = '\\';
    if (x != y) return false;
  }
  return true;
}

}  // namespace

std::vector<std::string> push_destination(std::vector<std::string> list, std::string_view utf8_dir,
                                          std::size_t max) {
  const std::string_view dir = trim_separator(utf8_dir);
  if (dir.empty() || max == 0) return list;
  std::vector<std::string> out;
  out.reserve(std::min(list.size() + 1, max));
  out.emplace_back(dir);
  for (auto& d : list) {
    if (out.size() >= max) break;
    if (d.empty() || same_folder(d, dir)) continue;
    out.push_back(std::move(d));
  }
  return out;
}

view_settings load_view_settings(const settings_store& store) noexcept {
  view_settings s;
  const auto doc = store.snapshot();
  if (!doc) return s;
  s.filmstrip_for_folder = doc->get_int(kView, "filmstrip_for_folder", s.filmstrip_for_folder ? 1 : 0) != 0;
  s.filmstrip_for_image = doc->get_int(kView, "filmstrip_for_image", s.filmstrip_for_image ? 1 : 0) != 0;
  s.wrap = doc->get_int(kView, "wrap", s.wrap ? 1 : 0) != 0;
  s.sticky_zoom = doc->get_int(kView, "sticky_zoom", s.sticky_zoom ? 1 : 0) != 0;
  const int bg = doc->get_int(kView, "background", s.background);
  s.background = static_cast<std::uint8_t>(bg < 0 ? 0 : bg > 3 ? 3 : bg);
  // Normalised, so a hand-edited or future value cannot leave an unknown key.
  s.sort = io::pack_sort(io::unpack_sort(doc->get_int(kView, "sort", 0)));
  return s;
}

void save_view_settings(settings_store& store, const view_settings& s) noexcept {
  store.update([&](settings_doc& d) {
    d.set(kView, "filmstrip_for_folder", s.filmstrip_for_folder ? "1" : "0");
    d.set(kView, "filmstrip_for_image", s.filmstrip_for_image ? "1" : "0");
    d.set(kView, "wrap", s.wrap ? "1" : "0");
    d.set(kView, "sticky_zoom", s.sticky_zoom ? "1" : "0");
    d.set(kView, "background", std::to_string(static_cast<unsigned>(s.background & 3)));
    d.set(kView, "sort", std::to_string(s.sort));
  });
}

std::vector<std::string> load_destinations(const settings_store& store) noexcept {
  try {
    std::vector<std::string> out;
    const auto doc = store.snapshot();
    if (!doc) return out;
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
      const std::string* v = doc->find(kDestinations, "d" + std::to_string(i));
      if (v && !v->empty()) out.push_back(*v);
    }
    return out;
  } catch (...) {
    return {};
  }
}

void save_destinations(settings_store& store, const std::vector<std::string>& list) noexcept {
  store.update([&](settings_doc& d) {
    d.erase_prefix(kDestinations, "d");
    for (std::size_t i = 0; i < kMaxDestinations && i < list.size(); ++i) {
      if (!list[i].empty()) d.set(kDestinations, "d" + std::to_string(i), list[i]);
    }
  });
}

std::vector<std::string> load_recent_folders(const settings_store& store) noexcept {
  try {
    std::vector<std::string> out;
    const auto doc = store.snapshot();
    if (!doc) return out;
    for (std::size_t i = 0; i < kMaxRecentFolders; ++i) {
      const std::string* v = doc->find(kRecent, "f" + std::to_string(i));
      if (v && !v->empty()) out.push_back(*v);
    }
    return out;
  } catch (...) {
    return {};
  }
}

void save_recent_folders(settings_store& store, const std::vector<std::string>& list) noexcept {
  store.update([&](settings_doc& d) {
    d.erase_prefix(kRecent, "f");
    for (std::size_t i = 0; i < kMaxRecentFolders && i < list.size(); ++i) {
      if (!list[i].empty()) d.set(kRecent, "f" + std::to_string(i), list[i]);
    }
  });
}

std::vector<key_override> load_key_overrides(const settings_store& store) noexcept {
  try {
    std::vector<key_override> out;
    const auto doc = store.snapshot();
    if (!doc) return out;
    const int n = doc->get_int(kKeys, "n", 0);
    for (int i = 0; i < n && i < 255; ++i) {
      const std::string* v = doc->find(kKeys, "r" + std::to_string(i));
      if (!v || v->empty()) continue;
      int row = 0, k = 0, mods = 0;
      if (std::sscanf(v->c_str(), "%d,%d,%d", &row, &k, &mods) != 3) continue;
      if (row < 0 || k <= 0 || k > 0xFFFF || mods < 0 || mods >= 8) continue;
      out.push_back(key_override{row, static_cast<std::uint16_t>(k), static_cast<std::uint8_t>(mods)});
    }
    return out;
  } catch (...) {
    return {};
  }
}

void save_key_overrides(settings_store& store, std::span<const key_override> list) noexcept {
  store.update([&](settings_doc& d) {
    d.erase_prefix(kKeys, "");
    const std::size_t n = std::min<std::size_t>(list.size(), 255);
    d.set(kKeys, "n", std::to_string(n));
    for (std::size_t i = 0; i < n; ++i) {
      char value[40]{};
      (void)std::snprintf(value, sizeof value, "%d,%u,%u", list[i].row, static_cast<unsigned>(list[i].k),
                          static_cast<unsigned>(list[i].mods));
      d.set(kKeys, "r" + std::to_string(i), value);
    }
  });
}

crash_settings load_crash_settings(const settings_store& store) noexcept {
  try {
    crash_settings out;
    const auto doc = store.snapshot();
    if (!doc) return out;
    const int c = doc->get_int(kCrash, "consent", -1);
    out.consent = c < 0 ? -1 : (c > 0 ? 1 : 0);
    out.upload_url = doc->get(kCrash, "upload_url");
    return out;
  } catch (...) {
    return {};
  }
}

void save_crash_consent(settings_store& store, bool accepted) noexcept {
  store.set(kCrash, "consent", accepted ? "1" : "0");
}

std::vector<key_override> load_key_overrides() noexcept { return load_key_overrides(app_settings()); }
void save_key_overrides(std::span<const key_override> list) noexcept { save_key_overrides(app_settings(), list); }
view_settings load_view_settings() noexcept { return load_view_settings(app_settings()); }
void save_view_settings(const view_settings& s) noexcept { save_view_settings(app_settings(), s); }
std::vector<std::string> load_destinations() noexcept { return load_destinations(app_settings()); }
void save_destinations(const std::vector<std::string>& list) noexcept { save_destinations(app_settings(), list); }
std::vector<std::string> load_recent_folders() noexcept { return load_recent_folders(app_settings()); }
void save_recent_folders(const std::vector<std::string>& list) noexcept { save_recent_folders(app_settings(), list); }
crash_settings load_crash_settings() noexcept { return load_crash_settings(app_settings()); }
void save_crash_consent(bool accepted) noexcept { save_crash_consent(app_settings(), accepted); }

}  // namespace mv::shell
