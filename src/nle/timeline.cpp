// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/timeline.h"

#include <algorithm>
#include <set>

namespace mv::nle {
namespace {

using xml::kNone;

// Exact arithmetic for model building; an overflow (only absurd times reach
// one) leaves the operand and is reported by the caller's `ok`.
rational plus(rational a, rational b, bool& ok) {
  const auto r = add(a, b);
  if (!r) ok = false;
  return r ? *r : a;
}
rational minus(rational a, rational b, bool& ok) {
  const auto r = sub(a, b);
  if (!r) ok = false;
  return r ? *r : a;
}

struct kind_entry {
  std::string_view element;
  item_kind kind;
};
constexpr kind_entry kKinds[] = {
    {"asset-clip", item_kind::asset_clip}, {"clip", item_kind::clip},
    {"ref-clip", item_kind::ref_clip},     {"mc-clip", item_kind::mc_clip},
    {"sync-clip", item_kind::sync_clip},   {"title", item_kind::title},
    {"gap", item_kind::gap},               {"transition", item_kind::transition},
    {"audio", item_kind::audio},           {"video", item_kind::video},
    {"audition", item_kind::audition},     {"caption", item_kind::caption},
    {"live-drawing", item_kind::live_drawing}, {"spine", item_kind::storyline},
};

bool story_kind(std::string_view element, item_kind& out) {
  for (const kind_entry& k : kKinds) {
    if (k.element == element) {
      out = k.kind;
      return true;
    }
  }
  return false;
}

// A container clip's children are its own contents (its storyline, its
// primary item and what is anchored inside it, as a sync clip's recorder is):
// not opened, like a compound's.
bool contains_contents(item_kind k) {
  return k == item_kind::clip || k == item_kind::sync_clip || k == item_kind::audition;
}

bool is_effect(std::string_view n) {
  if (n.rfind("filter-", 0) == 0) return true;
  // Levels and pan are mixing, not effects; conform is how the clip fits.
  if (n.rfind("adjust-", 0) == 0) return n != "adjust-conform" && n != "adjust-volume" && n != "adjust-panner";
  return n == "mc-source" || n == "live-drawing";
}

std::string trim(std::string_view s) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return std::string(s);
}

int to_int(std::string_view s) {
  int v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return v;
    v = v * 10 + (c - '0');
  }
  return v;
}

int to_lane(std::string_view s) {
  if (s.empty()) return 0;
  return s[0] == '-' ? -to_int(s.substr(1)) : to_int(s);
}

std::string strip_version_suffix(std::string_view base) {
  // "Interview (Claude 3)" -> "Interview"
  std::string s(base);
  const std::size_t open = s.rfind(" (Claude ");
  if (open != std::string::npos && !s.empty() && s.back() == ')') {
    const std::string_view digits(s.data() + open + 9, s.size() - open - 10);
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      s.erase(open);
    }
  }
  return s;
}

}  // namespace

const char* kind_name(item_kind k) {
  switch (k) {
    case item_kind::asset_clip: return "clip";
    case item_kind::clip: return "container";
    case item_kind::ref_clip: return "compound";
    case item_kind::mc_clip: return "multicam";
    case item_kind::sync_clip: return "sync";
    case item_kind::title: return "title";
    case item_kind::gap: return "gap";
    case item_kind::transition: return "transition";
    case item_kind::audio: return "audio";
    case item_kind::video: return "video";
    case item_kind::audition: return "audition";
    case item_kind::caption: return "caption";
    case item_kind::live_drawing: return "drawing";
    case item_kind::storyline: return "storyline";
    case item_kind::other: return "other";
  }
  return "other";
}

