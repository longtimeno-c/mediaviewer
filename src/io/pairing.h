// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Scan-time pairing and companion filtering (docs/design/04, PR 7).
//
// RAW+JPEG: same basename, one filmstrip stop, JPEG/HEIC is the primary.
// Live Photo: HEIC+MOV same basename, still is primary, motion is the pair.
// iPhones set to "Most Compatible" write JPG+MOV for the same Live Photo, so a
// JPEG+MOV of one stem pairs the same way (docs/design/04 names HEIC; the JPG case is
// a PR 7 call, flagged for the decision log). MP4 never pairs.
// Ambiguous groups (three files, mixed stems) stay separate — never hide a
// file. Portable; no windows.h (D9).
//
// Stems match case-insensitively for ASCII only. DCF camera names are upper-
// case ASCII, so a real pair always matches; two non-ASCII stems that differ
// only in case are left as two stops, which is the safe failure (nothing
// hidden). Cost is O(n log n): the watcher re-runs this on every relist of a
// 2000-file dump.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "io/dir.h"

namespace mv::io {

enum class pair_kind : std::uint32_t {
  none = 0,
  raw_jpeg = 1,
  live_photo = 2,
};

struct listed_item {
  dir_entry primary;
  dir_entry secondary;  // empty path when unpaired
  pair_kind kind = pair_kind::none;
};

namespace pairing_detail {

inline void ascii_lower_inplace(std::string& s) noexcept {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
}

inline std::string_view extension(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
  return name.substr(dot);
}

inline std::string_view stem(std::string_view name) noexcept {
  const auto slash = name.find_last_of("\\/");
  const std::string_view base = slash == std::string_view::npos ? name : name.substr(slash + 1);
  const auto dot = base.find_last_of('.');
  if (dot == std::string_view::npos) return base;
  return base.substr(0, dot);
}

inline bool eq_lower(std::string_view a, const char* b) noexcept {
  std::size_t i = 0;
  for (; b[i] != '\0'; ++i) {
    if (i >= a.size()) return false;
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    char d = b[i];
    if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
    if (c != d) return false;
  }
  return i == a.size();
}

inline bool is_jpeg(std::string_view ext) noexcept {
  return eq_lower(ext, ".jpg") || eq_lower(ext, ".jpeg");
}
inline bool is_heic(std::string_view ext) noexcept {
  return eq_lower(ext, ".heic") || eq_lower(ext, ".heif") || eq_lower(ext, ".hif");
}
inline bool is_still_pair(std::string_view ext) noexcept { return is_jpeg(ext) || is_heic(ext); }
inline bool is_mov(std::string_view ext) noexcept { return eq_lower(ext, ".mov"); }
inline bool is_raw(std::string_view ext) noexcept {
  static constexpr const char* k[] = {
      ".cr2", ".cr3", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".orf", ".raf", ".rw2",
      ".pef", ".ptx", ".srw", ".rwl", ".dng", ".3fr", ".fff", ".iiq", ".mef", ".mos", ".raw",
  };
  for (const char* e : k) {
    if (eq_lower(ext, e)) return true;
  }
  return false;
}

inline std::string key_of(std::string_view name) {
  std::string k(stem(name));
  ascii_lower_inplace(k);
  return k;
}

}  // namespace pairing_detail

// An audio file (MP3, iTunes M4A / M4P), by its extension. Played through
// player/ like a clip, with its cover art or a card as the picture
// (docs/plans/audio-and-documents.md §2.2).
[[nodiscard]] inline bool is_audio_name(std::string_view name) noexcept {
  static constexpr const char* k[] = {".mp3", ".m4a", ".m4p"};
  const std::string_view ext = pairing_detail::extension(name);
  for (const char* e : k) {
    if (pairing_detail::eq_lower(ext, e)) return true;
  }
  return false;
}

// A document (PDF, DOCX), by its extension. Decoded page by page as a still
// (docs/design/04 "Pages").
[[nodiscard]] inline bool is_document_name(std::string_view name) noexcept {
  static constexpr const char* k[] = {".pdf", ".docx"};
  const std::string_view ext = pairing_detail::extension(name);
  for (const char* e : k) {
    if (pairing_detail::eq_lower(ext, e)) return true;
  }
  return false;
}

// Kinds a listing can leave out (Settings, "Show audio files" / "Show
// documents"). Bits of one word so a host can pass it through the ABI as is.
inline constexpr std::uint32_t kHideAudio = 1u << 0;
inline constexpr std::uint32_t kHideDocuments = 1u << 1;

// True when `name` is of a kind `hide` leaves out of a folder listing.
[[nodiscard]] inline bool is_hidden_kind(std::string_view name, std::uint32_t hide) noexcept {
  if (hide == 0) return false;
  return ((hide & kHideAudio) != 0 && is_audio_name(name)) ||
         ((hide & kHideDocuments) != 0 && is_document_name(name));
}

// Played through player/ rather than decoded as a still, by its extension: a
// clip in the D5 video containers, or an audio file. Same set as dir_win.cpp /
// dir_mac.cpp's; the player probes the real container when it opens the file.
// Drives the gallery's play badge and the hosts' clip-vs-still routing, never
// a decode.
[[nodiscard]] inline bool is_video_name(std::string_view name) noexcept {
  static constexpr const char* k[] = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".ts", ".m4v"};
  const std::string_view ext = pairing_detail::extension(name);
  for (const char* e : k) {
    if (pairing_detail::eq_lower(ext, e)) return true;
  }
  return is_audio_name(name);
}

