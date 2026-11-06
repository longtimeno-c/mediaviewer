// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Compare": what the Edit header's and the rail's Compare (and
// `Y` held) compare the still on screen with, and the before/after split's
// maths. One body for both hosts and both labs.
//
//   * With edits on the stack, the before is the photo with no edit.
//   * An Enhance result (doc 29: `<stem>-enhance.jpg`, `-restored`,
//     `-extended`, `-removed`, `-eyes`, `-faces`, each also as "name (2)")
//     or a file the app was told is a result (note_result): the before is the
//     photo it was made from.
//   * Otherwise there is nothing to compare, and the chromes say why.
//
// No I/O here. The registry of results the app wrote is a mutex and a few
// strings; a caller that needs the disk to find a source passes the existence
// check (source_of) and runs it off the UI thread, or answers it from the
// folder listing it already holds (is_source_name).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mv::shell {

// What there is to compare the still on screen with. The chromes read it (the
// Mac's mv_edit_view.compare, Windows' chrome_edit_args.compare) to enable
// Compare and to say why it is off.
enum class compare_kind : std::int32_t {
  none = 0,            // no edits, and not an Enhance result: "No edits yet to compare"
  edits = 1,           // before = the photo with no edit
  source = 2,          // an Enhance result: before = the photo it was made from
  source_missing = 3,  // named like a result, but its photo is not beside it
  looking = 4,         // named like a result; its photo is being looked for
};

[[nodiscard]] constexpr bool can_compare(compare_kind k) noexcept {
  return k == compare_kind::edits || k == compare_kind::source;
}

// What the host says when Compare (or Y) has nothing to show.
[[nodiscard]] constexpr const char* compare_unavailable_reason(compare_kind k) noexcept {
  switch (k) {
    case compare_kind::source_missing: return "Compare: the original photo is not beside this one";
    case compare_kind::looking: return "Compare: looking for the original photo…";
    case compare_kind::none: return "No edits yet to compare";
    case compare_kind::edits:
    case compare_kind::source: break;
  }
  return "";
}

// Where a result's source lies in it, in the result's uv (x, y, w, h); w = 0
// is not known (the labs place it by size, source_map).
struct compare_rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  [[nodiscard]] bool known() const noexcept { return w > 0.0f && h > 0.0f; }
};

// How a result's pixels relate to its source's (input_snapshot::before_layout).
enum class compare_layout : std::uint8_t {
  same_picture = 0,  // the same picture, maybe larger (Enhance image 2x / 4x, a face fix, Remove)
  extended = 1,      // the source inside a larger frame at its own scale (Extend)
};

// The words Enhance (and the hosts' face fixes) put after the stem (doc 29).
inline constexpr std::string_view kCompareResultSuffixes[] = {"enhance", "restored", "extended", "removed",
                                                               "eyes",    "faces",    "restyled"};
// The still extensions a source may have, the likeliest first.
inline constexpr std::string_view kCompareSourceExtensions[] = {
    "jpg", "jpeg", "heic", "heif", "png", "tif", "tiff", "webp", "avif", "dng", "cr2", "cr3",
    "nef", "nrw",  "arw",  "srf",  "sr2", "raf", "orf",  "rw2",  "pef",  "srw", "x3f", "3fr",
    "iiq", "erf",  "mos",  "kdc",  "rwl", "bmp", "jpe",  "gif",  "ico"};