std::string path_of_file_url(std::string_view url) {
  constexpr std::string_view kScheme = "file://";
  if (url.size() < kScheme.size()) return {};
  for (std::size_t i = 0; i < kScheme.size(); ++i) {
    const char c = url[i];
    const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (lower != kScheme[i]) return {};
  }
  std::string_view rest = url.substr(kScheme.size());
  if (rest.rfind("localhost/", 0) == 0) rest.remove_prefix(9);
  std::string out;
  if (!rest.empty() && rest[0] != '/') out = "//";  // file://server/share
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() + 0 && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
      out += static_cast<char>(hex(rest[i + 1]) * 16 + hex(rest[i + 2]));
      i += 2;
    } else {
      out += rest[i];
    }
  }
  // "/C:/x" -> "C:/x"
  if (out.size() >= 3 && out[0] == '/' && out[2] == ':' &&
      ((out[1] >= 'A' && out[1] <= 'Z') || (out[1] >= 'a' && out[1] <= 'z'))) {
    out.erase(0, 1);
  }
  // A folder URL ends in '/': keep it (a library is a folder).
  return out;
}

std::string next_version_name(std::string_view base, const std::vector<std::string>& taken) {
  const std::string stem = strip_version_suffix(base);
  for (int n = 1; n < 10000; ++n) {
    std::string name = stem + " (Claude " + std::to_string(n) + ")";
    if (std::find(taken.begin(), taken.end(), name) == taken.end()) return name;
  }
  return stem + " (Claude)";
}

rational timeline::frame_of(std::string_view format) const {
  const auto it = frames_.find(std::string(format));
  return it == frames_.end() ? rational{0, 1} : it->second;
}

void timeline::read_resources() {
  assets_.clear();
  names_.clear();
  frames_.clear();
  sizes_.clear();
  media_files_.clear();
  const std::uint32_t root = doc_.root();
  const std::uint32_t res = doc_.first(root, "resources");
  if (res == kNone) return;
  for (const std::uint32_t r : doc_.elements(res)) {
    const xml::node& n = doc_.at(r);
    const std::string id(doc_.attr_or(r, "id", ""));
    if (id.empty()) continue;
    if (n.name == "format") {
      if (const auto f = parse_time(doc_.attr_or(r, "frameDuration", ""))) frames_[id] = *f;
      sizes_[id] = {to_int(doc_.attr_or(r, "width", "")), to_int(doc_.attr_or(r, "height", ""))};
    } else if (n.name == "asset") {
      asset a;
      a.id = id;
      a.name.assign(doc_.attr_or(r, "name", ""));
      a.uid.assign(doc_.attr_or(r, "uid", ""));
      a.start = parse_time(doc_.attr_or(r, "start", "0s")).value_or(rational{});
      a.duration = parse_time(doc_.attr_or(r, "duration", "0s")).value_or(rational{});
      a.has_video = doc_.attr_or(r, "hasVideo", "0") == "1";
      a.has_audio = doc_.attr_or(r, "hasAudio", "0") == "1";
      a.format.assign(doc_.attr_or(r, "format", ""));
      std::string_view src = doc_.attr_or(r, "src", "");
      for (const std::uint32_t m : doc_.elements(r, "media-rep")) {
        if (doc_.attr_or(m, "kind", "original-media") == "original-media" || src.empty()) {
          src = doc_.attr_or(m, "src", src);
          if (doc_.attr_or(m, "kind", "original-media") == "original-media") break;
        }
      }
      a.path = path_of_file_url(src);
      names_[id] = a.name;
      assets_[id] = std::move(a);
    } else {
      names_[id].assign(doc_.attr_or(r, "name", ""));
    }
  }
  // What each media (compound, multicam) plays, through nested media.
  std::map<std::string, std::vector<std::string>> direct;  // media id -> refs inside it
  for (const std::uint32_t r : doc_.elements(res, "media")) {
    const std::string id(doc_.attr_or(r, "id", ""));
    std::vector<std::uint32_t> stack{r};
    while (!stack.empty()) {
      const std::uint32_t c = stack.back();
      stack.pop_back();
      if (const std::string* ref = doc_.attr(c, "ref")) direct[id].push_back(*ref);
      for (const std::uint32_t k : doc_.elements(c)) stack.push_back(k);
    }
  }
  for (const auto& [id, refs] : direct) {
    std::set<std::string> files, seen;
    std::vector<std::string> work = refs;
    while (!work.empty()) {
      const std::string ref = work.back();
      work.pop_back();
      if (!seen.insert(ref).second) continue;
      if (const auto a = assets_.find(ref); a != assets_.end()) {
        if (!a->second.path.empty()) files.insert(a->second.path);
      } else if (const auto d = direct.find(ref); d != direct.end()) {
        work.insert(work.end(), d->second.begin(), d->second.end());
      }
    }
    media_files_[id] = std::vector<std::string>(files.begin(), files.end());
  }
}

