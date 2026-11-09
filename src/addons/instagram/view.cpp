// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/instagram/view.h"

#include <algorithm>
#include <array>
#include <charconv>

#include "core/json.h"

namespace mv::instagram {
namespace {

using json::writer;

// The strip never reads as a blank band: empty places hold the shape until
// the first few are chosen.
constexpr std::size_t kPlaces = 7;

void text(writer& w, std::string_view id, std::string_view words, std::string_view style, bool hidden = false) {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(words);
  w.key("style").string(style);
  if (hidden) w.key("hidden").boolean(true);
  w.end_object();
}

void button(writer& w, std::string_view id, std::string_view label, std::string_view style, bool disabled,
            bool hidden = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  if (!style.empty()) w.key("style").string(style);
  if (disabled) w.key("disabled").boolean(true);
  if (hidden) w.key("hidden").boolean(true);
  w.end_object();
}

std::string count_words(const post& p) {
  return std::to_string(p.items().size()) + " of " + std::to_string(kMaxChosen);
}

void tile(writer& w, const view_state& v, const chosen& c, std::size_t index) {
  const tile_size size = tile_of(v.p->current());
  const std::string uid = std::to_string(c.uid);
  w.begin_object().key("type").string("stack").key("id").string("w" + uid).key("axis").string("vertical");
  w.key("spacing").integer(6).key("align").string("center").key("children").begin_array();
  {
    w.begin_object().key("type").string("surface").key("id").string(tile_node_id(c.uid));
    w.key("surface").string(surface_id(c.uid)).key("width").integer(size.w).key("height").integer(size.h);
    w.key("interactive").boolean(true);
    w.key("label").string((c.video ? "Clip " : "Photo ") + std::to_string(index + 1) + ": " + c.name);
    w.end_object();
    const bool bad = std::find(v.unreadable.begin(), v.unreadable.end(), c.uid) != v.unreadable.end();
    std::string words = std::to_string(index + 1);
    if (bad) {
      words += " · Can't read this";
    } else if (c.video) {
      words += " · Clip";
    }
    text(w, "n" + uid, words, "caption");
  }
  w.end_array().end_object();
}

void place(writer& w, const view_state& v, std::size_t i) {
  const tile_size size = tile_of(v.p->current());
  const std::string n = std::to_string(i);
  w.begin_object().key("type").string("stack").key("id").string("g" + n).key("axis").string("vertical");
  w.key("spacing").integer(6).key("align").string("center").key("children").begin_array();
  w.begin_object().key("type").string("surface").key("id").string("place" + n);
  w.key("surface").string("place" + n).key("width").integer(size.w).key("height").integer(size.h);
  w.key("label").string("Empty place").end_object();
  text(w, "gn" + n, " ", "caption");
  w.end_array().end_object();
}

void strip_node(writer& w, const view_state& v) {
  w.begin_object().key("type").string("stack").key("id").string("strip").key("axis").string("horizontal");
  w.key("spacing").integer(10).key("animate").boolean(true).key("children").begin_array();
  const auto& items = v.p->items();
  for (std::size_t i = 0; i < items.size(); ++i) tile(w, v, items[i], i);
  for (std::size_t i = items.size(); i < kPlaces; ++i) place(w, v, i);
  w.end_array().end_object();
}

void selection(writer& w, const view_state& v) {
  w.key("selection").begin_array();
  for (const chosen& c : v.p->items()) w.string(c.path);
  w.end_array();
}

void grid_rows(writer& w, const view_state& v) {
  w.key("items").begin_array();
  if (v.rows) {
    for (std::size_t i = 0; i < v.shown; ++i) {
      const gallery_row& r = *(*v.rows)[i];
      w.begin_object().key("path").string(r.path).key("label").string(r.name);
      if (r.video) w.key("badge").string("video");
      w.end_object();
    }
  }
  w.end_array();
}

std::string more_words(const view_state& v) {
  const std::size_t left = v.rows ? v.rows->size() - v.shown : 0;
  return "Show " + std::to_string(std::min(left, kGalleryPage)) + " more";
}

bool more_hidden(const view_state& v) {
  return v.mode != tool_mode::choose || !v.rows || v.shown >= v.rows->size();
}
bool choosing(const view_state& v) { return v.mode == tool_mode::choose; }
bool source_hidden(const view_state& v) { return !v.has_library || !choosing(v); }

std::string empty_words(const view_state& v) {
  if (v.loading || !choosing(v)) return "";
  if (v.rows && !v.rows->empty()) return "";
  if (!v.query.empty()) return "Nothing matches that";
  if (v.from == source::folder && !v.has_folder) return "Open a folder in the viewer to choose from it";
  return "Nothing here to choose from";
}

std::string export_words(const view_state& v) { return v.exporting ? "Cancel" : "Export"; }

// Progress is said in the status line, in place: nothing is laid out for it,
// so the gallery stays put while an export runs.
std::string note_words(const view_state& v) {
  if (!v.exporting) return v.note;
  return "Exporting " + v.progress_words + " · " + std::to_string(static_cast<int>(v.progress * 100.0 + 0.5)) + "%";
}

bool nothing_chosen(const view_state& v) { return v.p->items().empty(); }

void set_props(writer& w, std::string_view id) {
  w.begin_object().key("op").string("set").key("id").string(id).key("props");
}

const chosen* focused(const view_state& v) {
  const post& p = *v.p;
  const std::size_t i = p.index_of(p.focus());
  return i < p.items().size() ? &p.items()[i] : nullptr;
}

void heading(writer& w, std::string_view id, std::string_view words) { text(w, id, words, "heading"); }

void adjust_tools(writer& w, const chosen& c) {
  w.begin_object().key("type").string("stack").key("id").string("adj_top").key("axis").string("horizontal");
  w.key("spacing").integer(8).key("children").begin_array();
  button(w, "auto", "Auto", "", false);
  button(w, "reset", "Reset", "", !adjusted(c.adjust));
  w.end_array().end_object();
  static constexpr std::array<std::string_view, 3> kTitles{"Light", "Colour", "Finish"};
  int section = -1;
  for (const slider_info& s : sliders()) {
    if (s.section != section) {
      section = s.section;
      heading(w, "h" + std::to_string(section), kTitles[static_cast<std::size_t>(section)]);
    }
    w.begin_object().key("type").string("slider").key("id").string("a." + std::to_string(s.wire));
    w.key("label").string(s.label).key("min").number(s.min).key("max").number(s.max).key("step").number(s.step);
    const double shown = c.adjust[s.wire] / s.scale;
    w.key("value").number(shown).key("live").boolean(true).end_object();
  }
}

void panel_node(writer& w, const view_state& v) {
  const chosen* c = focused(v);
  const bool usable = c && !c->video;
  w.begin_object().key("type").string("split").key("id").string("panel").key("axis").string("horizontal");
  w.key("fraction").number(0.4).key("fill").boolean(true);
  if (choosing(v)) w.key("hidden").boolean(true);
  w.key("children").begin_array();

  w.begin_object().key("type").string("stack").key("id").string("stage_box").key("axis").string("vertical");
  w.key("spacing").integer(8).key("align").string("center").key("children").begin_array();
  if (usable) {
    const tile_size size = stage_of(v.p->current());
    w.begin_object().key("type").string("surface").key("id").string(kStageNode).key("surface").string(kStageSurface);
    w.key("width").integer(size.w).key("height").integer(size.h).key("interactive").boolean(true);
    w.key("label").string("Preview of " + c->name).end_object();
  } else {
    text(w, "stage_note", c ? "These tools are for photos" : "Choose a photo in the strip above", "secondary");
  }
  w.end_array().end_object();

  w.begin_object().key("type").string("scroll").key("id").string("side").key("axis").string("vertical");
  w.key("children").begin_array();
  w.begin_object().key("type").string("stack").key("id").string("side_body").key("axis").string("vertical");
  w.key("spacing").integer(10).key("align").string("fill").key("children").begin_array();
  if (usable && v.mode == tool_mode::adjust) adjust_tools(w, *c);
  if (usable && v.mode == tool_mode::enhance) {
    if (v.enhance_missing) {
      text(w, "enh_note", "Add or update Enhance in Settings, Add-ons, to use it here", "secondary");
    } else {
      w.begin_object().key("type").string("embed").key("id").string("enh");
      w.key("addon").string("enhance").key("screen").string("panel").end_object();
    }
  }
  w.end_array().end_object();
  w.end_array().end_object();

  w.end_array().end_object();
}

void move_buttons(writer& w, const view_state& v) {
  const post& p = *v.p;
  const std::size_t i = p.index_of(p.focus());
  const bool none = p.focus() == 0;
  button(w, "earlier", "Move earlier", "", none || i == 0);
  button(w, "later", "Move later", "", none || i + 1 >= p.items().size());
  button(w, "remove", "Remove", "", none);
}

void set_disabled(writer& w, std::string_view id, bool disabled) {
  set_props(w, id);
  w.begin_object().key("disabled");
  if (disabled) {
    w.boolean(true);
  } else {
    w.null();
  }
  w.end_object().end_object();
}

}  // namespace

tile_size stage_of(shape s) noexcept {
  const double aspect = aspect_of(s);
  double h = 340;
  double w = h * aspect;
  if (w > 420) {
    w = 420;
    h = w / aspect;
  }
  return {static_cast<std::int32_t>(w + 0.5), static_cast<std::int32_t>(h + 0.5)};
}

std::string surface_id(std::uint64_t uid) { return "t" + std::to_string(uid); }
std::string tile_node_id(std::uint64_t uid) { return "tile" + std::to_string(uid); }

std::uint64_t uid_of_tile_node(std::string_view node) noexcept {
  if (node.substr(0, 4) != "tile") return 0;
  std::uint64_t uid = 0;
  const char* end = node.data() + node.size();
  const auto [p, ec] = std::from_chars(node.data() + 4, end, uid);
  return ec == std::errc{} && p == end ? uid : 0;
}

std::string tree_json(const view_state& v) {
  writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("axis").string("vertical");
  w.key("spacing").integer(12).key("align").string("fill").key("children").begin_array();

  // The shape, set apart from what the post amounts to.
  w.begin_object().key("type").string("stack").key("id").string("bar").key("axis").string("horizontal");
  w.key("spacing").integer(12).key("justify").string("between").key("children").begin_array();
  {
    w.begin_object().key("type").string("choice").key("id").string("shape").key("label").string("Shape");
    w.key("look").string("segmented").key("value").string(info_of(v.p->current()).id);
    w.key("options").begin_array();
    for (const shape_info& s : shapes()) {
      w.begin_object().key("id").string(s.id).key("text").string(s.label).end_object();
    }
    w.end_array().end_object();

    w.begin_object().key("type").string("stack").key("id").string("act").key("axis").string("horizontal");
    w.key("spacing").integer(12).key("children").begin_array();
    text(w, "count", count_words(*v.p), "secondary");
    button(w, "export", export_words(v), "primary", !v.exporting && nothing_chosen(v));
    w.end_array().end_object();
  }
  w.end_array().end_object();

  // The carousel.
  w.begin_object().key("type").string("scroll").key("id").string("strip_scroll").key("axis").string("horizontal");
  w.key("height").integer(224).key("children").begin_array();
  strip_node(w, v);
  w.end_array().end_object();

  // What the focused tile can do, and what the last export did. Always laid
  // out, so the gallery never jumps.
  w.begin_object().key("type").string("stack").key("id").string("tools").key("axis").string("horizontal");
  w.key("spacing").integer(8).key("justify").string("between").key("children").begin_array();
  {
    w.begin_object().key("type").string("stack").key("id").string("moves").key("axis").string("horizontal");
    w.key("spacing").integer(8).key("children").begin_array();
    move_buttons(w, v);
    w.end_array().end_object();

    w.begin_object().key("type").string("stack").key("id").string("after").key("axis").string("horizontal");
    w.key("spacing").integer(8).key("children").begin_array();
    text(w, "note", note_words(v), "secondary");
    button(w, "show", "Show", "", false, !v.can_show);
    w.end_array().end_object();
  }
  w.end_array().end_object();

  w.begin_object().key("type").string("separator").key("id").string("rule").end_object();

  // Where the pictures come from.
  w.begin_object().key("type").string("stack").key("id").string("find").key("axis").string("horizontal");
  w.key("spacing").integer(12).key("children").begin_array();
  {
    w.begin_object().key("type").string("choice").key("id").string("mode").key("label").string("Tools");
    w.key("look").string("segmented").key("value").string(v.mode == tool_mode::adjust    ? "adjust"
                                                          : v.mode == tool_mode::enhance ? "enhance"
                                                                                         : "choose");
    w.key("options").begin_array();
    w.begin_object().key("id").string("choose").key("text").string("Choose").end_object();
    w.begin_object().key("id").string("adjust").key("text").string("Adjust").end_object();
    w.begin_object().key("id").string("enhance").key("text").string("Enhance").end_object();
    w.end_array().end_object();

    w.begin_object().key("type").string("choice").key("id").string("source").key("label").string("Choose from");
    w.key("look").string("segmented").key("value").string(v.from == source::library ? "library" : "folder");
    if (source_hidden(v)) w.key("hidden").boolean(true);
    w.key("options").begin_array();
    w.begin_object().key("id").string("library").key("text").string("Photos").end_object();
    w.begin_object().key("id").string("folder").key("text").string("This folder").end_object();
    w.end_array().end_object();

    w.begin_object().key("type").string("text_field").key("id").string("search").key("label").string("Search");
    w.key("placeholder").string("Search").key("value").string(v.query);
    if (!choosing(v)) w.key("hidden").boolean(true);
    w.end_object();

    button(w, "more", more_words(v), "", false, more_hidden(v));
  }
  w.end_array().end_object();

  text(w, "empty", empty_words(v), "secondary", empty_words(v).empty());

  w.begin_object().key("type").string("thumbnail_grid").key("id").string("grid").key("label").string("Your pictures");
  w.key("tile").integer(112).key("multi").boolean(true).key("numbered").boolean(true).key("fill").boolean(true);
  if (!choosing(v)) w.key("hidden").boolean(true);
  grid_rows(w, v);
  selection(w, v);
  w.end_object();

  panel_node(w, v);

  w.end_array().end_object();
  return w.take();
}

std::string strip_diff(const view_state& v) {
  writer w;
  w.begin_array();
  w.begin_object().key("op").string("replace").key("id").string("strip").key("node");
  strip_node(w, v);
  w.end_object();
  set_props(w, "count");
  w.begin_object().key("text").string(count_words(*v.p)).end_object().end_object();
  set_disabled(w, "export", !v.exporting && nothing_chosen(v));
  const post& p = *v.p;
  const std::size_t i = p.index_of(p.focus());
  const bool none = p.focus() == 0;
  set_disabled(w, "earlier", none || i == 0);
  set_disabled(w, "later", none || i + 1 >= p.items().size());
  set_disabled(w, "remove", none);
  set_props(w, "grid");
  w.begin_object();
  selection(w, v);
  w.end_object().end_object();
  w.end_array();
  return w.take();
}

std::string gallery_diff(const view_state& v) {
  writer w;
  w.begin_array();
  set_props(w, "grid");
  w.begin_object();
  grid_rows(w, v);
  selection(w, v);
  w.end_object().end_object();
  set_props(w, "more");
  w.begin_object().key("label").string(more_words(v));
  w.key("hidden").boolean(more_hidden(v)).end_object().end_object();
  set_props(w, "empty");
  w.begin_object().key("text").string(empty_words(v)).key("hidden").boolean(empty_words(v).empty());
  w.end_object().end_object();
  set_props(w, "source");
  w.begin_object().key("value").string(v.from == source::library ? "library" : "folder");
  w.key("hidden").boolean(source_hidden(v)).end_object().end_object();
  w.end_array();
  return w.take();
}

std::string reset_diff(bool disabled) {
  writer w;
  w.begin_array();
  set_disabled(w, "reset", disabled);
  w.end_array();
  return w.take();
}

std::string panel_diff(const view_state& v) {
  writer w;
  w.begin_array();
  w.begin_object().key("op").string("replace").key("id").string("panel").key("node");
  panel_node(w, v);
  w.end_object();
  set_props(w, "grid");
  w.begin_object().key("hidden").boolean(!choosing(v)).end_object().end_object();
  set_props(w, "search");
  w.begin_object().key("hidden").boolean(!choosing(v)).end_object().end_object();
  set_props(w, "source");
  w.begin_object().key("hidden").boolean(source_hidden(v)).end_object().end_object();
  set_props(w, "more");
  w.begin_object().key("hidden").boolean(more_hidden(v)).end_object().end_object();
  set_props(w, "empty");
  w.begin_object().key("text").string(empty_words(v)).key("hidden").boolean(empty_words(v).empty());
  w.end_object().end_object();
  w.end_array();
  return w.take();
}

std::string progress_diff(const view_state& v) {
  writer w;
  w.begin_array();
  set_props(w, "export");
  w.begin_object().key("label").string(export_words(v));
  w.key("disabled").boolean(!v.exporting && nothing_chosen(v)).end_object().end_object();
  set_props(w, "note");
  w.begin_object().key("text").string(note_words(v)).end_object().end_object();
  set_props(w, "show");
  w.begin_object().key("hidden").boolean(!v.can_show).end_object().end_object();
  w.end_array();
  return w.take();
}

}  // namespace mv::instagram