namespace compare_detail {

[[nodiscard]] constexpr char lower(char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

[[nodiscard]] inline std::size_t name_start(std::string_view path) noexcept {
  const std::size_t sep = path.find_last_of("/\\");
  return sep == std::string_view::npos ? 0 : sep + 1;
}

// " (2)" at the end of a name: the first free name (io/collision_name.h).
[[nodiscard]] inline std::string_view strip_free_number(std::string_view base) noexcept {
  if (base.size() < 5 || base.back() != ')') return base;
  const std::size_t open = base.rfind(" (");
  if (open == std::string_view::npos || open + 3 > base.size() - 1) return base;
  for (std::size_t i = open + 2; i + 1 < base.size(); ++i) {
    if (base[i] < '0' || base[i] > '9') return base;
  }
  return base.substr(0, open);
}

// The suffix a result's name ends with ("" for none), and the rest before it.
[[nodiscard]] inline std::pair<std::string_view, std::string_view> split_result_name(
    std::string_view name) noexcept {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return {};
  const std::string_view ext = name.substr(dot + 1);
  if (!iequals(ext, "jpg") && !iequals(ext, "jpeg") && !iequals(ext, "png")) return {};
  const std::string_view base = strip_free_number(name.substr(0, dot));
  for (const std::string_view suffix : kCompareResultSuffixes) {
    if (base.size() <= suffix.size() + 1) continue;
    const std::size_t at = base.size() - suffix.size();
    if (base[at - 1] != '-' || !iequals(base.substr(at), suffix)) continue;
    return {base.substr(0, at - 1), suffix};
  }
  return {};
}

struct noted_result {
  std::string result;
  std::string source;
  compare_rect where;
};

struct registry {
  static constexpr std::size_t kMax = 64;  // the newest results the app wrote this run
  std::mutex mutex;
  std::vector<noted_result> entries;
};

[[nodiscard]] inline registry& results() {
  static registry r;
  return r;
}

}  // namespace compare_detail

// "dir/IMG_0007-enhance (2).jpg" -> "dir/IMG_0007": the path of the photo it
// was made from, without its extension; "" when the name is not a result's.
[[nodiscard]] inline std::string result_source_stem(std::string_view result_path) {
  const std::size_t start = compare_detail::name_start(result_path);
  const std::string_view stem = compare_detail::split_result_name(result_path.substr(start)).first;
  if (stem.empty()) return {};
  std::string out(result_path.substr(0, start));
  out.append(stem);
  return out;
}

[[nodiscard]] inline bool looks_like_result(std::string_view path) {
  return !compare_detail::split_result_name(path.substr(compare_detail::name_start(path))).first.empty();
}

// How the before lies in a result named `path` (Extend's sits inside its frame).
[[nodiscard]] inline compare_layout result_layout(std::string_view path) {
  const std::string_view suffix =
      compare_detail::split_result_name(path.substr(compare_detail::name_start(path))).second;
  return suffix == "extended" ? compare_layout::extended : compare_layout::same_picture;
}

// Whether `candidate` (a path from the host's folder listing) can be the photo
// the result `result_path` was made from: the same stem, any still extension,
// case ignored (the listing's own spelling is the one to open, so its cache
// entry is hit).
[[nodiscard]] inline bool is_source_name(std::string_view result_path, std::string_view candidate) {
  const std::string stem = result_source_stem(result_path);
  if (stem.empty() || candidate.size() <= stem.size() + 1 || candidate[stem.size()] != '.') return false;
  if (!compare_detail::iequals(candidate.substr(0, stem.size()), stem)) return false;
  const std::string_view ext = candidate.substr(stem.size() + 1);
  for (const std::string_view e : kCompareSourceExtensions) {
    if (compare_detail::iequals(ext, e)) return true;
  }
  return false;
}

// The paths a result's source may have, the likeliest first (lower-case
// extensions: Windows and macOS volumes ignore case by default).
[[nodiscard]] inline std::vector<std::string> source_candidates(std::string_view result_path) {
  std::vector<std::string> out;
  const std::string stem = result_source_stem(result_path);
  if (stem.empty()) return out;
  out.reserve(std::size(kCompareSourceExtensions));
  for (const std::string_view e : kCompareSourceExtensions) {
    std::string c = stem;
    c.push_back('.');
    c.append(e);
    out.push_back(std::move(c));
  }
  return out;
}

// [any thread] The app wrote `result_path` from `source_path` (a file, or on
// the Mac a Photos-library key for either). `where`: the source's place in the
// result when the writer knows it (Extend's frame), else unknown. The newest
// kMax are kept for the run; a second note of one result replaces the first.
inline void note_result(std::string result_path, std::string source_path, compare_rect where = {}) {
  if (result_path.empty() || source_path.empty() || result_path == source_path) return;
  auto& r = compare_detail::results();
  std::lock_guard lock(r.mutex);
  r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(),
                                 [&](const compare_detail::noted_result& e) { return e.result == result_path; }),
                  r.entries.end());
  if (r.entries.size() >= compare_detail::registry::kMax) r.entries.erase(r.entries.begin());
  r.entries.push_back({std::move(result_path), std::move(source_path), where});
}

// [any thread] What note_result said about `path` ("" and an unknown rect when
// nothing).
[[nodiscard]] inline std::string noted_source(const std::string& path, compare_rect* where = nullptr) {
  auto& r = compare_detail::results();
  std::lock_guard lock(r.mutex);
  for (auto it = r.entries.rbegin(); it != r.entries.rend(); ++it) {
    if (it->result != path) continue;
    if (where) *where = it->where;
    return it->source;
  }
  if (where) *where = {};
  return {};
}

// [any thread] The photo `path` was made from: the one note_result named, else
// the first of source_candidates that `exists(const std::string&)` accepts
// (the caller's check: a stat on a worker, or a look in its listing); "" when
// none. `exists` is never called for a path that is not a result's name.
template <class Exists>
[[nodiscard]] std::string source_of(const std::string& path, Exists&& exists) {
  std::string noted = noted_source(path);
  if (!noted.empty()) return noted;
  for (std::string& c : source_candidates(path)) {
    if (c != path && exists(static_cast<const std::string&>(c))) return std::move(c);
  }
  return {};
}