bool timeline::find_project(std::string_view wanted, std::string* error) {
  project_ = {};
  project_names_.clear();
  std::vector<std::uint32_t> projects;
  std::vector<std::uint32_t> stack{doc_.root()};
  while (!stack.empty()) {
    const std::uint32_t c = stack.back();
    stack.pop_back();
    const xml::node& n = doc_.at(c);
    if (n.name == "project") {
      projects.push_back(c);
      continue;
    }
    if (n.name != "fcpxml" && n.name != "library" && n.name != "event") continue;
    const auto kids = doc_.elements(c);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
  }
  for (const std::uint32_t p : projects) project_names_.emplace_back(doc_.attr_or(p, "name", ""));
  if (projects.empty()) {
    if (error) *error = "the FCPXML holds no project (drag a project, not clips, from Final Cut Pro)";
    return false;
  }
  std::uint32_t chosen = projects.front();
  if (!wanted.empty()) {
    chosen = kNone;
    for (const std::uint32_t p : projects) {
      if (doc_.attr_or(p, "name", "") == wanted) {
        chosen = p;
        break;
      }
    }
    if (chosen == kNone) {
      if (error) *error = "no project of that name in the FCPXML";
      return false;
    }
  }
  return read_project(chosen, error);
}

bool timeline::read_project(std::uint32_t chosen, std::string* error) {
  project_ = {};
  project_info& p = project_;
  p.node = chosen;
  p.name.assign(doc_.attr_or(chosen, "name", ""));
  p.uid.assign(doc_.attr_or(chosen, "uid", ""));
  const std::uint32_t parent = doc_.at(chosen).parent;
  if (parent != kNone && doc_.at(parent).name == "event") {
    p.event = parent;
    p.event_name.assign(doc_.attr_or(parent, "name", ""));
    const std::uint32_t lib = doc_.at(parent).parent;
    if (lib != kNone && doc_.at(lib).name == "library") p.library.assign(doc_.attr_or(lib, "location", ""));
  }
  p.sequence = doc_.first(chosen, "sequence");
  if (p.sequence == kNone) {
    if (error) *error = "the project has no sequence";
    return false;
  }
  p.spine = doc_.first(p.sequence, "spine");
  if (p.spine == kNone) {
    if (error) *error = "the project's sequence has no spine";
    return false;
  }
  p.tc_start = parse_time(doc_.attr_or(p.sequence, "tcStart", "0s")).value_or(rational{});
  p.duration = parse_time(doc_.attr_or(p.sequence, "duration", "0s")).value_or(rational{});
  p.format.assign(doc_.attr_or(p.sequence, "format", ""));
  p.frame = frame_of(p.format);
  if (const auto s = sizes_.find(p.format); s != sizes_.end()) {
    p.width = s->second.first;
    p.height = s->second.second;
  }
  return true;
}

bool timeline::load(std::string fcpxml, std::string* error, std::string_view project) {
  items_.clear();
  xml::parse_error pe;
  if (!doc_.parse(std::move(fcpxml), &pe)) {
    if (error) *error = "not XML at byte " + std::to_string(pe.offset) + ": " + pe.message;
    return false;
  }
  if (doc_.at(doc_.root()).name != "fcpxml") {
    if (error) *error = "not FCPXML (the document element is not <fcpxml>)";
    return false;
  }
  version_.assign(doc_.attr_or(doc_.root(), "version", ""));
  wanted_project_.assign(project);
  read_resources();
  if (!find_project(project, error)) return false;
  reindex();
  return true;
}

