// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a drop of files onto a folder becomes (docs/design/16 "Drag and drop
// into folders"): copy, move, or nothing and why. One rule for both chromes and
// for the keyboard twins (Paste files here, Move to…), so a drop on a tile, a
// sidebar place, a breadcrumb or the grid itself all answer alike. Pure, no
// I/O: the chrome asks it on every drag-over to colour the target.
//
// Finder's rules, kept: a drag from inside the app moves, Option (Windows:
// Ctrl) copies; a drag from another app or Finder copies ("adds them"), Cmd
// (Windows: Shift) moves. A folder never goes into itself. Files already in
// the folder are not moved onto themselves. Where the folder cannot be
// written (a Photos library, a read-only list, another computer's folder) the
// drop is refused, with words the chrome shows on the target.
//
// `classify` says what a path is (place_kind); the host backs it with
// write_guard. A remote folder is its own kind so the phase that lets a
// `change` device write there swaps the answer, not the rule.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "io/collision_name.h"
#include "shell/browse_path.h"

namespace mv::shell {

enum class drop_verb : std::uint8_t { none = 0, copy = 1, move = 2 };

// Wire values: both chromes read them (mv_chrome_drop_verdict, the C# DropWords).
enum class drop_refusal : std::uint8_t {
  none = 0,
  nothing = 1,           // no files, or no folder under the pointer
  read_only_target = 2,  // a Photos library, a read-only list
  remote_target = 3,     // another computer's folder this device cannot change
  into_itself = 4,       // a folder onto itself or into one of its own folders
  already_there = 5,     // a move of files that are already in that folder
  remote_source = 6,     // a folder on another computer (a file comes down; a folder not yet),
                         // or files on another server than the target's
};

enum class place_kind : std::uint8_t { writable, read_only, remote_read_only, remote_writable };

struct drop_request {
  std::vector<std::string> sources;  // full paths (or keys, "photos:…", "remote:…")
  std::string target_dir;
  bool from_app = false;   // the drag started in our own window
  bool copy_key = false;   // Option on the Mac, Ctrl on Windows
  bool move_key = false;   // Cmd on the Mac, Shift on Windows
};

struct drop_decision {
  drop_verb verb = drop_verb::none;
  drop_refusal refusal = drop_refusal::nothing;
  [[nodiscard]] bool allowed() const noexcept { return verb != drop_verb::none; }
};

using classify_fn = std::function<place_kind(std::string_view path)>;

namespace drop_detail {
inline bool is_remote(place_kind k) noexcept {
  return k == place_kind::remote_read_only || k == place_kind::remote_writable;
}
// "remote:<server>/…" -> "remote:<server>/" (docs/plans/desktop-client.md keys); "" otherwise.
inline std::string_view server_of(std::string_view key) {
  constexpr std::string_view kPrefix = "remote:";
  if (key.substr(0, kPrefix.size()) != kPrefix) return {};
  const std::size_t slash = key.find('/', kPrefix.size());
  return slash == std::string_view::npos ? key : key.substr(0, slash + 1);
}
inline bool is_folder_key(std::string_view key) { return !key.empty() && key.back() == '/'; }
// ASCII letters folded to lower case: names that a case-insensitive volume takes as one.
inline std::string folded(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}
// Trailing separators dropped, except a root's ("/", "C:\\").
inline std::string trim_dir(std::string_view dir) {
  std::string p(dir);
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\') && !(p.size() == 3 && p[1] == ':')) p.pop_back();
  return p;
}
}  // namespace drop_detail

[[nodiscard]] inline drop_decision decide_drop(const drop_request& r, const classify_fn& classify) {
  drop_decision out;
  // An outside drag's files are not read while it hovers (the pasteboard is
  // only read on drop): with no sources it is judged by the folder alone, and
  // judged again with its files when it lands.
  if (r.target_dir.empty() || (r.sources.empty() && r.from_app)) return out;
  const place_kind target = classify(r.target_dir);
  if (target == place_kind::remote_read_only) {
    out.refusal = drop_refusal::remote_target;
    return out;
  }
  if (target == place_kind::read_only) {
    out.refusal = drop_refusal::read_only_target;
    return out;
  }
  const std::string target_dir = drop_detail::trim_dir(r.target_dir);
  bool all_here = !r.sources.empty();
  bool any_protected = false;
  bool remote_folder = false;
  for (const std::string& src : r.sources) {
    // A folder onto itself or anything inside it: Finder refuses that too.
    if (browse_path::within(target_dir, src)) {
      out.refusal = drop_refusal::into_itself;
      return out;
    }
    const place_kind k = classify(src);
    if (drop_detail::is_remote(k)) {
      // Another computer's file comes down (a download) or goes across its own server (the
      // service's move); nothing goes from one server to another, and a remote folder only
      // moves within its server (the service has no folder copy, this app no folder download).
      if (drop_detail::is_remote(target) &&
          drop_detail::server_of(src) != drop_detail::server_of(target_dir)) {
        out.refusal = drop_refusal::remote_source;
        return out;
      }
      if (drop_detail::is_folder_key(src)) remote_folder = true;
    }
    if (k == place_kind::read_only || k == place_kind::remote_read_only) any_protected = true;
    if (browse_path::parent_of(src) != target_dir) all_here = false;
  }
  drop_verb verb = r.from_app ? (r.copy_key ? drop_verb::copy : drop_verb::move)
                              : (r.move_key ? drop_verb::move : drop_verb::copy);
  // What cannot be removed from where it is (a Photos library item, a
  // read-only place) is copied, as Finder copies off a locked volume.
  if (verb == drop_verb::move && any_protected) verb = drop_verb::copy;
  // A remote folder goes only by the service's move, within its own server.
  if (remote_folder && (verb != drop_verb::move || !drop_detail::is_remote(target))) {
    out.refusal = drop_refusal::remote_source;
    return out;
  }
  if (verb == drop_verb::move && all_here) {
    out.refusal = drop_refusal::already_there;
    return out;
  }
  out.verb = verb;
  out.refusal = drop_refusal::none;
  return out;
}

// The short reason a refused target shows (both chromes use these words).
[[nodiscard]] constexpr const char* drop_refusal_words(drop_refusal r) noexcept {
  switch (r) {
    case drop_refusal::read_only_target: return "Read-only: nothing can be added here";
    case drop_refusal::remote_target: return "On another computer: read-only from here";
    case drop_refusal::into_itself: return "Can't put a folder inside itself";
    case drop_refusal::already_there: return "Already in this folder";
    case drop_refusal::remote_source: return "A folder on another computer can't be copied here";
    default: return "";
  }
}

// One file or folder of a transfer and where it lands.
struct transfer_step {
  std::string source;
  std::string dest;  // the full new path, its name never one that exists
};

// Where each source lands in `target_dir`: its own name, else "name (2).ext"
// and so on (io::unique_name), never one `exists` reports and never one an
// earlier source of the same batch took. A move skips sources already in the
// folder. `exists` is the caller's (a stat on the worker), so this stays pure.
template <class Exists>
[[nodiscard]] std::vector<transfer_step> plan_transfer(const std::vector<std::string>& sources,
                                                       std::string_view target_dir, drop_verb verb,
                                                       Exists&& exists) {
  std::vector<transfer_step> out;
  std::unordered_set<std::string> taken;
  const std::string dir = drop_detail::trim_dir(target_dir);
  const char sep = dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos ? '\\' : '/';
  const auto join = [&](std::string_view name) {
    std::string p = dir;
    if (p.empty() || (p.back() != '/' && p.back() != '\\')) p.push_back(sep);
    p.append(name);
    return p;
  };
  for (const std::string& src : sources) {
    if (verb == drop_verb::move && browse_path::parent_of(src) == dir) continue;
    std::string leaf = browse_path::leaf(src);
    // A Photos item key ("photos:<id>") has no name of its own here; the host
    // resolves it to its original before the plan (copy only).
    if (leaf.empty()) continue;
    // Taken by an earlier source of this batch: compared case-folded, as APFS and NTFS compare
    // names by default (IMG_1.JPG and img_1.jpg are one name there).
    std::string name = io::unique_name(leaf, [&](std::string_view candidate) {
      std::string full = join(candidate);
      return taken.count(drop_detail::folded(full)) != 0 || exists(full);
    });
    if (name.empty()) continue;  // every "(n)" up to the cap is taken
    std::string dest = join(name);
    taken.insert(drop_detail::folded(dest));
    out.push_back({src, std::move(dest)});
  }
  return out;
}

}  // namespace mv::shell
