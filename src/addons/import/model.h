// SPDX-License-Identifier: GPL-2.0-or-later
// The Import add-on's data model (plan/18-import.md "The engine").
//
// A *unit* is what is copied, verified, skipped and sorted together: one file,
// a RAW+JPEG pair, or a Live Photo, with its camera sidecars (.xmp .THM .LRV
// .XML). Units are never split across folders. Portable; no platform header.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::import {

using digest = std::array<std::uint8_t, 32>;  // BLAKE3-256

enum class file_type : std::uint8_t {
  raw = 0,
  jpeg = 1,
  heic = 2,
  video = 3,
  other = 4,    // the rest of the D5 still set: PNG, TIFF, WebP, GIF, BMP, AVIF, ICO
  sidecar = 5,  // .xmp .thm .lrv .xml: travel with their file, never alone
  none = 6,     // not media: camera system files, never imported
};

// Bits for preset::types (plan/18 "type filter").
inline constexpr std::uint32_t kTypeRaw = 1u << 0;
inline constexpr std::uint32_t kTypeJpeg = 1u << 1;
inline constexpr std::uint32_t kTypeHeic = 1u << 2;
inline constexpr std::uint32_t kTypeVideo = 1u << 3;
inline constexpr std::uint32_t kTypeOther = 1u << 4;
inline constexpr std::uint32_t kTypeAll = kTypeRaw | kTypeJpeg | kTypeHeic | kTypeVideo | kTypeOther;

[[nodiscard]] constexpr std::uint32_t type_bit(file_type t) noexcept {
  switch (t) {
    case file_type::raw: return kTypeRaw;
    case file_type::jpeg: return kTypeJpeg;
    case file_type::heic: return kTypeHeic;
    case file_type::video: return kTypeVideo;
    case file_type::other: return kTypeOther;
    default: return 0;
  }
}

// By extension. Import decides what to copy, never what to decode; the viewer
// still probes magic bytes when it opens a file.
[[nodiscard]] file_type classify(std::string_view name) noexcept;

struct source_file {
  std::string path;      // absolute, native
  std::string rel;       // from the source root, '/'-separated
  std::string vol_rel;   // from the volume root: the card memory's key
  std::string name;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  file_type type = file_type::none;
  bool have_hash = false;
  digest hash{};
};

enum class unit_kind : std::uint8_t { single = 0, raw_jpeg = 1, live_photo = 2 };

struct unit {
  std::vector<std::uint32_t> files;  // indices into scan::files; [0] is the primary
  unit_kind kind = unit_kind::single;
  std::int64_t taken = 0;    // local wall-clock seconds, "as if UTC"
  bool has_date = false;     // false: `taken` is the file time (labelled)
  std::int64_t file_time = 0;  // earliest member mtime, local wall clock
  std::string camera;
  std::uint64_t bytes = 0;
  bool imported_before = false;  // the card memory has every member as imported
  bool has_raw = false;
};

struct scan_result {
  std::uint64_t id = 0;
  std::string root;
  std::string volume_id;
  std::string label;
  std::string device_key;
  bool network = false;
  bool removable = false;
  bool explicit_files = false;  // from the viewer's marks, not a walk
  std::vector<source_file> files;
  std::vector<unit> units;
};

// ---------------------------------------------------------------------------
// Presets (plan/18 "Configurability").

enum class selection_mode : std::uint8_t { new_only = 0, all = 1, marked = 2, date_range = 3 };
enum class date_source : std::uint8_t { taken = 0, file_time = 1 };
enum class dup_scope : std::uint8_t { destination = 0, library = 1 };
enum class layout_kind : std::uint8_t {
  year_day = 0,   // YYYY/YYYY-MM-DD (default)
  year_month_day, // YYYY/MM/DD
  day,            // YYYY-MM-DD
  card,           // keep card structure
  flat,
};

struct preset {
  std::string name = "Default";
  selection_mode selection = selection_mode::new_only;
  std::string range_from;  // "YYYY-MM-DD", inclusive; date_range only
  std::string range_to;
  std::uint32_t types = kTypeAll;
  std::string destination;  // empty: the default library folder
  std::string backup;       // empty: off
  layout_kind layout = layout_kind::year_day;
  bool layout_camera = false;  // + camera model
  bool layout_type = false;    // + RAW / JPEG / Video subfolders
  date_source dates = date_source::taken;
  std::string rename;          // template; empty: names unchanged
  bool skip_duplicates = true; // false: import anyway, under a safe name
  dup_scope scope = dup_scope::destination;
  bool full_verify = true;     // false: hash-on-read only (network targets)
  bool companions = true;
  bool eject_after = true;
  bool open_after = false;
  bool notify = true;
  bool fast = false;           // priority: false = background (yields to the viewer)
};

// Strict parse; unknown keys are ignored, a mistyped known key fails.
[[nodiscard]] bool parse_preset(std::string_view json, preset& out);
[[nodiscard]] std::string preset_to_json(const preset& p);

// ---------------------------------------------------------------------------
// Plans.

enum class unit_state : std::uint8_t {
  fresh = 0,       // "new"
  duplicate = 1,   // same content already in scope: skipped, says what it matched
  imported = 2,    // imported from this card before (card memory)
  filtered = 3,    // outside the preset's type filter or date range
};

struct plan_unit {
  std::uint32_t unit = 0;  // index into scan_result::units
  unit_state state = unit_state::fresh;
  bool selected = false;
  std::string day;         // "YYYY-MM-DD" (the grid's grouping)
  std::string folder;      // relative destination folder, '/'-separated
  std::vector<std::string> names;  // destination file name per member
  std::string matched;     // for a duplicate: what it matched (relative path)
  bool renamed_for_clash = false;
  std::uint32_t seq = 0;   // the {seq} it was given, 0 if none
};

struct plan_result {
  std::uint64_t id = 0;
  std::uint64_t scan_id = 0;
  preset settings;
  std::string destination;  // resolved
  std::string backup;       // resolved; empty = off
  std::vector<plan_unit> units;
};

}  // namespace mv::import
