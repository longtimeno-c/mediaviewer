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
#include <iterator>
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
// Issue #207, the cull filter (View > Show's second group): bits that need
// each stop's rating and colour label (cull_marks), which a worker reads
// (meta::read_cull_marks) and the caller caches by path + size + mtime.
inline constexpr std::uint32_t kHideRejected = 1u << 6;  // rating -1 ("Rejected")
inline constexpr std::uint32_t kHideUnpicked = 1u << 7;  // rated below one star (or rejected)
// Bits 8-10: a colour label (cull_label 1..5); every stop without it is hidden.
inline constexpr unsigned kHideLabelShift = 8;
inline constexpr std::uint32_t kHideOtherLabels = 7u << kHideLabelShift;
inline constexpr std::uint32_t kHideCullBits = kHideRejected | kHideUnpicked | kHideOtherLabels;
inline constexpr std::uint32_t kHideKnown = kHideAudio | kHideDocuments | kHideStills | kHideClips |
                                            kHideRaw | kHideUnmarked | kHideCullBits;
inline constexpr std::uint32_t kHideFilterBits =
    kHideStills | kHideClips | kHideRaw | kHideUnmarked | kHideCullBits;

// The colour labels Lightroom and Bridge write in xmp:Label, in their key
// order (6 Red .. 9 Blue, Purple with no key). `other` is a word this app does
// not know (another app's label set): it is a label, just none of these.
enum class cull_label : std::uint8_t { none = 0, red, yellow, green, blue, purple, other };

// What a cull filter needs to know of one stop. The default is an unrated,
// unlabelled file, which is also what a stop not read yet counts as.
struct cull_marks {
  std::int8_t rating = 0;  // -1 rejected, 0 unrated, 1..5 stars
  cull_label label = cull_label::none;
  friend constexpr bool operator==(const cull_marks&, const cull_marks&) = default;
};

// "Red", "red", " RED " -> red; "" -> none; any other word -> other.
[[nodiscard]] constexpr cull_label cull_label_of(std::string_view word) noexcept {
  while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) word.remove_prefix(1);
  while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) word.remove_suffix(1);
  if (word.empty()) return cull_label::none;
  constexpr std::string_view names[] = {"red", "yellow", "green", "blue", "purple"};
  for (std::size_t i = 0; i < std::size(names); ++i) {
    const std::string_view n = names[i];
    if (n.size() != word.size()) continue;
    bool same = true;
    for (std::size_t c = 0; c < n.size() && same; ++c) {
      char w = word[c];
      if (w >= 'A' && w <= 'Z') w = static_cast<char>(w - 'A' + 'a');
      same = w == n[c];
    }
    if (same) return static_cast<cull_label>(i + 1);
  }
  return cull_label::other;
}

// True when `hide` has a bit that needs the stop's cull_marks.
[[nodiscard]] constexpr bool needs_cull_marks(std::uint32_t hide) noexcept {
  return (hide & kHideCullBits) != 0;
}

// True when the cull bits of `hide` leave out a stop with `m`.
[[nodiscard]] constexpr bool is_culled_out(const cull_marks& m, std::uint32_t hide) noexcept {
  if ((hide & kHideRejected) != 0 && m.rating < 0) return true;
  if ((hide & kHideUnpicked) != 0 && m.rating < 1) return true;
  const std::uint32_t want = (hide & kHideOtherLabels) >> kHideLabelShift;
  if (want != 0 && static_cast<std::uint32_t>(m.label) != want) return true;
  return false;
}

