// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode (docs/plans/phone-in-the-app.md §4): a paired library with no catalogue yet is the
// NAS's named folders, listed through the service, and searched as the desktop's File search
// searches (docs/design/16 "File search", 06 "Search text"): the folder on screen only, by name
// (case- and accent-insensitive substring) and then by tags, every typed word somewhere in the
// file's fields, with the field the first word matched in as the "why". The fold, the fields and
// the match are the desktop's own code (core/fold.h, meta/search.cpp); only the tag read differs:
// the phone parses the file's head with ImageIO and hands the values over named as Exiv2 names them.
// Pure apart from the listing helpers, which take a listing already read.
#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wire/serve_wire.h"

namespace mv::mobile::files {

enum class kind : std::int32_t { none = -1, photo = 0, raw = 1, clip = 2 };

// What the phone shows a file as, from its extension (the D5 table, io/media_extensions.h): a
// photo, a RAW, a clip, or nothing (audio, documents and everything else are not shown).
[[nodiscard]] kind kind_of(std::string_view name) noexcept;

struct entry {
  std::string name;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  bool dir = false;
  kind k = kind::none;
};

// What a folder shows: its folders and the files the phone can show, hidden (dot) names left out,
// in the listing's order.
[[nodiscard]] std::vector<entry> shown(std::span<const driftwire::serve::list_entry> listing);

// One value ImageIO read, under the Exiv2 key the desktop's search_facts_of reads it by
// ("Exif.Image.Software", "Iptc.Application2.Keywords", "Xmp.dc.subject" as "a, b").
struct tag {
  std::string raw_tag;
  std::string value;
};

// The desktop's camera line (meta/still.cpp): the model when it already starts with the make.
[[nodiscard]] std::string camera_line(std::string_view make, std::string_view model);

// The file's search fields in why-order (camera, lens, software, title, caption, keywords,
// creator), through meta::search_facts_of. `comment` is the EXIF user comment.
[[nodiscard]] std::vector<std::string> fields_of(std::string_view make, std::string_view model, std::string_view lens,
                                                 std::string_view comment, std::span<const tag> tags);

// The folder on screen, for the search: its names, and each file's fields once they are read.
// Thread-safe; a keystroke is one pass with no allocation per file beyond the results.
class index {
 public:
  // A new listing: every file's fields are forgotten.
  void set_names(std::vector<std::string> names);
  // `i` out of range is ignored (a listing replaced meanwhile).
  void set_fields(std::size_t i, std::span<const std::string> fields);

  struct hit {
    std::size_t index = 0;
    std::string why;  // empty for a name match
  };
  // Names first, in listing order, then the files whose tags hold every typed word (two folded
  // letters at least, as on the desktop). A file matched by name is not repeated.
  [[nodiscard]] std::vector<hit> match(std::string_view typed) const;
  [[nodiscard]] std::size_t count() const;
  // How many files' fields have been set.
  [[nodiscard]] std::size_t tagged() const;

 private:
  mutable std::mutex m_;
  std::vector<std::string> folded_names_;
  std::vector<std::vector<std::string>> fields_;
  std::vector<std::string> folded_fields_;
  std::vector<bool> has_fields_;
  std::size_t tagged_ = 0;
};

// Where a catalogue folder's files may be among the service's folders, most likely first, when no
// folder is labelled with its share's name: the catalogue was made where the library sat then (a
// NAS share, a disk), and the folder served now may be the share under another label, a folder
// inside it, or the disk above it. Each is a service folder and the path inside it that the
// catalogue's relative paths go under ("" for its top); the caller keeps the first that holds a file.
struct place {
  std::string root;    // the service's folder id
  std::string prefix;  // '/'-separated, may be empty
  bool operator==(const place&) const = default;
};
struct served_root {
  std::string id;
  std::string label;
};
[[nodiscard]] std::vector<place> places_for(std::string_view share, std::string_view sub, std::span<const served_root> roots);

}  // namespace mv::mobile::files
