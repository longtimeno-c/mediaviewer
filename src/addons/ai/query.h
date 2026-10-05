// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Local search query language (docs/design/17 "Query syntax", 2026-09-28): one
// parser in the shared core, so the WinUI and SwiftUI search fields, and the
// Voice add-on after them (docs/design/19), mean the same thing by the same words.
//
//   Tristan beach          a person AND what the picture shows
//   Tristan "hello"        a person AND words said in the clip (quoted = exact)
//   Tristan or Aaryan      either of them (people side by side: all of them)
//   @Tri  person:"Anna S"  a person by name prefix (typo tolerant)
//   -Nico  -beach  -"hi"   leave out a person, a subject, a phrase
//   beach video  is:photo  a kind (the last word, "videos of ...", or is: /
//                          type:); "photos of ..." still asks for anything
//   in:2024  before:2025-06  after:2023  since:2024-03-01
//                          the file's date (modification time, UTC)
//   said:hello             same as "hello"
//   file:IMG_12  file:"my trip"  -file:copy
//                          the file's name contains the text (case and accents
//                          folded); the base app's file search, in the query
//
// A lone word that starts a name ("Trist", or "Tristna" with a typo) shows
// that person's photos first and what the word describes after them: results
// appear while a name is typed, and "car" still finds cars when a Carla exists.
//
// Pure C++: no ORT, no SQLite, no OS. Parsing is per keystroke and linear in
// the query; name resolution is linear in the named people.
#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mv::ai::query {

// What the field said, before names are known.
struct parsed {
  struct word {
    std::string text;         // as typed (a quoted person:"..." keeps its spaces)
    bool negated = false;     // -word
    bool person = false;      // @x or person:x: a name, never a subject
    bool or_before = false;   // "or" (or "|") stood between this and the previous word
  };
  std::vector<word> words;    // bare words in order, operators and quotes removed
  struct phrase {
    std::string text;
    bool negated = false;
    bool open = false;        // the closing quote is not typed yet: the last word may be partial
  };
  std::vector<phrase> phrases;
  std::uint32_t kinds = 0;    // MV_AI_KIND_* bits the query asked for; 0 = not said
  std::uint32_t not_kinds = 0;
  std::int64_t from_unix = std::numeric_limits<std::int64_t>::min();  // inclusive
  std::int64_t to_unix = std::numeric_limits<std::int64_t>::max();    // exclusive
  std::vector<std::string> files;      // file:x, folded: the name must contain each
  std::vector<std::string> not_files;  // -file:x, folded: the name must contain none
  [[nodiscard]] bool has_dates() const noexcept {
    return from_unix != std::numeric_limits<std::int64_t>::min() ||
           to_unix != std::numeric_limits<std::int64_t>::max();
  }
};

[[nodiscard]] parsed parse(std::string_view utf8);

struct person_name {
  std::int64_t id = 0;
  std::string name;
};

// What a search runs: parse() with the named people applied.
struct plan {
  std::string text;                               // for the picture, sound and speech towers; "" none
  std::vector<std::string> not_text;              // -beach: assets the picture search finds are left out
  std::vector<std::vector<std::int64_t>> people;  // every group must appear; a group is any of its ids
  std::vector<std::int64_t> not_people;
  std::vector<std::string> phrases;               // each must be said (speech transcript)
  std::vector<std::string> not_phrases;
  bool last_phrase_open = false;                  // phrases.back()'s last word may be partial
  std::uint32_t kinds = 0;                        // MV_AI_KIND_* to keep; 0 = any
  std::int64_t from_unix = std::numeric_limits<std::int64_t>::min();
  std::int64_t to_unix = std::numeric_limits<std::int64_t>::max();
  std::vector<std::string> files;      // folded: the file's name contains each
  std::vector<std::string> not_files;  // folded: and none of these
  // A lone word that only starts (or nearly spells) a name: those people
  // lead, and `text` answers after them (a union, not a filter).
  std::vector<std::int64_t> people_first;
  // An explicit @name that matched nobody: nothing can match.
  bool impossible = false;

  [[nodiscard]] bool has_dates() const noexcept {
    return from_unix != std::numeric_limits<std::int64_t>::min() ||
           to_unix != std::numeric_limits<std::int64_t>::max();
  }
  // Anything that narrows the assets before ranking (not scope or kind).
  [[nodiscard]] bool narrows() const noexcept { return !people.empty() || !phrases.empty() || !files.empty(); }
  // A file name term, kept or left out.
  [[nodiscard]] bool has_files() const noexcept { return !files.empty() || !not_files.empty(); }
};

[[nodiscard]] plan resolve(const parsed& p, std::span<const person_name> people);

// Names for the word being typed (the last bare word, or an @ / person: term):
// prefix matches first, then near spellings; a word already spelling a whole
// name suggests nothing. `completion` is the query with that word replaced.
struct suggestion {
  std::int64_t id = 0;
  std::string name;
  std::string completion;
};
[[nodiscard]] std::vector<suggestion> suggest(std::string_view utf8, std::span<const person_name> people,
                                              std::size_t max = 5);

// ---- helpers (exposed for the engine and the tests) ---------------------------------

// Lower-case, with common Latin accents folded ("José" -> "jose").
[[nodiscard]] std::string fold(std::string_view utf8);
// file: / -file: against a path's last component (either separator).
[[nodiscard]] bool name_matches(std::string_view path, std::span<const std::string> files,
                                std::span<const std::string> not_files);
// Words as the speech index stores them (infer::speech_words' rule: letters,
// digits and bytes >= 0x80, lower-cased, an apostrophe never splits).
[[nodiscard]] std::vector<std::string> words_of(std::string_view utf8);
// `phrase` as consecutive words of `words`; `last_prefix`: its last word may
// be the start of a longer one (a quote still being typed).
[[nodiscard]] bool contains_phrase(std::span<const std::string> words, std::span<const std::string> phrase,
                                   bool last_prefix);
// Damerau-Levenshtein (adjacent transpositions), stopping above `limit`.
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b, std::size_t limit);

// The other grammatical number of an English noun ("mountain" <-> "mountains",
// "beach" <-> "beaches", "city" <-> "cities", "child" <-> "children").
// Empty when the word has none worth trying (short, a number, a stop word).
[[nodiscard]] std::string other_number(std::string_view word);
// A picture query with its last noun in both numbers: {text} alone, or
// {singular form, plural form} in that order whichever was typed, so
// "mountain" and "mountains" embed as the same mean vector.
[[nodiscard]] std::vector<std::string> number_forms(std::string_view text);

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
[[nodiscard]] std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept;

}  // namespace mv::ai::query