void timeline::reindex() {
  items_.clear();
  // Resources and the project node survive edits; times may not.
  read_resources();
  if (project_.node == kNone || !read_project(project_.node, nullptr)) return;
  if (project_.spine == kNone) return;
  walk_spine(project_.spine, -1, -1, rational{}, project_.tc_start);
}

int timeline::add_item(std::uint32_t node, int parent, int storyline, bool in_storyline, rational at) {
  const xml::node& n = doc_.at(node);
  item it;
  it.node = node;
  if (!story_kind(n.name, it.kind)) it.kind = item_kind::other;
  it.parent = parent;
  it.storyline = storyline;
  it.in_storyline = in_storyline;
  it.lane = to_lane(doc_.attr_or(node, "lane", ""));
  it.at = at;
  it.name.assign(doc_.attr_or(node, "name", ""));
  it.ref.assign(doc_.attr_or(node, "ref", ""));
  it.enabled = doc_.attr_or(node, "enabled", "1") != "0";
  bool times_ok = true;
  auto time = [&](std::string_view a, std::string_view fallback) {
    const auto t = parse_time(doc_.attr_or(node, a, fallback));
    if (!t) times_ok = false;
    return t.value_or(rational{});
  };
  it.offset = time("offset", "0s");
  it.start = time("start", "0s");
  if (doc_.attr(node, "duration")) {
    it.duration = time("duration", "0s");
  } else if (const auto a = assets_.find(it.ref); a != assets_.end() && it.kind == item_kind::asset_clip) {
    it.duration = a->second.duration;  // an asset-clip may leave its duration to the asset
  } else if (it.kind != item_kind::storyline && it.kind != item_kind::other && it.kind != item_kind::audition) {
    times_ok = false;
  }
  for (const char* r : {"role", "videoRole", "audioRole"}) {
    if (const std::string* v = doc_.attr(node, r)) {
      if (!it.role.empty()) it.role += ", ";
      it.role += *v;
    }
  }
  if (doc_.attr(node, "audioStart") || doc_.attr(node, "audioDuration")) {
    it.split_audio = true;
    it.audio_start = time("audioStart", "0s");
    it.audio_duration = time("audioDuration", "0s");
  }
  if (const auto a = assets_.find(it.ref); a != assets_.end()) {
    it.source = a->second.path;
    bool ok = true;
    it.source_in = minus(it.start, a->second.start, ok);
    if (!ok) times_ok = false;
  }
  if (it.kind == item_kind::title || it.kind == item_kind::caption) {
    // The words are in the text-style runs; the whitespace around them is
    // the file's indentation.
    for (const std::uint32_t t : doc_.elements(node, "text")) {
      std::string line;
      const auto runs = doc_.elements(t, "text-style");
      if (runs.empty()) {
        line = trim(doc_.text_of(t));
      } else {
        for (const std::uint32_t run : runs) line += doc_.text_of(run);
      }
      if (!it.text.empty()) it.text += "\n";
      it.text += line;
    }
  }
  for (const std::uint32_t c : doc_.elements(node)) {
    const std::string& cn = doc_.at(c).name;
    if (is_effect(cn)) it.has_effects = true;
    if (cn == "timeMap") it.retimed = true;
    if (cn == "conform-rate" && doc_.attr_or(c, "scaleEnabled", "1") == "1" && doc_.attr(c, "srcFrameRate")) {
      it.retimed = true;  // a rate conform plays the file faster or slower
    }
  }
  it.opaque = it.kind == item_kind::ref_clip || it.kind == item_kind::mc_clip || it.kind == item_kind::sync_clip ||
              it.kind == item_kind::clip || it.kind == item_kind::audition;
  if (it.kind == item_kind::other) {
    it.not_editable = "an element MediaViewer does not know (<" + n.name + ">): carried as it is";
  } else if (!times_ok) {
    it.not_editable = "a time MediaViewer could not read: carried as it is";
  } else if (it.kind == item_kind::storyline) {
    it.not_editable = "a connected storyline: its clips are edited, not the storyline";
  } else {
    it.editable = true;
  }
  const int self = static_cast<int>(items_.size());
  items_.push_back(std::move(it));
  return self;
}

