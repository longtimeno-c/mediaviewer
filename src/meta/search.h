// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #291 — every tag searchable (docs/design/06 "Search text", 16 "File search").
//
// One search string per file, built once in the core from the slice 1 read:
// camera (make and model), lens, made-with, software, title, caption,
// keywords and creator. Each is a *field*: the folded text keeps them apart
// (`kFieldSeparator`), so a match can say which one it was ("Canon EOS R5",
// "ChatGPT") instead of the whole string.
//
// Place names are not here (owner, 2026-10-11): they come from the Places
// add-on's search provider, which reads the coordinates kept beside this text
// (meta_cache.h, the host table's locations_in).
//
// Pure apart from `read_search_facts`, which is a bounded read (a prefix of
// the file, like read_date_taken) for the folder pass, never the full 8 MB read
// the Metadata pane makes.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "meta/meta.h"

namespace mv::meta {

// Bump when search_facts_of changes what it extracts: meta_cache.db rows of an
// older version are read again. 2 (#405): rating, colour label and exposure.
inline constexpr std::int32_t kSearchFactsVersion = 2;

// Between two folded fields. Never typed, never in a fold.
inline constexpr char kFieldSeparator = '\x1f';

struct search_facts {
  // Display values in why-order, no empty ones, no repeats (folded): camera,
  // lens, made-with, software, title, caption, each keyword, creator.
  std::vector<std::string> fields;
  std::string camera;     // summary::camera
  std::string lens;
  std::string made_with;
  bool ai_generated = false;
  std::int64_t date_taken_key = 0;  // 0 = none (summary::date_taken_key)
  std::optional<geo_point> where;
  // #405: what Local search's rating: / label: / iso: / f: / mm: / shutter:
  // terms ask. Never search fields: a word search for "400" must not return
  // every ISO 400 frame. 0 = absent for every number.
  int rating = 0;               // summary::rating: 1..5, -1 rejected, 0 unrated
  std::string label;            // summary::label, as written ("Red")
  std::int32_t iso = 0;         // ISO speed
  std::int32_t aperture_x100 = 0;  // f/2.8 -> 280
  std::int32_t focal_mm = 0;    // focal length, whole millimetres
  std::int32_t shutter_den = 0; // N when the exposure time is 1/N s, else 0
  std::int64_t shutter_us = 0;  // the exposure time in microseconds (2 s -> 2000000)
};

// #405: the summary's printed exposure values as numbers (pure; exposed for
// the tests). Each returns 0 for text it cannot read.
[[nodiscard]] std::int32_t iso_number(std::string_view printed);        // "ISO 400" -> 400
[[nodiscard]] std::int32_t aperture_x100(std::string_view printed);     // "f/2.8" -> 280
[[nodiscard]] std::int32_t focal_mm(std::string_view printed);          // "35 mm", "4.25 mm" -> 35, 4
// "1/250 s" -> 4000 us and den 250; "2 s" -> 2000000 us and den 0;
// "0.5 s" or "1/2" -> 500000 us and den 2.
[[nodiscard]] std::int64_t shutter_us(std::string_view printed, std::int32_t* den = nullptr);

// The facts of an already-read record (pure).
[[nodiscard]] search_facts search_facts_of(const metadata& m);

// The folder pass's read: the file's head only (kSearchPrefixBytes), then the
// XMP sidecar, a TIFF / RAW retry on 8 MB when the head held no tags, and a
// clip through libavformat. Worker thread only. status::io when the file
// cannot be read; a file with nothing to say is facts with no fields.
inline constexpr std::size_t kSearchPrefixBytes = 512u * 1024;
[[nodiscard]] result<search_facts> read_search_facts(std::string_view utf8_path);

// Every field folded (core/fold.h) and joined by kFieldSeparator.
[[nodiscard]] std::string folded_text(std::span<const std::string> fields);

// What was typed, folded and split on white space. Empty words dropped.
[[nodiscard]] std::vector<std::string> search_words(std::string_view typed);

// A file matches when every word is found in its folded text (in any field).
// Returns the index of the field the first word matched in, or -1 when the
// file does not match. Words never span a separator. An empty word list never
// matches.
[[nodiscard]] int match_fields(std::string_view folded, std::span<const std::string> words) noexcept;

// The "why" a result shows for field `i`: the display value, cut to
// `max_chars` code points with an ellipsis.
[[nodiscard]] std::string why_of(std::span<const std::string> fields, int i, std::size_t max_chars = 48);

}  // namespace mv::meta
