// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a person types or pastes at the driftwire prompt, as the files to
// send. Terminals paste a dropped file as its path (quoted, escaped, or as a
// file:// URI), so a line is split the way a shell splits it, and expanded
// the way a shell would, without running one:
//
//   'single quotes'    everything inside as it is
//   "double quotes"    as it is but \" \\ \$ \` (POSIX)
//   back\ slash        the next character as it is (POSIX only: on Windows a
//                      backslash is the separator, and "..." quotes)
//   ~  ~/photos        the home folder (unquoted, at the start of a word)
//   file:///a%20b.jpg  the path in the URI, percent-decoded
//   *.jpg  IMG_?.HEIC  [ab]*.png
//                      globs (unquoted), matched here; * and ? never match
//                      a leading dot; no match: the word as it is, if that
//                      exists, else missing
//
// Relative paths are taken from the current folder, every path is made
// absolute (lexically: links are kept as typed), and repeats are dropped, in
// the order typed. Only paths that exist come back as paths; the rest are
// named as missing, as typed (the prompt says so in red and adds the others).
//
// One exception to the shell: a line with spaces and no quotes, escapes or
// globs whose words do not all exist, but which as a whole names a file, is
// that one file (a path pasted from somewhere that does not quote it).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

struct path_options {
  std::string cwd;   // absolute, UTF-8
  std::string home;  // "" : no ~ expansion
  bool windows = false;  // the Windows rules: backslash separates, quotes are "..."
};

// The process's own: its current folder, the home folder (home_dir(): HOME,
// or the profile folder on Windows), the platform's rules.
[[nodiscard]] path_options default_path_options();

// One word of a line: its text with the quotes and escapes resolved, which
// of its characters are live glob characters (unquoted * ? [), and whether
// it began with an unquoted ~.
struct word {
  std::string text;
  std::vector<bool> glob;  // beside text, byte for byte
  bool tilde = false;
  std::size_t begin = 0, end = 0;  // where it stands in the line (bytes)
  char open_quote = 0;             // '\'' or '"' when the line ended inside one
};

// The line split into words by the shell's rules (see above).
[[nodiscard]] std::vector<word> split_words(std::string_view line, bool windows);

struct parsed_paths {
  std::vector<std::string> paths;    // absolute, existing, in order, once each
  std::vector<std::string> missing;  // as typed (a file:// URI decoded)
  bool unterminated = false;         // a quote was left open (closed at the end)
};

[[nodiscard]] parsed_paths parse_paths(std::string_view line, const path_options& o);

// The path inside a file:// URI, percent-decoded; "" when `s` is not one, or
// names another host.
[[nodiscard]] std::string file_uri_path(std::string_view s, bool windows);

// Shell-style matching of `name` against `pattern`, by character: * any run,
// ? one, [abc] [a-z] [!a] one of (or not). `live` marks which pattern bytes
// are glob characters (a quoted * is a star).
[[nodiscard]] bool glob_match(std::string_view name, std::string_view pattern, const std::vector<bool>& live);

}  // namespace mv::driftwire::cli