void timeline::walk_spine(std::uint32_t spine, int parent, int storyline, rational base_at, rational origin) {
  bool ok = true;
  for (const std::uint32_t c : doc_.elements(spine)) {
    // A spine holds only story elements and transitions: anything else is an
    // element a newer FCP writes, listed as `other` and carried.
    const rational offset = parse_time(doc_.attr_or(c, "offset", "0s")).value_or(rational{});
    const rational at = plus(base_at, minus(offset, origin, ok), ok);
    const int self = add_item(c, parent, storyline, true, at);
    walk_anchored(self);
  }
}

void timeline::walk_anchored(int self) {
  const std::uint32_t node = items_[static_cast<std::size_t>(self)].node;
  const item_kind kind = items_[static_cast<std::size_t>(self)].kind;
  bool ok = true;
  if (kind == item_kind::storyline) {
    // A connected storyline: its clips sit one after another from its own
    // start, which is where its first element begins.
    const auto kids = doc_.elements(node);
    rational origin{};
    for (const std::uint32_t c : kids) {
      item_kind k;
      if (story_kind(doc_.at(c).name, k)) {
        origin = parse_time(doc_.attr_or(c, "offset", "0s")).value_or(rational{});
        break;
      }
    }
    const rational at = items_[static_cast<std::size_t>(self)].at;
    walk_spine(node, self, self, at, origin);
    return;
  }
  const bool contents = contains_contents(kind);
  for (const std::uint32_t c : doc_.elements(node)) {
    const xml::node& n = doc_.at(c);
    const item& me = items_[static_cast<std::size_t>(self)];
    if (n.name == "marker" || n.name == "chapter-marker") {
      marker m;
      m.node = c;
      m.kind = n.name;
      m.start = parse_time(doc_.attr_or(c, "start", "0s")).value_or(rational{});
      m.duration = parse_time(doc_.attr_or(c, "duration", "0s")).value_or(rational{});
      m.at = plus(me.at, minus(m.start, me.start, ok), ok);
      m.value.assign(doc_.attr_or(c, "value", ""));
      if (const std::string* done = doc_.attr(c, "completed")) {
        m.todo = true;
        m.completed = *done == "1";
      }
      items_[static_cast<std::size_t>(self)].markers.push_back(std::move(m));
      continue;
    }
    item_kind k;
    if (!story_kind(n.name, k)) continue;
    if (contents || kind == item_kind::other) continue;  // inside a container clip: not opened
    const rational offset = parse_time(doc_.attr_or(c, "offset", "0s")).value_or(rational{});
    const rational at = plus(me.at, minus(offset, me.start, ok), ok);
    const int child = add_item(c, self, -1, false, at);
    walk_anchored(child);
  }
}

rational timeline::length() const {
  rational end{};
  bool ok = true;
  for (const item& it : items_) {
    if (it.parent != -1 || !it.in_storyline || it.kind == item_kind::transition) continue;
    const rational e = plus(it.at, it.duration, ok);
    if (e > end) end = e;
  }
  return end;
}

std::vector<std::string> timeline::media_paths() const {
  std::set<std::string> out;
  for (const item& it : items_) {
    if (!it.source.empty()) out.insert(it.source);
    if (const auto m = media_files_.find(it.ref); m != media_files_.end()) out.insert(m->second.begin(), m->second.end());
    // Auditions and container clips hold their media inside.
    if (it.opaque) {
      std::vector<std::uint32_t> stack{it.node};
      while (!stack.empty()) {
        const std::uint32_t c = stack.back();
        stack.pop_back();
        if (const std::string* ref = doc_.attr(c, "ref")) {
          if (const auto a = assets_.find(*ref); a != assets_.end() && !a->second.path.empty()) {
            out.insert(a->second.path);
          }
          if (const auto m = media_files_.find(*ref); m != media_files_.end()) {
            out.insert(m->second.begin(), m->second.end());
          }
        }
        for (const std::uint32_t k : doc_.elements(c)) stack.push_back(k);
      }
    }
  }
  return std::vector<std::string>(out.begin(), out.end());
}