// A RAW by its extension. The listing already filters by extension; this only
// drives the filmstrip badge, never a decode (decode probes magic bytes).
[[nodiscard]] inline bool is_raw_name(std::string_view name) noexcept {
  return pairing_detail::is_raw(pairing_detail::extension(name));
}

// Issue #206, the kind filter (View > Show): more bits of the same word, set
// for as long as the user asks, never saved. Each stop is exactly one of
// audio, document, RAW, clip or still, so a filter is the bits of every kind
// it does not show. kHideUnmarked is the caller's: it holds the set of paths
// that stay (the marks when "Marked" was chosen); it is not a kind of name.
inline constexpr std::uint32_t kHideStills = 1u << 2;    // JPEG, HEIC, PNG, GIF... (not RAW)
inline constexpr std::uint32_t kHideClips = 1u << 3;     // MP4, MOV... (audio is its own kind)
inline constexpr std::uint32_t kHideRaw = 1u << 4;       // a RAW, or a RAW+JPEG pair
inline constexpr std::uint32_t kHideUnmarked = 1u << 5;  // all but a kept set of paths
inline constexpr std::uint32_t kHideKnown =
    kHideAudio | kHideDocuments | kHideStills | kHideClips | kHideRaw | kHideUnmarked;
inline constexpr std::uint32_t kHideFilterBits = kHideStills | kHideClips | kHideRaw | kHideUnmarked;

// True when the stop `name` (with `secondary`, its pair's other half, empty
// when unpaired) is of a kind `hide` leaves out. A RAW+JPEG pair is RAW
// whichever half leads; a Live Photo is its still. Kind bits only: the
// unmarked bit is the caller's.
[[nodiscard]] inline bool is_hidden_stop(std::string_view name, std::string_view secondary,
                                         std::uint32_t hide) noexcept {
  if ((hide & ~kHideUnmarked) == 0) return false;
  if (is_audio_name(name)) return (hide & kHideAudio) != 0;
  if (is_document_name(name)) return (hide & kHideDocuments) != 0;
  if (is_raw_name(name) || (!secondary.empty() && is_raw_name(secondary))) {
    return (hide & kHideRaw) != 0;
  }
  if (is_video_name(name)) return (hide & kHideClips) != 0;
  return (hide & kHideStills) != 0;
}

// Whether a listing keeps one stop. `settings_hide` is Settings' audio /
// documents word: the stop that `stays` (the file the folder was opened on,
// the stop on screen) is never dropped by it. `filter_hide` is a kind
// filter's (kind_filter_mask): a relist keeps the staying stop too, so an
// explicit open still shows, but the relist a new filter choice makes
// (`filter_changed`) does not, so choosing Videos on a photo moves to a
// video. `marked` is whether the stop is in the kept set (kHideUnmarked).
// Both hosts call this; a filter that leaves nothing is the caller's (it
// keeps the staying stop, so the screen never goes blank).
[[nodiscard]] inline bool keeps_stop(std::string_view name, std::string_view secondary,
                                     bool marked, bool stays, std::uint32_t settings_hide,
                                     std::uint32_t filter_hide, bool filter_changed) noexcept {
  if (!stays && is_hidden_stop(name, secondary, settings_hide & (kHideAudio | kHideDocuments))) {
    return false;
  }
  if (filter_hide == 0) return true;
  if (stays && !filter_changed) return true;
  if ((filter_hide & kHideUnmarked) != 0 && !marked) return false;
  return !is_hidden_stop(name, secondary, filter_hide);
}

