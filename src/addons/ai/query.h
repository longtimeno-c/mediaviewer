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
//                          the date taken (#404: the camera's wall clock read
//                          as UTC, or a Photos item's creationDate), else the
//                          file's modification time (UTC) while there is none
//   said:hello             same as "hello", in a clip only (its speech, or its
//                          own text: a chapter title, a subtitle line)
//   text:lease  says:"invoice 4471"
//                          in a document's text only (#354)
//   is:document  is:pdf  is:docx  invoice documents  receipts pdfs
//                          documents (PDF, DOCX); is:pdf / is:docx the format
//   file:IMG_12  file:"my trip"  -file:copy
//                          the file's name contains the text (case and accents
//                          folded); the base app's file search, in the query
//   camera:R5  lens:"24-70"  -camera:iphone
//                          the file's camera / lens tag contains the text (#291)
//   ai:yes  ai:no  made:chatgpt  ai:firefly
//                          AI provenance (summary::ai_generated), or the
//                          generator the file names ("made with")
//   rating:5  rating:3+  rating:0  rejected  -rating:1  label:red
//   iso:400  iso:100-800  iso:1600+  f:2.8  f:1.4-2.8  mm:35  mm:24-70
//   shutter:1/250  shutter:250  shutter:2s
//                          the rating, colour label and exposure the file's
//                          tags state (#405); a file with no value for a term
//                          does not match it (a clip has no ISO)
//   place:portugal  in:lisbon
//                          where it was taken: answered by the Places add-on's
//                          search provider; `in:` is a date when it reads as
//                          one (in:2024), a place otherwise, never a folder
//   R5  chatgpt            a plain word that matches a file's tags (camera,
//                          lens, made-with, software, title, caption, keywords,
//                          creator) puts those files first
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

// Where a quoted phrase must be found (#354).
enum class phrase_in : std::uint8_t {
  anywhere = 0,   // "quoted": speech, a clip's own text, a picture's text, a document
  said = 1,       // said: a clip's speech or its own text (as before #354)
  documents = 2,  // text: / says: a document's text only
};

// #405: a numeric term as an inclusive range in the index's own units
// (rating, ISO, aperture hundredths, focal mm, shutter microseconds).
struct range {
  std::int64_t lo = std::numeric_limits<std::int64_t>::min();
  std::int64_t hi = std::numeric_limits<std::int64_t>::max();
  bool negated = false;  // -rating:1: the file must not be in it
  [[nodiscard]] bool holds(std::int64_t v) const noexcept { return v >= lo && v <= hi; }
};
// What a file states for the #405 terms; `rated` false: not read yet.
struct exposure_facts {
  bool rated = false;
  int rating = 0;
  std::string label;  // folded
  std::int64_t iso = 0, aperture_x100 = 0, focal_mm = 0, shutter_us = 0;  // 0 absent
};

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
    phrase_in in = phrase_in::anywhere;
  };
  std::vector<phrase> phrases;
  std::uint32_t kinds = 0;    // MV_AI_KIND_* bits the query asked for; 0 = not said
  std::uint32_t not_kinds = 0;
  std::vector<std::string> extensions;  // #354: is:pdf / is:docx: the file's extension is one of these
  std::int64_t from_unix = std::numeric_limits<std::int64_t>::min();  // inclusive
  std::int64_t to_unix = std::numeric_limits<std::int64_t>::max();    // exclusive
  std::vector<std::string> files;      // file:x, folded: the name must contain each
  std::vector<std::string> not_files;  // -file:x, folded: the name must contain none
  // #291: tags. Folded; each must be contained in the tag (none, for not_).
  std::vector<std::string> cameras, not_cameras;
  std::vector<std::string> lenses, not_lenses;
  std::vector<std::string> made, not_made;  // made:x / ai:x for x not yes/no
  int ai = 0;                               // ai:yes 1, ai:no -1, not said 0
  std::vector<std::string> places, not_places;  // place:x / in:x (not a date), as typed
  // #405: rating: / rejected, label:, iso:, f:, mm:, shutter:.
  std::vector<range> ratings, isos, apertures, focals, shutters;
  std::vector<std::string> labels, not_labels;  // folded
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
  std::vector<phrase_in> phrase_where;            // where each of `phrases` must be (#354)
  std::vector<phrase_in> not_phrase_where;
  bool last_phrase_open = false;                  // phrases.back()'s last word may be partial
  std::vector<std::string> extensions;            // #354: is:pdf / is:docx
  std::uint32_t kinds = 0;                        // MV_AI_KIND_* to keep; 0 = any
  std::int64_t from_unix = std::numeric_limits<std::int64_t>::min();
  std::int64_t to_unix = std::numeric_limits<std::int64_t>::max();
  std::vector<std::string> files;      // folded: the file's name contains each
  std::vector<std::string> not_files;  // folded: and none of these
  // #291: tag terms, as parsed (folded).
  std::vector<std::string> cameras, not_cameras, lenses, not_lenses, made, not_made;
  int ai = 0;
  // place: / in: (not a date). The engine keeps only geotagged assets; which
  // place matches is the Places add-on's provider's answer (docs/design/17).
  std::vector<std::string> places, not_places;
  // #405: the rating, label and exposure terms, as parsed.
  std::vector<range> ratings, isos, apertures, focals, shutters;
  std::vector<std::string> labels, not_labels;
  // The plain words, folded, for the tag-text match (search.h's rule: every
  // word in the file's search fields). Empty when there are none.
  std::vector<std::string> tag_words;
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
  [[nodiscard]] bool narrows() const noexcept {
    return !people.empty() || !phrases.empty() || !files.empty() || has_tags() || !places.empty();
  }
  // A camera / lens / made-with / AI term, kept or left out.
  [[nodiscard]] bool has_tags() const noexcept {
    return !cameras.empty() || !not_cameras.empty() || !lenses.empty() || !not_lenses.empty() || !made.empty() ||
           !not_made.empty() || ai != 0 || has_exposure();
  }
  // #405: a rating, label or exposure term.
  [[nodiscard]] bool has_exposure() const noexcept {
    return !ratings.empty() || !isos.empty() || !apertures.empty() || !focals.empty() || !shutters.empty() ||
           !labels.empty() || !not_labels.empty();
  }
  // Whether a file's facts pass every #405 term: a kept term needs a value in
  // range (no value, no match); a left-out one drops only a value in range.
  [[nodiscard]] bool exposure_matches(const exposure_facts& f) const;
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

// The shared fold (core/fold.h, #291): lower case, accents dropped, the
// hosts' name rule ("José" -> "jose"; ß, æ, ø stay themselves).
[[nodiscard]] std::string fold(std::string_view utf8);
// `term` (folded) is contained in `value` (folded here).
[[nodiscard]] bool tag_contains(std::string_view value, std::string_view folded_term);
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

// #405: a term's text as a range in the index's units; false when it does
// not read as one yet (a term still being typed is dropped). `op` is the
// operator: "rating", "iso", "f", "mm" or "shutter".
[[nodiscard]] bool numeric_term(std::string_view op, std::string_view text, range& out);

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
[[nodiscard]] std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept;

}  // namespace mv::ai::query