// ---- the split on the canvas -------------------------------------------------

// The before's texture onto the after's picture frame (rw x rh): output uv ->
// source uv, the blit's uv_map (gfx/blit.h, blit_metal.h), and whether a
// sample past the source shows the background. The same picture, larger or
// not, fills the frame (Enhance image's 2x / 4x, a face fix, Remove); a
// different aspect is fitted inside it, centred. An Extend result's source
// lies inside the bigger frame at its own scale: where the writer said
// (`where`), else centred (Extend's presets grow evenly), else fitted.
struct compare_map {
  float m[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  bool clip = false;
};

[[nodiscard]] inline compare_map source_map(float rw, float rh, float sw, float sh,
                                            compare_layout layout = compare_layout::same_picture,
                                            compare_rect where = {}) noexcept {
  compare_map out;
  if (!(rw > 0.0f && rh > 0.0f && sw > 0.0f && sh > 0.0f)) return out;
  compare_rect r = where;
  if (!r.known()) {
    const float ra = rw / rh;
    const float sa = sw / sh;
    if (layout == compare_layout::same_picture && std::fabs(ra - sa) <= 0.005f * sa) return out;
    if (layout == compare_layout::extended && sw <= rw * 1.001f && sh <= rh * 1.001f) {
      r.w = std::min(1.0f, sw / rw);
      r.h = std::min(1.0f, sh / rh);
    } else {
      const float s = std::min(rw / sw, rh / sh);
      r.w = std::min(1.0f, sw * s / rw);
      r.h = std::min(1.0f, sh * s / rh);
    }
    r.x = (1.0f - r.w) * 0.5f;
    r.y = (1.0f - r.h) * 0.5f;
  }
  out.m[0] = 1.0f / r.w;
  out.m[1] = 0.0f;
  out.m[2] = -r.x / r.w;
  out.m[3] = 0.0f;
  out.m[4] = 1.0f / r.h;
  out.m[5] = -r.y / r.h;
  out.clip = true;
  return out;
}

// Lab -> host: where the split is on screen, in the client's physical pixels,
// published after each frame that drew it (present_lab*.h compare_frame()).
// [x0, x1] x [y0, y1] is the after's picture as far as the canvas shows it.
struct compare_frame {
  bool valid = false;
  float x0 = 0.0f;
  float x1 = 0.0f;
  float y0 = 0.0f;
  float y1 = 0.0f;
  float divider_x = 0.0f;
};

// Half the divider's grab band, DIP: the line is 2 wide, the handle 28.
inline constexpr float kCompareGrabDip = 14.0f;
inline constexpr float kCompareHandleDip = 14.0f;  // the round handle's radius

[[nodiscard]] inline compare_frame compare_frame_of(float view_x, float view_y, float view_w, float view_h,
                                                    float pic_x0, float pic_y0, float pic_x1, float pic_y1,
                                                    float split) noexcept {
  compare_frame f;
  f.x0 = std::max(view_x, std::min(pic_x0, pic_x1));
  f.x1 = std::min(view_x + view_w, std::max(pic_x0, pic_x1));
  f.y0 = std::max(view_y, std::min(pic_y0, pic_y1));
  f.y1 = std::min(view_y + view_h, std::max(pic_y0, pic_y1));
  if (!(f.x1 - f.x0 >= 2.0f && f.y1 - f.y0 >= 2.0f)) return compare_frame{};
  f.divider_x = std::round(f.x0 + std::clamp(split, 0.0f, 1.0f) * (f.x1 - f.x0));
  f.valid = true;
  return f;
}

// A press at (x, y) grabs the divider: within the grab band of the line, or on
// the handle at the middle of the picture.
[[nodiscard]] inline bool divider_hit(const compare_frame& f, float x, float y, float dpi_scale) noexcept {
  if (!f.valid) return false;
  const float s = dpi_scale > 0.0f ? dpi_scale : 1.0f;
  if (y < f.y0 || y > f.y1) return false;
  return std::fabs(x - f.divider_x) <= kCompareGrabDip * s;
}

// The split a drag to `x` sets: its place across the picture's visible span.
[[nodiscard]] inline float split_at(const compare_frame& f, float x) noexcept {
  if (!f.valid || f.x1 <= f.x0) return 0.5f;
  return std::clamp((x - f.x0) / (f.x1 - f.x0), 0.0f, 1.0f);
}

}  // namespace mv::shell