std::string timeline::describe() const {
  std::string out;
  out += "project \"" + project_.name + "\" event \"" + project_.event_name + "\" format " + project_.format +
         " frame " + format_time(project_.frame) + " tcStart " + format_time(project_.tc_start) + " duration " +
         format_time(project_.duration) + "\n";
  for (std::size_t i = 0; i < items_.size(); ++i) {
    const item& it = items_[i];
    out += std::to_string(i) + " " + kind_name(it.kind) + " parent " + std::to_string(it.parent) + " lane " +
           std::to_string(it.lane) + (it.in_storyline ? " storyline " + std::to_string(it.storyline) : " anchored") +
           " at " + format_time(it.at) + " offset " + format_time(it.offset) + " start " + format_time(it.start) +
           " duration " + format_time(it.duration) + (it.enabled ? "" : " disabled") + " name \"" + it.name +
           "\" ref " + it.ref + " role \"" + it.role + "\" file \"" + it.source + "\"" +
           (it.has_effects ? " effects" : "") + (it.retimed ? " retimed" : "") +
           (it.split_audio ? " split " + format_time(it.audio_start) + "+" + format_time(it.audio_duration) : "") +
           (it.text.empty() ? "" : " text \"" + it.text + "\"") + "\n";
    for (const marker& m : it.markers) {
      out += "  " + m.kind + " at " + format_time(m.at) + " start " + format_time(m.start) + " duration " +
             format_time(m.duration) + " \"" + m.value + "\"" + (m.todo ? (m.completed ? " done" : " todo") : "") +
             "\n";
    }
  }
  return out;
}

std::string timeline::write_version(std::string_view new_name) const {
  xml::document d = doc_;  // node ids are the same in the copy
  const std::uint32_t root = d.root();
  const std::uint32_t project = project_.node;
  // Only this project in its event, only its event in its library, nothing
  // else at the top: the import makes one new project and nothing more.
  std::uint32_t keep = project;
  for (std::uint32_t p = d.at(project).parent; p != xml::kNone; p = d.at(p).parent) {
    if (d.at(p).kind != xml::node_kind::element) break;
    for (const std::uint32_t c : d.elements(p)) {
      const std::string& cn = d.at(c).name;
      if (c == keep || cn == "resources" || cn == "import-options") continue;
      d.remove(c);
    }
    keep = p;
    if (p == root) break;
  }
  d.set_attr(project, "name", new_name);
  d.remove_attr(project, "uid");
  d.remove_attr(project, "modDate");
  // Resources: keep what the project references, transitively.
  const std::uint32_t res = d.first(root, "resources");
  if (res != xml::kNone) {
    std::map<std::string, std::uint32_t> by_id;
    for (const std::uint32_t r : d.elements(res)) {
      if (const std::string* id = d.attr(r, "id")) by_id[*id] = r;
    }
    std::set<std::string> used;
    std::vector<std::uint32_t> work{project};
    std::set<std::uint32_t> visited;
    while (!work.empty()) {
      const std::uint32_t top = work.back();
      work.pop_back();
      if (!visited.insert(top).second) continue;
      std::vector<std::uint32_t> stack{top};
      while (!stack.empty()) {
        const std::uint32_t c = stack.back();
        stack.pop_back();
        for (const xml::attribute& a : d.at(c).attrs) {
          if (a.name == "id") continue;
          // Values are single ids in FCPXML; split on spaces anyway.
          std::size_t from = 0;
          while (from <= a.value.size()) {
            const std::size_t sp = a.value.find(' ', from);
            const std::string tok = a.value.substr(from, sp == std::string::npos ? std::string::npos : sp - from);
            if (const auto r = by_id.find(tok); r != by_id.end() && used.insert(tok).second) work.push_back(r->second);
            if (sp == std::string::npos) break;
            from = sp + 1;
          }
        }
        for (const std::uint32_t k : d.elements(c)) stack.push_back(k);
      }
    }
    for (const auto& [id, r] : by_id) {
      if (!used.count(id)) d.remove(r);
    }
  }
  return d.write();
}

}  // namespace mv::nle