// View > Show. Order is the menu's and the wire's (chrome arg, bridge int).
enum class kind_filter : std::uint8_t { all = 0, photos = 1, videos = 2, raw = 3, marked = 4 };
inline constexpr int kKindFilterCount = 5;

[[nodiscard]] constexpr kind_filter kind_filter_from(int v) noexcept {
  return v >= 0 && v < kKindFilterCount ? static_cast<kind_filter>(v) : kind_filter::all;
}

// The hide bits a filter adds to Settings' own. Photos are every still,
// RAW included; Videos are clips; RAW is RAWs and RAW+JPEG pairs. A filter
// shows only its kind, so audio and documents go too.
[[nodiscard]] constexpr std::uint32_t kind_filter_mask(kind_filter f) noexcept {
  switch (f) {
    case kind_filter::all: return 0;
    case kind_filter::photos: return kHideClips | kHideAudio | kHideDocuments;
    case kind_filter::videos: return kHideStills | kHideRaw | kHideAudio | kHideDocuments;
    case kind_filter::raw: return kHideStills | kHideClips | kHideAudio | kHideDocuments;
    case kind_filter::marked: return kHideUnmarked;
  }
  return 0;
}

// "Photos", for the notice line and menus; the hosts' own menus spell the same.
[[nodiscard]] constexpr const char* kind_filter_name(kind_filter f) noexcept {
  switch (f) {
    case kind_filter::all: return "All";
    case kind_filter::photos: return "Photos";
    case kind_filter::videos: return "Videos";
    case kind_filter::raw: return "RAW";
    case kind_filter::marked: return "Marked";
  }
  return "All";
}

// The filter after `f` in menu order, wrapping: the keyed command cycles.
[[nodiscard]] constexpr kind_filter next_kind_filter(kind_filter f) noexcept {
  return static_cast<kind_filter>((static_cast<int>(f) + 1) % kKindFilterCount);
}

// Collapse a directory listing into navigation stops. Input is the extension-
// filtered scan (hidden/system already dropped), in the user's sort order.
// A pair sits where the first of its two files sat, so pairing never reorders
// the listing; everything unpaired keeps its own position.
inline std::vector<listed_item> pair_listing(std::vector<dir_entry> entries) {
  using namespace pairing_detail;
  const std::size_t n = entries.size();
  std::vector<std::string> keys;
  keys.reserve(n);
  for (const auto& e : entries) keys.push_back(key_of(e.name_utf8));

  // Group equal stems by sorting indices on (stem, original position).
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&keys](std::size_t a, std::size_t b) {
    const int c = keys[a].compare(keys[b]);
    return c != 0 ? c < 0 : a < b;
  });

  // One slot per original position; a pair fills its first member's slot.
  std::vector<listed_item> slots(n);
  std::vector<char> filled(n, 0);
  auto single = [&](std::size_t i) {
    slots[i].primary = std::move(entries[i]);
    filled[i] = 1;
  };

  for (std::size_t run = 0; run < n;) {
    std::size_t end = run + 1;
    while (end < n && keys[order[end]] == keys[order[run]]) ++end;

    if (end - run == 2) {
      const std::size_t a = order[run];      // earlier in the listing
      const std::size_t b = order[run + 1];
      const auto ext_a = extension(entries[a].name_utf8);
      const auto ext_b = extension(entries[b].name_utf8);
      pair_kind kind = pair_kind::none;
      bool a_primary = true;
      if (is_raw(ext_a) && is_still_pair(ext_b)) {
        kind = pair_kind::raw_jpeg;
        a_primary = false;
      } else if (is_raw(ext_b) && is_still_pair(ext_a)) {
        kind = pair_kind::raw_jpeg;
      } else if (is_still_pair(ext_a) && is_mov(ext_b)) {
        kind = pair_kind::live_photo;
      } else if (is_still_pair(ext_b) && is_mov(ext_a)) {
        kind = pair_kind::live_photo;
        a_primary = false;
      }
      if (kind != pair_kind::none) {
        listed_item& item = slots[a];
        item.primary = std::move(entries[a_primary ? a : b]);
        item.secondary = std::move(entries[a_primary ? b : a]);
        item.kind = kind;
        filled[a] = 1;
        run = end;
        continue;
      }
    }
    // One file, an unpairable two, or an ambiguous group: every file a stop.
    for (std::size_t k = run; k < end; ++k) single(order[k]);
    run = end;
  }

  std::vector<listed_item> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (filled[i]) out.push_back(std::move(slots[i]));
  }
  return out;
}

}  // namespace mv::io