// True when the stop `name` (with `secondary`, its pair's other half, empty
// when unpaired) is of a kind `hide` leaves out. A RAW+JPEG pair is RAW
// whichever half leads; a Live Photo is its still. Kind bits only: the
// unmarked and cull bits are the caller's.
[[nodiscard]] inline bool is_hidden_stop(std::string_view name, std::string_view secondary,
                                         std::uint32_t hide) noexcept {
  if ((hide & ~(kHideUnmarked | kHideCullBits)) == 0) return false;
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
// `marks` are its rating and label (issue #207), only read when the filter
// has cull bits; the default is an unrated, unlabelled stop.
// Both hosts call this; a filter that leaves nothing is the caller's (it
// keeps the staying stop, so the screen never goes blank).
[[nodiscard]] inline bool keeps_stop(std::string_view name, std::string_view secondary,
                                     bool marked, bool stays, std::uint32_t settings_hide,
                                     std::uint32_t filter_hide, bool filter_changed,
                                     const cull_marks& marks = {}) noexcept {
  if (!stays && is_hidden_stop(name, secondary, settings_hide & (kHideAudio | kHideDocuments))) {
    return false;
  }
  if (filter_hide == 0) return true;
  if (stays && !filter_changed) return true;
  if ((filter_hide & kHideUnmarked) != 0 && !marked) return false;
  if (is_culled_out(marks, filter_hide)) return false;
  return !is_hidden_stop(name, secondary, filter_hide);
}

// View > Show. Order is the menu's and the wire's (chrome arg, bridge int,
// the 4-bit field of the Windows chrome's sort word). 0-4 are kinds (issue
// #206); 5-11 are the cull filters (issue #207), on rating and label.
enum class kind_filter : std::uint8_t {
  all = 0, photos = 1, videos = 2, raw = 3, marked = 4,
  picks = 5, not_rejected = 6, red = 7, yellow = 8, green = 9, blue = 10, purple = 11,
};
inline constexpr int kKindFilterCount = 12;
inline constexpr int kFirstCullFilter = 5;  // the menus put a separator before it

[[nodiscard]] constexpr kind_filter kind_filter_from(int v) noexcept {
  return v >= 0 && v < kKindFilterCount ? static_cast<kind_filter>(v) : kind_filter::all;
}

[[nodiscard]] constexpr std::uint32_t label_filter_bits(cull_label l) noexcept {
  return (static_cast<std::uint32_t>(l) << kHideLabelShift) & kHideOtherLabels;
}

// The hide bits a filter adds to Settings' own. Photos are every still,
// RAW included; Videos are clips; RAW is RAWs and RAW+JPEG pairs. A filter
// shows only its kind, so audio and documents go too. Picks are stops rated
// one star or more (XMP has no pick flag; Lightroom keeps its in the catalog);
// Not rejected hides rating -1; a colour shows that xmp:Label only. The cull
// filters leave audio and documents to Settings: a rated clip is a rated file.
[[nodiscard]] constexpr std::uint32_t kind_filter_mask(kind_filter f) noexcept {
  switch (f) {
    case kind_filter::all: return 0;
    case kind_filter::photos: return kHideClips | kHideAudio | kHideDocuments;
    case kind_filter::videos: return kHideStills | kHideRaw | kHideAudio | kHideDocuments;
    case kind_filter::raw: return kHideStills | kHideClips | kHideAudio | kHideDocuments;
    case kind_filter::marked: return kHideUnmarked;
    case kind_filter::picks: return kHideUnpicked;
    case kind_filter::not_rejected: return kHideRejected;
    case kind_filter::red: return label_filter_bits(cull_label::red);
    case kind_filter::yellow: return label_filter_bits(cull_label::yellow);
    case kind_filter::green: return label_filter_bits(cull_label::green);
    case kind_filter::blue: return label_filter_bits(cull_label::blue);
    case kind_filter::purple: return label_filter_bits(cull_label::purple);
  }
  return 0;
}

// "Photos", for the menus; the hosts' own menus spell the same.
[[nodiscard]] constexpr const char* kind_filter_name(kind_filter f) noexcept {
  switch (f) {
    case kind_filter::all: return "All";
    case kind_filter::photos: return "Photos";
    case kind_filter::videos: return "Videos";
    case kind_filter::raw: return "RAW";
    case kind_filter::marked: return "Marked";
    case kind_filter::picks: return "Picks";
    case kind_filter::not_rejected: return "Not rejected";
    case kind_filter::red: return "Red";
    case kind_filter::yellow: return "Yellow";
    case kind_filter::green: return "Green";
    case kind_filter::blue: return "Blue";
    case kind_filter::purple: return "Purple";
  }
  return "All";
}

// What the notice line says when `f` is chosen (both hosts).
[[nodiscard]] constexpr const char* kind_filter_notice(kind_filter f) noexcept {
  switch (f) {
    case kind_filter::all: return "Showing all";
    case kind_filter::photos: return "Showing Photos only";
    case kind_filter::videos: return "Showing Videos only";
    case kind_filter::raw: return "Showing RAW only";
    case kind_filter::marked: return "Showing Marked only";
    case kind_filter::picks: return "Showing picks (1 star or more) only";
    case kind_filter::not_rejected: return "Hiding rejected";
    case kind_filter::red: return "Showing Red label only";
    case kind_filter::yellow: return "Showing Yellow label only";
    case kind_filter::green: return "Showing Green label only";
    case kind_filter::blue: return "Showing Blue label only";
    case kind_filter::purple: return "Showing Purple label only";
  }
  return "Showing all";
}

// The filter after `f` in menu order, wrapping: the keyed command cycles.
[[nodiscard]] constexpr kind_filter next_kind_filter(kind_filter f) noexcept {
  return static_cast<kind_filter>((static_cast<int>(f) + 1) % kKindFilterCount);
}
// The same, passing over Marked when nothing is marked (it would be refused,
// and the cycle would never reach the cull filters after it).
[[nodiscard]] constexpr kind_filter next_kind_filter(kind_filter f, bool have_marks) noexcept {
  const kind_filter n = next_kind_filter(f);
  return n == kind_filter::marked && !have_marks ? next_kind_filter(n) : n;
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
