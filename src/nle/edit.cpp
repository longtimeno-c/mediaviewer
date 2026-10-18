// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "core/json.h"

namespace mv::nle {
namespace {

using xml::kNone;

bool is_story(std::string_view n) {
  static constexpr std::string_view kStory[] = {"asset-clip", "clip",    "ref-clip", "mc-clip", "sync-clip",
                                                "title",      "gap",     "audio",    "video",   "audition",
                                                "caption",    "live-drawing", "spine"};
  return std::find(std::begin(kStory), std::end(kStory), n) != std::end(kStory);
}

bool is_markerish(std::string_view n) {
  return n == "marker" || n == "chapter-marker" || n == "rating" || n == "keyword" || n == "analysis-marker" ||
         n == "hidden-clip-marker";
}

// What comes after a clip's anchored items and markers (FCPXML's content order).
bool after_markers(std::string_view n) {
  return n == "audio-channel-source" || n == "audio-role-source" || n == "sync-source" || n == "filter-video" ||
         n == "filter-video-mask" || n == "filter-audio" || n == "metadata" || n == "reserved";
}

bool container(item_kind k) {
  return k == item_kind::clip || k == item_kind::sync_clip || k == item_kind::audition;
}

rational P(rational a, rational b) { return add(a, b).value_or(a); }
rational M(rational a, rational b) { return sub(a, b).value_or(a); }
rational rmax(rational a, rational b) { return a < b ? b : a; }
rational rmin(rational a, rational b) { return a < b ? a : b; }

std::optional<rational> time_attr(const xml::document& d, std::uint32_t n, std::string_view a) {
  const std::string* v = d.attr(n, a);
  if (!v) return std::nullopt;
  return parse_time(*v);
}

// Sets a time attribute only when its value changes (so "100/2500s" is not
// rewritten as "1/25s").
void set_time(xml::document& d, std::uint32_t n, std::string_view a, rational v) {
  if (const auto old = time_attr(d, n, a); old && *old == v) return;
  d.set_attr(n, a, format_time(v));
}

std::string file_stem(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
  const std::size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0) name.erase(dot);
  return name;
}

std::string seconds_text(rational t) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.2f s", seconds(t));
  return buf;
}

// A connected item that plays on through a cut (a music bed) rather than
// being cut with the picture.
bool plays_through(const item& c, const timeline& t) {
  if (c.role.rfind("music", 0) == 0 || c.role.find(", music") != std::string::npos) return true;
  if (c.lane < 0 && (c.kind == item_kind::asset_clip || c.kind == item_kind::audio)) {
    const auto a = t.assets().find(c.ref);
    if (a != t.assets().end() && !a->second.has_video && a->second.has_audio) return true;
  }
  return false;
}

}  // namespace

// ---- time and clock text ------------------------------------------------------------------

std::optional<rational> parse_clock(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  if (s.find('/') != std::string_view::npos || (s.back() == 's' && s.find(':') == std::string_view::npos)) {
    return parse_time(s);
  }
  bool negative = false;
  if (s.front() == '-') {
    negative = true;
    s.remove_prefix(1);
  }
  // h:mm:ss.fff, m:ss.fff or ss.fff
  std::vector<std::string_view> parts;
  std::size_t from = 0;
  for (;;) {
    const std::size_t colon = s.find(':', from);
    parts.push_back(s.substr(from, colon == std::string_view::npos ? std::string_view::npos : colon - from));
    if (colon == std::string_view::npos) break;
    from = colon + 1;
  }
  if (parts.size() > 3) return std::nullopt;
  rational total{};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1 == parts.size();
    std::optional<rational> v = last ? parse_time(parts[i]) : parse_time(std::string(parts[i]) + "s");
    if (!v || v->num < 0) return std::nullopt;
    if (!last && v->den != 1) return std::nullopt;
    const std::int64_t scale = parts.size() - 1 - i == 2 ? 3600 : (parts.size() - 1 - i == 1 ? 60 : 1);
    const auto scaled = reduce({v->num * scale, v->den});
    total = P(total, scaled);
  }
  if (negative) total.num = -total.num;
  return total;
}

std::string format_clock(rational t) {
  const bool negative = t.num < 0;
  double s = std::fabs(seconds(t));
  long long cs = static_cast<long long>(std::llround(s * 100.0));
  const long long h = cs / 360000;
  cs %= 360000;
  const long long m = cs / 6000;
  cs %= 6000;
  char buf[48];
  if (h > 0) {
    std::snprintf(buf, sizeof buf, "%s%lld:%02lld:%02lld.%02lld", negative ? "-" : "", h, m, cs / 100, cs % 100);
  } else {
    std::snprintf(buf, sizeof buf, "%s%lld:%02lld.%02lld", negative ? "-" : "", m, cs / 100, cs % 100);
  }
  return buf;
}

std::string editor::clock(rational t) const { return format_clock(t); }

// ---- parsing a batch ----------------------------------------------------------------------

namespace {

std::optional<rational> json_time(const json::value& v) {
  if (v.k == json::kind::string) return parse_clock(v.s);
  if (v.k == json::kind::number) {
    if (v.is_integer) return rational{v.i, 1};
    // A fractional number of seconds: to the millisecond.
    return reduce({static_cast<std::int64_t>(std::llround(v.d * 1000.0)), 1000});
  }
  return std::nullopt;
}

}  // namespace

bool parse_ops(std::string_view json_array, std::vector<edit_op>& out, std::string* error) {
  out.clear();
  const auto doc = json::parse(json_array, 16);
  if (!doc || doc->k != json::kind::array) {
    if (error) *error = "ops must be a JSON array of operations";
    return false;
  }
  if (doc->a.size() > 2000) {
    if (error) *error = "at most 2000 operations in one batch";
    return false;
  }
  for (std::size_t i = 0; i < doc->a.size(); ++i) {
    const json::value& o = doc->a[i];
    auto fail = [&](const std::string& why) {
      if (error) *error = "op " + std::to_string(i) + ": " + why;
      return false;
    };
    if (o.k != json::kind::object) return fail("not an object");
    const std::string* name = o.str("op");
    if (!name) return fail("no \"op\"");
    edit_op op;
    static const std::pair<std::string_view, edit_op::kind> kKinds[] = {
        {"cut", edit_op::kind::cut},         {"trim", edit_op::kind::trim},
        {"split", edit_op::kind::split},     {"remove", edit_op::kind::remove},
        {"move", edit_op::kind::move},       {"source", edit_op::kind::source},
        {"insert", edit_op::kind::insert},   {"connect", edit_op::kind::connect},
        {"marker", edit_op::kind::marker},   {"unmarker", edit_op::kind::unmarker},
        {"title", edit_op::kind::title},     {"volume", edit_op::kind::volume},
        {"enable", edit_op::kind::enable},
    };
    bool known = false;
    for (const auto& [n, k] : kKinds) {
      if (*name == n) {
        op.k = k;
        known = true;
      }
    }
    if (!known) return fail("unknown op \"" + *name + "\"");
    auto time_field = [&](std::string_view key, rational& dst, bool* has) -> bool {
      const json::value* v = o.find(key);
      if (!v) return true;
      const auto t = json_time(*v);
      if (!t) return false;
      dst = *t;
      if (has) *has = true;
      return true;
    };
    bool has_at = false, has_dur = false, has_src = false;
    if (!time_field("from", op.from, &op.has_from) || !time_field("to", op.to, &op.has_to) ||
        !time_field("at", op.at, &has_at) || !time_field("head", op.head, &op.has_head) ||
        !time_field("tail", op.tail, &op.has_tail) || !time_field("duration", op.duration, &has_dur) ||
        !time_field("source_in", op.source_in, &has_src)) {
      return fail("a time is not readable (use seconds, \"m:ss.ff\" or FCPXML \"n/ds\")");
    }
    if (const auto v = o.integer("item")) op.item = static_cast<int>(*v);
    if (const auto v = o.integer("before")) op.before = static_cast<int>(*v);
    if (const auto v = o.integer("lane")) op.lane = static_cast<int>(*v);
    if (const auto v = o.integer("marker")) op.marker = static_cast<int>(*v);
    if (const auto v = o.boolean("ripple")) op.ripple = *v;
    if (const auto v = o.boolean("with_audio")) op.with_audio = *v;
    if (const auto v = o.boolean("todo")) op.todo = *v;
    if (const auto v = o.boolean("on")) op.on = *v;
    if (const std::string* v = o.str("path")) op.path = *v;
    if (const std::string* v = o.str("text")) op.text = *v;
    if (const json::value* v = o.find("db"); v && v->k == json::kind::number) {
      op.db = v->is_integer ? static_cast<double>(v->i) : v->d;
    }
    using K = edit_op::kind;
    const bool needs_item = op.k == K::trim || op.k == K::split || op.k == K::remove || op.k == K::move ||
                            op.k == K::source || op.k == K::unmarker || op.k == K::title || op.k == K::volume ||
                            op.k == K::enable;
    if (needs_item && op.item < 0) return fail("needs \"item\" (an id from get_timeline)");
    if (op.k == K::cut && (!op.has_from || !op.has_to)) return fail("cut needs \"from\" and \"to\"");
    if ((op.k == K::split || op.k == K::marker || op.k == K::connect) && !has_at) return fail("needs \"at\"");
    if ((op.k == K::insert || op.k == K::connect) && (op.path.empty() || !has_dur || !has_src)) {
      return fail("needs \"path\", \"source_in\" and \"duration\"");
    }
    if (op.k == K::source && !has_src) return fail("source needs \"source_in\"");
    if (op.k == K::trim && !op.has_from && !op.has_to && !op.has_head && !op.has_tail) {
      return fail("trim needs \"from\"/\"to\" or \"head\"/\"tail\"");
    }
    if (op.k == K::marker && op.text.empty()) return fail("marker needs \"text\"");
    if (op.k == K::unmarker && op.marker < 0) return fail("unmarker needs \"marker\" (its index on the item)");
    if (op.k == K::volume && !o.find("db")) return fail("volume needs \"db\"");
    if (op.k == K::volume && (op.db < -96.0 || op.db > 12.0)) return fail("volume must be between -96 and +12 dB");
    out.push_back(std::move(op));
  }
  return true;
}

// ---- helpers over the tree ----------------------------------------------------------------

rational editor::map_time(rational t) const {
  for (const map_step& s : steps_) {
    if (s.remove) {
      if (t < s.a) continue;
      t = t < s.b ? s.a : M(t, M(s.b, s.a));
    } else if (t >= s.a) {
      t = P(t, s.len);
    }
  }
  return t;
}

const item* editor::item_of(std::uint32_t node) const {
  for (const item& it : t_.items()) {
    if (it.node == node) return &it;
  }
  return nullptr;
}

std::vector<int> editor::storyline() const {
  std::vector<int> out;
  for (std::size_t i = 0; i < t_.items().size(); ++i) {
    const item& it = t_.items()[i];
    if (it.parent == -1 && it.in_storyline) out.push_back(static_cast<int>(i));
  }
  return out;
}

int editor::storyline_at(rational t) const {
  int last = -1;
  for (const int i : storyline()) {
    const item& it = t_.items()[static_cast<std::size_t>(i)];
    if (it.kind == item_kind::transition) continue;
    last = i;
    if (it.at <= t && t < P(it.at, it.duration)) return i;
  }
  // The very end of the timeline belongs to the last item.
  if (last >= 0 && t == t_.length()) return last;
  return -1;
}

rational editor::snap(rational t) const {
  const rational f = t_.project().frame;
  if (f.num <= 0) return t;
  return nle::snap(t, f).value_or(t);
}

namespace {

// An element's duration as the storyline lays it out: an audition plays its
// first pick.
std::optional<rational> laid_duration(const xml::document& d, std::uint32_t n) {
  if (d.at(n).name == "audition") {
    for (const std::uint32_t c : d.elements(n)) return time_attr(d, c, "duration");
    return std::nullopt;
  }
  if (const auto v = time_attr(d, n, "duration")) return v;
  return std::nullopt;
}

}  // namespace

void editor::relay() {
  xml::document& d = t_.doc();
  const project_info& p = t_.project();
  rational pos = p.tc_start;
  for (const std::uint32_t c : d.elements(p.spine)) {
    if (d.at(c).name == "transition") {
      // Centred on the edit point it sits at.
      const rational dur = time_attr(d, c, "duration").value_or(rational{});
      set_time(d, c, "offset", M(pos, reduce({dur.num, dur.den * 2})));
      continue;
    }
    set_time(d, c, "offset", pos);
    pos = P(pos, laid_duration(d, c).value_or(rational{}));
  }
  set_time(d, p.sequence, "duration", M(pos, p.tc_start));
  t_.reindex();
}

void editor::insert_anchor(std::uint32_t host, std::uint32_t child) {
  xml::document& d = t_.doc();
  std::uint32_t before = kNone;
  for (const std::uint32_t c : d.elements(host)) {
    const std::string& n = d.at(c).name;
    if (is_markerish(n) || after_markers(n)) {
      before = c;
      break;
    }
  }
  d.insert(host, child, before);
}

void editor::insert_marker_node(std::uint32_t host, std::uint32_t marker) {
  xml::document& d = t_.doc();
  std::uint32_t before = kNone;
  for (const std::uint32_t c : d.elements(host)) {
    if (after_markers(d.at(c).name)) {
      before = c;
      break;
    }
  }
  d.insert(host, marker, before);
}

void editor::mark(rational at, const std::string& text) {
  if (!o_.mark_changes) return;
  int host = storyline_at(at);
  if (host < 0) return;
  const item& h = t_.items()[static_cast<std::size_t>(host)];
  if (h.kind == item_kind::other || h.kind == item_kind::audition) return;
  rational local_at = at;
  if (local_at >= P(h.at, h.duration)) local_at = M(P(h.at, h.duration), t_.project().frame.num > 0 ? t_.project().frame : rational{1, 1000});
  xml::document& d = t_.doc();
  const std::uint32_t m = d.make_element("marker");
  d.set_attr(m, "start", format_time(P(h.start, M(local_at, h.at))));
  d.set_attr(m, "duration", format_time(t_.project().frame.num > 0 ? t_.project().frame : rational{1, 1000}));
  d.set_attr(m, "value", "Claude: " + text);
  insert_marker_node(h.node, m);
  t_.reindex();
}

std::string editor::ensure_asset(const std::string& path, const media_probe& probe) {
  for (const auto& [id, a] : t_.assets()) {
    if (a.path == path) return id;
  }
  xml::document& d = t_.doc();
  std::uint32_t res = d.first(d.root(), "resources");
  if (res == kNone) {
    res = d.make_element("resources");
    d.insert(d.root(), res, d.elements(d.root()).empty() ? kNone : d.elements(d.root()).front());
  }
  std::set<std::string> ids;
  int top = 0;
  for (const std::uint32_t r : d.elements(res)) {
    if (const std::string* id = d.attr(r, "id")) {
      ids.insert(*id);
      if (id->size() > 1 && (*id)[0] == 'r') {
        int v = 0;
        bool digits = true;
        for (std::size_t i = 1; i < id->size(); ++i) {
          if ((*id)[i] < '0' || (*id)[i] > '9') digits = false;
          else v = v * 10 + ((*id)[i] - '0');
        }
        if (digits) top = std::max(top, v);
      }
    }
  }
  auto fresh = [&] {
    std::string id;
    do {
      id = "r" + std::to_string(++top);
    } while (ids.count(id));
    ids.insert(id);
    return id;
  };
  std::string format;
  if (probe.has_video && probe.width > 0 && probe.height > 0) {
    format = fresh();
    const std::uint32_t f = d.make_element("format");
    d.set_attr(f, "id", format);
    if (probe.frame.num > 0) d.set_attr(f, "frameDuration", format_time(probe.frame));
    d.set_attr(f, "width", std::to_string(probe.width));
    d.set_attr(f, "height", std::to_string(probe.height));
    d.insert(res, f);
  }
  const std::string id = fresh();
  const std::uint32_t a = d.make_element("asset");
  d.set_attr(a, "id", id);
  d.set_attr(a, "name", file_stem(path));
  d.set_attr(a, "start", "0s");
  d.set_attr(a, "duration", format_time(probe.duration));
  if (probe.has_video) {
    d.set_attr(a, "hasVideo", "1");
    if (!format.empty()) d.set_attr(a, "format", format);
  }
  if (probe.has_audio) {
    d.set_attr(a, "hasAudio", "1");
    d.set_attr(a, "audioSources", "1");
    if (probe.audio_channels > 0) d.set_attr(a, "audioChannels", std::to_string(probe.audio_channels));
    if (probe.audio_rate > 0) d.set_attr(a, "audioRate", std::to_string(probe.audio_rate));
  }
  const std::uint32_t rep = d.make_element("media-rep");
  d.set_attr(rep, "kind", "original-media");
  d.set_attr(rep, "src", file_url(path));
  d.insert(a, rep);
  d.insert(res, a);
  t_.reindex();
  return id;
}

// ---- the batch ----------------------------------------------------------------------------

bool editor::apply(const std::vector<edit_op>& ops, const std::vector<std::pair<std::string, media_probe>>& probes,
                   std::string* error, std::size_t* failed_op) {
  using K = edit_op::kind;
  bool has_move = false, has_time = false;
  for (const edit_op& op : ops) {
    has_move = has_move || op.k == K::move;
    has_time = has_time || op.k == K::cut || op.k == K::split || op.k == K::connect || op.k == K::marker ||
               (op.k == K::trim && (op.has_from || op.has_to));
  }
  if (has_move && has_time) {
    if (error) *error = "a move reorders the storyline: send time-addressed edits (cut, trim from/to, split, "
                        "connect, marker) in a batch of their own, after get_timeline";
    if (failed_op) *failed_op = 0;
    return false;
  }
  // Ids name the timeline as it is now, before any op of the batch.
  std::vector<std::uint32_t> nodes;
  for (const item& it : t_.items()) nodes.push_back(it.node);
  auto node_of = [&](int id) -> std::uint32_t {
    return id >= 0 && static_cast<std::size_t>(id) < nodes.size() ? nodes[static_cast<std::size_t>(id)] : kNone;
  };
  auto probe_of = [&](const std::string& path) -> const media_probe* {
    for (const auto& [p, m] : probes) {
      if (p == path) return &m;
    }
    return nullptr;
  };
  const xml::document snapshot = t_.doc();
  const std::size_t changes_before = changes_.size();
  steps_.clear();
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const edit_op& op = ops[i];
    std::string why;
    bool ok = false;
    const std::uint32_t node = node_of(op.item);
    if (op.item >= 0 && node == kNone) {
      why = "no item " + std::to_string(op.item);
    } else if (op.item >= 0 && !item_of(node)) {
      why = "item " + std::to_string(op.item) + " was removed earlier in this batch";
    } else {
      switch (op.k) {
        case K::cut: ok = op_cut(map_time(op.from), map_time(op.to), op.ripple, &why); break;
        case K::trim: ok = op_trim(node, op, &why); break;
        case K::split: ok = op_split(node, map_time(op.at), &why); break;
        case K::remove: ok = op_remove(node, op.ripple, &why); break;
        case K::move: ok = op_move(node, node_of(op.before), &why); break;
        case K::source: ok = op_source(node, op, op.path.empty() ? nullptr : probe_of(op.path), &why); break;
        case K::insert:
          if (const media_probe* m = probe_of(op.path)) {
            ok = op_insert(op, node_of(op.before), *m, &why);
          } else {
            why = "the file could not be read";
          }
          break;
        case K::connect:
          if (const media_probe* m = probe_of(op.path)) {
            edit_op mapped = op;
            mapped.at = map_time(op.at);
            ok = op_connect(mapped, *m, &why);
          } else {
            why = "the file could not be read";
          }
          break;
        case K::marker: ok = op_marker(map_time(op.at), op.text, op.todo, &why); break;
        case K::unmarker: ok = op_unmarker(node, op.marker, &why); break;
        case K::title: ok = op_title(node, op.text, &why); break;
        case K::volume: ok = op_volume(node, op.db, &why); break;
        case K::enable: ok = op_enable(node, op.on, &why); break;
      }
    }
    if (!ok) {
      t_.doc() = snapshot;
      t_.reindex();
      changes_.resize(changes_before);
      steps_.clear();
      if (error) *error = "op " + std::to_string(i) + ": " + why + " (nothing in this batch was applied)";
      if (failed_op) *failed_op = i;
      return false;
    }
  }
  steps_.clear();
  return true;
}

// ---- cut ----------------------------------------------------------------------------------

bool editor::op_cut(rational from, rational to, bool ripple, std::string* error, std::string_view why) {
  xml::document& d = t_.doc();
  const rational a = snap(from), b0 = snap(to);
  const rational length = t_.length();
  const rational b = rmin(b0, length);
  if (a < rational{} || a >= length) {
    *error = "the range starts outside the timeline (0:00.00 to " + clock(length) + ")";
    return false;
  }
  if (b <= a) {
    *error = "the range is empty on the frame grid";
    return false;
  }
  // What the cut touches on the primary storyline.
  struct target {
    std::uint32_t node;
    rational at, end, start;
    item_kind kind;
  };
  std::vector<target> hits;
  std::vector<std::uint32_t> transitions;
  for (const int i : storyline()) {
    const item& it = t_.items()[static_cast<std::size_t>(i)];
    const rational end = P(it.at, it.duration);
    if (it.kind == item_kind::transition) {
      // Touching an edge counts: the edit point it sits on goes or moves.
      if (it.at <= b && end >= a) transitions.push_back(it.node);
      continue;
    }
    if (!(it.at < b && end > a)) continue;
    const bool whole = a <= it.at && end <= b;
    if (!whole && !it.editable) {
      *error = "the range cuts into \"" + it.name + "\", which MediaViewer does not edit (" + it.not_editable + ")";
      return false;
    }
    if (!whole && it.kind == item_kind::audition) {
      *error = "the range cuts into an audition: pick its clip in Final Cut Pro first";
      return false;
    }
    hits.push_back({it.node, it.at, end, it.start, it.kind});
  }
  // A connected storyline the range crosses.
  for (const item& it : t_.items()) {
    if (it.kind != item_kind::storyline) continue;
    const rational end = P(it.at, it.duration.num ? it.duration : rational{});
    rational s_end = it.at;
    for (const item& c : t_.items()) {
      if (c.storyline >= 0 && t_.items()[static_cast<std::size_t>(c.storyline)].node == it.node) {
        s_end = rmax(s_end, P(c.at, c.duration));
      }
    }
    (void)end;
    if (it.at < b && s_end > a) {
      *error = "a connected storyline crosses " + clock(a) + "-" + clock(b) +
               ": break it apart in Final Cut Pro, or edit around it";
      return false;
    }
  }
  if (hits.empty()) {
    *error = "nothing on the storyline there";
    return false;
  }
  for (const std::uint32_t tr : transitions) d.remove(tr);

  // The anchored children of every element the cut touches, with where they
  // are on the timeline now.
  struct child {
    std::uint32_t node;
    std::size_t index;  // among the host's element children (for its clone)
    rational at, dur, start, offset;
    bool music;
    bool marker;
  };
  struct rehost {
    std::uint32_t node;
    rational at;  // where it is on the timeline before the cut
  };
  std::vector<rehost> moves;
  const rational frame = t_.project().frame.num > 0 ? t_.project().frame : rational{1, 1000};

  for (const target& e_ : hits) {
    const std::uint32_t E = e_.node;
    const rational s = rmax(a, e_.at), e = rmin(b, e_.end);
    const item* host = item_of(E);
    if (!host) continue;
    std::vector<child> kids;
    if (!container(host->kind)) {
      const auto els = d.elements(E);
      for (std::size_t k = 0; k < els.size(); ++k) {
        const std::uint32_t c = els[k];
        const std::string& n = d.at(c).name;
        if (n == "marker" || n == "chapter-marker") {
          const rational ms = time_attr(d, c, "start").value_or(rational{});
          kids.push_back({c, k, P(e_.at, M(ms, e_.start)), {}, ms, ms, false, true});
          continue;
        }
        if (!is_story(n)) continue;
        const item* ci = item_of(c);
        if (!ci) continue;
        kids.push_back({c, k, ci->at, ci->duration, ci->start, ci->offset, plays_through(*ci, t_), false});
      }
    }
    const bool whole = s == e_.at && e == e_.end;
    const bool head = s == e_.at && !whole;
    const bool tail = e == e_.end && !whole;
    std::uint32_t E2 = kNone;
    if (!head && !tail && !whole) {
      E2 = d.clone(E);
      // After E in the storyline.
      const auto siblings = d.elements(t_.project().spine);
      std::uint32_t next = kNone;
      for (std::size_t k = 0; k + 1 < siblings.size(); ++k) {
        if (siblings[k] == E) next = siblings[k + 1];
      }
      d.insert(t_.project().spine, E2, next);
    }
    const auto e2_els = E2 == kNone ? std::vector<std::uint32_t>{} : d.elements(E2);
    auto twin = [&](const child& c) { return c.index < e2_els.size() ? e2_els[c.index] : kNone; };

    // The children.
    for (const child& c : kids) {
      const rational cend = P(c.at, c.dur);
      const std::uint32_t cin2 = twin(c);
      if (!ripple) {
        // A lift leaves everything where it is in time: what sat over the
        // lifted part goes onto the gap.
        if (c.at >= s && c.at < e && !whole && !(head && c.at >= e)) {
          if (cin2 != kNone) d.remove(cin2);
          d.remove(c.node);
          moves.push_back({c.node, c.at});
        } else if (whole) {
          d.remove(c.node);
          moves.push_back({c.node, c.at});
        } else if (tail && c.at >= s) {
          d.remove(c.node);
          moves.push_back({c.node, c.at});
        } else if (E2 != kNone) {
          // Before s: stays in E; at or after e: goes with E2.
          if (c.at < s) {
            if (cin2 != kNone) d.remove(cin2);
          } else {
            d.remove(c.node);
          }
        }
        continue;
      }
      if (c.marker) {
        const bool in_cut = c.at >= s && c.at < e;
        if (in_cut) {
          d.remove(c.node);
          if (cin2 != kNone) d.remove(cin2);
        } else if (E2 != kNone) {
          if (c.at < s) {
            if (cin2 != kNone) d.remove(cin2);
          } else {
            d.remove(c.node);
          }
        } else if (whole) {
          d.remove(c.node);  // its clip is gone
        }
        continue;
      }
      if (c.at < s) {
        // Starts before the cut.
        if (cend <= s) {
          if (cin2 != kNone) d.remove(cin2);
          continue;  // ends before it: untouched
        }
        if (c.music && o_.music_plays_through) {
          // Plays on, shorter by what was cut under it.
          set_time(d, c.node, "duration", M(c.dur, M(rmin(cend, e), s)));
          if (cin2 != kNone) d.remove(cin2);
          continue;
        }
        // Cut with the picture: the part before s stays; the part after e, if
        // any, carries on after the cut.
        set_time(d, c.node, "duration", M(s, c.at));
        if (cend > e) {
          const std::uint32_t rest = cin2 != kNone ? cin2 : d.clone(c.node);
          const rational skip = M(e, c.at);
          set_time(d, rest, "start", P(c.start, skip));
          set_time(d, rest, "duration", M(cend, e));
          if (cin2 != kNone) {
            set_time(d, rest, "offset", P(c.offset, skip));  // E2's own time: anchored at e
          } else {
            moves.push_back({rest, e});
          }
        } else if (cin2 != kNone) {
          d.remove(cin2);
        }
        continue;
      }
      if (c.at < e) {
        // Starts inside the cut.
        if (cend <= e && !(c.music && o_.music_plays_through)) {
          d.remove(c.node);
          if (cin2 != kNone) d.remove(cin2);
          continue;
        }
        const std::uint32_t keep = cin2 != kNone ? cin2 : c.node;
        if (cin2 != kNone) d.remove(c.node);
        if (!(c.music && o_.music_plays_through)) {
          const rational skip = M(e, c.at);
          set_time(d, keep, "start", P(c.start, skip));
          set_time(d, keep, "duration", M(cend, e));
        }
        if (head) {
          // E itself carries on from e, in the same local time.
          set_time(d, keep, "offset", P(e_.start, M(e, e_.at)));
        } else if (cin2 != kNone) {
          set_time(d, keep, "offset", P(e_.start, M(e, e_.at)));
        } else {
          d.remove(keep);
          moves.push_back({keep, e});
        }
        continue;
      }
      // At or after e: goes with what follows the cut.
      if (E2 != kNone) {
        d.remove(c.node);
      } else if (whole || tail) {
        d.remove(c.node);
        moves.push_back({c.node, c.at});
      }
      // head: stays in E, its local time unchanged.
    }

    // The element itself.
    auto audio_head = [&](std::uint32_t n, rational by) {
      // A J or L cut keeps its lead and tail when the head moves.
      const auto as = time_attr(d, n, "audioStart");
      const auto ad = time_attr(d, n, "audioDuration");
      if (as) set_time(d, n, "audioStart", P(*as, by));
      if (ad) set_time(d, n, "audioDuration", M(*ad, by));
    };
    auto audio_tail = [&](std::uint32_t n, rational by) {
      const auto ad = time_attr(d, n, "audioDuration");
      if (ad) set_time(d, n, "audioDuration", M(*ad, by));
    };
    if (whole) {
      if (ripple) {
        d.remove(E);
      } else {
        // A gap where it was.
        const std::uint32_t gap = d.make_element("gap");
        d.set_attr(gap, "name", "Gap");
        d.set_attr(gap, "offset", format_time(P(t_.project().tc_start, e_.at)));
        d.set_attr(gap, "start", "3600s");
        d.set_attr(gap, "duration", format_time(M(e_.end, e_.at)));
        d.insert(t_.project().spine, gap, E);
        d.remove(E);
      }
    } else if (head) {
      const rational by = M(e, e_.at);
      set_time(d, E, "start", P(e_.start, by));
      set_time(d, E, "duration", M(M(e_.end, e_.at), by));
      audio_head(E, by);
    } else if (tail) {
      const rational by = M(e_.end, s);
      set_time(d, E, "duration", M(s, e_.at));
      audio_tail(E, by);
    } else {
      set_time(d, E, "duration", M(s, e_.at));
      audio_tail(E, M(e_.end, s));
      const rational by = M(e, e_.at);
      set_time(d, E2, "start", P(e_.start, by));
      set_time(d, E2, "duration", M(e_.end, e));
      audio_head(E2, by);
    }
    if (!ripple && !whole) {
      // The lifted part becomes a gap between the halves.
      const std::uint32_t gap = d.make_element("gap");
      d.set_attr(gap, "name", "Gap");
      d.set_attr(gap, "start", "3600s");
      d.set_attr(gap, "duration", format_time(M(e, s)));
      std::uint32_t next = kNone;
      const auto siblings = d.elements(t_.project().spine);
      for (std::size_t k = 0; k + 1 < siblings.size(); ++k) {
        if (siblings[k] == E) next = siblings[k + 1];
      }
      d.insert(t_.project().spine, gap, head ? E : next);
    }
  }
  relay();
  if (ripple) steps_.push_back({true, a, b, {}});
  // Connected clips that lost their host find the one now under them.
  for (const rehost& m : moves) {
    rational at = ripple ? (m.at < a ? m.at : (m.at < b ? a : M(m.at, M(b, a)))) : m.at;
    int h = storyline_at(at);
    if (h < 0) {
      const auto line = storyline();
      if (line.empty()) continue;
      h = line.back();
      const item& last = t_.items()[static_cast<std::size_t>(h)];
      at = M(P(last.at, last.duration), frame);
    }
    const item& host = t_.items()[static_cast<std::size_t>(h)];
    set_time(d, m.node, "offset", P(host.start, M(at, host.at)));
    if (d.at(m.node).name == "marker" || d.at(m.node).name == "chapter-marker") {
      set_time(d, m.node, "start", P(host.start, M(at, host.at)));
      d.remove_attr(m.node, "offset");
      insert_marker_node(host.node, m.node);
    } else {
      insert_anchor(host.node, m.node);
    }
  }
  t_.reindex();
  const std::string what = std::string(why) + " " + seconds_text(M(b, a)) + " at " + clock(a);
  changes_.push_back({ripple ? what : std::string(why) + " (lifted) " + seconds_text(M(b, a)) + " at " + clock(a), a,
                      ripple ? M(b, a) : rational{}, {}});
  mark(a, ripple ? what : "lifted " + seconds_text(M(b, a)));
  return true;
}

// ---- trim, split, remove, move ------------------------------------------------------------

bool editor::op_trim(std::uint32_t node, const edit_op& op, std::string* error) {
  const item* it = item_of(node);
  if (!it || !it->editable) {
    *error = it ? "\"" + it->name + "\" is not editable: " + it->not_editable : "no such item";
    return false;
  }
  if (it->kind == item_kind::transition) {
    *error = "a transition is removed, not trimmed";
    return false;
  }
  rational head{}, tail{};
  if (op.has_from || op.has_to) {
    const rational from = op.has_from ? snap(map_time(op.from)) : it->at;
    const rational to = op.has_to ? snap(map_time(op.to)) : P(it->at, it->duration);
    head = M(from, it->at);
    tail = M(P(it->at, it->duration), to);
  }
  if (op.has_head) head = snap(op.head);
  if (op.has_tail) tail = snap(op.tail);
  if (M(it->duration, P(head, tail)) <= rational{}) {
    *error = "that leaves nothing of \"" + it->name + "\"";
    return false;
  }
  // Extending into the media needs media there.
  if (head < rational{} || tail < rational{}) {
    const auto a = t_.assets().find(it->ref);
    if (it->kind == item_kind::asset_clip && a != t_.assets().end() && a->second.duration.num > 0) {
      const rational first = M(it->start, a->second.start);  // file time now
      const rational last = P(first, it->duration);
      if (head < rational{} && P(first, head) < rational{}) {
        *error = "\"" + it->name + "\" has only " + seconds_text(first) + " of media before it";
        return false;
      }
      if (tail < rational{} && M(last, tail) > a->second.duration) {
        *error = "\"" + it->name + "\" has only " + seconds_text(M(a->second.duration, last)) + " of media after it";
        return false;
      }
    } else if (it->opaque || it->kind == item_kind::audio || it->kind == item_kind::video) {
      *error = "MediaViewer does not know how much media \"" + it->name + "\" has beyond its edges";
      return false;
    }
  }
  xml::document& d = t_.doc();
  const std::string name = it->name;
  if (it->parent == -1 && it->in_storyline) {
    const rational at = it->at, end = P(it->at, it->duration), start = it->start;
    // Shortening is a ripple cut of the ends; lengthening inserts media.
    if (tail > rational{} && !op_cut(M(end, tail), end, true, error, "trimmed")) return false;
    if (head > rational{} && !op_cut(at, P(at, head), true, error, "trimmed")) return false;
    if (head < rational{} || tail < rational{}) {
      const item* now = item_of(node);
      if (!now) return false;
      const rational dur = now->duration;
      rational s = now->start;
      if (head < rational{}) s = P(s, head);
      set_time(d, node, "start", s);
      set_time(d, node, "duration", M(dur, P(rmin(head, rational{}), rmin(tail, rational{}))));
      // Anchored children keep their place in the clip's own time: nothing to move.
      relay();
      if (head < rational{}) steps_.push_back({false, at, {}, M(rational{}, head)});
      if (tail < rational{}) steps_.push_back({false, P(end, M(rational{}, head)), {}, M(rational{}, tail)});
      changes_.push_back({"lengthened \"" + name + "\" by " + seconds_text(M(rational{}, P(rmin(head, rational{}), rmin(tail, rational{})))),
                          at, {}, M(rational{}, P(rmin(head, rational{}), rmin(tail, rational{})))});
      mark(at, "lengthened");
      (void)start;
    }
    return true;
  }
  // A connected clip: its own start, length and anchor.
  const rational s = P(it->start, head);
  set_time(d, node, "start", s);
  set_time(d, node, "offset", P(it->offset, head));
  set_time(d, node, "duration", M(it->duration, P(head, tail)));
  t_.reindex();
  changes_.push_back({"trimmed connected \"" + name + "\"", item_of(node) ? item_of(node)->at : rational{}, {}, {}});
  return true;
}

bool editor::op_split(std::uint32_t node, rational at0, std::string* error) {
  const item* it = item_of(node);
  if (!it || !it->editable || it->kind == item_kind::transition || it->kind == item_kind::audition) {
    *error = it ? "\"" + it->name + "\" cannot be split here" : "no such item";
    return false;
  }
  const rational at = snap(at0);
  const rational end = P(it->at, it->duration);
  if (!(it->at < at && at < end)) {
    *error = "the split point " + clock(at) + " is not inside \"" + it->name + "\" (" + clock(it->at) + "-" +
             clock(end) + ")";
    return false;
  }
  xml::document& d = t_.doc();
  const rational by = M(at, it->at);
  const std::string name = it->name;
  const std::uint32_t parent = d.at(node).parent;
  const std::uint32_t second = d.clone(node);
  std::uint32_t next = kNone;
  const auto siblings = d.elements(parent);
  for (std::size_t k = 0; k + 1 < siblings.size(); ++k) {
    if (siblings[k] == node) next = siblings[k + 1];
  }
  d.insert(parent, second, next);
  set_time(d, node, "duration", by);
  set_time(d, second, "start", P(it->start, by));
  set_time(d, second, "duration", M(it->duration, by));
  if (const auto as = time_attr(d, second, "audioStart")) set_time(d, second, "audioStart", P(*as, by));
  if (const auto ad = time_attr(d, second, "audioDuration")) {
    set_time(d, second, "audioDuration", M(*ad, by));
    set_time(d, node, "audioDuration", M(*ad, M(it->duration, by)));
  }
  if (it->parent != -1 || !it->in_storyline) {
    set_time(d, second, "offset", P(it->offset, by));
  }
  // Anchored items and markers: those from `at` on go with the second half.
  if (!container(it->kind)) {
    const auto a = d.elements(node), b = d.elements(second);
    for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
      const std::string& n = d.at(a[k]).name;
      rational local;
      if (n == "marker" || n == "chapter-marker") {
        local = time_attr(d, a[k], "start").value_or(rational{});
      } else if (is_story(n)) {
        local = time_attr(d, a[k], "offset").value_or(rational{});
      } else {
        continue;
      }
      const rational tl = P(it->at, M(local, it->start));
      d.remove(tl < at ? b[k] : a[k]);
    }
  }
  if (it->parent == -1 && it->in_storyline) {
    relay();
  } else {
    t_.reindex();
  }
  changes_.push_back({"split \"" + name + "\" at " + clock(at), at, {}, {}});
  return true;
}

bool editor::op_remove(std::uint32_t node, bool ripple, std::string* error) {
  const item* it = item_of(node);
  if (!it) {
    *error = "no such item";
    return false;
  }
  if (!it->editable && it->kind != item_kind::storyline) {
    *error = "\"" + it->name + "\" is not editable: " + it->not_editable;
    return false;
  }
  const std::string name = it->name.empty() ? kind_name(it->kind) : it->name;
  if (it->kind == item_kind::transition || it->parent != -1 || !it->in_storyline) {
    const rational at = it->at;
    t_.doc().remove(node);
    t_.reindex();
    changes_.push_back({"removed \"" + name + "\"", at, {}, {}});
    return true;
  }
  // The storyline: exactly its own range, not snapped (its edges are FCP's).
  const rational a = it->at, b = P(it->at, it->duration);
  const edit_options keep = o_;
  const bool ok = op_cut(a, b, ripple, error, "removed \"" + name + "\",");
  o_ = keep;
  return ok;
}

bool editor::op_move(std::uint32_t node, std::uint32_t before, std::string* error) {
  const item* it = item_of(node);
  if (!it || it->parent != -1 || !it->in_storyline || it->kind == item_kind::transition) {
    *error = "only a clip in the storyline can be moved";
    return false;
  }
  if (!it->editable) {
    *error = "\"" + it->name + "\" is not editable: " + it->not_editable;
    return false;
  }
  if (before == node) return true;
  if (before != kNone) {
    const item* b = item_of(before);
    if (!b || b->parent != -1 || !b->in_storyline) {
      *error = "\"before\" must be a clip in the storyline";
      return false;
    }
  }
  xml::document& d = t_.doc();
  const std::uint32_t spine = t_.project().spine;
  const std::string name = it->name;
  // Transitions on either side of where it was and where it goes.
  auto drop_transitions_around = [&](std::uint32_t n) {
    const auto els = d.elements(spine);
    for (std::size_t k = 0; k < els.size(); ++k) {
      if (els[k] != n) continue;
      if (k > 0 && d.at(els[k - 1]).name == "transition") d.remove(els[k - 1]);
      if (k + 1 < els.size() && d.at(els[k + 1]).name == "transition") d.remove(els[k + 1]);
    }
  };
  drop_transitions_around(node);
  if (before != kNone) drop_transitions_around(before);
  d.remove(node);
  d.insert(spine, node, before);
  relay();
  const item* now = item_of(node);
  changes_.push_back({"moved \"" + name + "\" to " + clock(now ? now->at : rational{}), now ? now->at : rational{}, {}, {}});
  if (now) mark(now->at, "moved here");
  return true;
}

// ---- source, insert, connect --------------------------------------------------------------

bool editor::op_source(std::uint32_t node, const edit_op& op, const media_probe* probe, std::string* error) {
  const item* it = item_of(node);
  if (!it || !it->editable || (it->kind != item_kind::asset_clip && it->kind != item_kind::audio &&
                               it->kind != item_kind::video)) {
    *error = "only a clip of one file can play another range";
    return false;
  }
  if (it->retimed) {
    *error = "\"" + it->name + "\" is retimed: change its range in Final Cut Pro";
    return false;
  }
  xml::document& d = t_.doc();
  std::string ref = it->ref;
  if (!op.path.empty() && op.path != it->source) {
    if (!probe) {
      *error = "the file could not be read";
      return false;
    }
    ref = ensure_asset(op.path, *probe);
    it = item_of(node);
    if (!it) return false;
  }
  const auto a = t_.assets().find(ref);
  if (a == t_.assets().end()) {
    *error = "the clip's media is not described in the project";
    return false;
  }
  if (op.source_in < rational{} || (a->second.duration.num > 0 && P(op.source_in, it->duration) > a->second.duration)) {
    *error = "the file has no media for " + seconds_text(it->duration) + " from " + clock(op.source_in);
    return false;
  }
  const rational start = P(a->second.start, op.source_in);
  const rational shift = M(start, it->start);
  const std::string name = it->name;
  const rational at = it->at;
  d.set_attr(node, "ref", ref);
  if (!op.path.empty()) d.set_attr(node, "name", file_stem(op.path));
  set_time(d, node, "start", start);
  if (const auto as = time_attr(d, node, "audioStart")) set_time(d, node, "audioStart", P(*as, shift));
  // Its anchored items and markers keep their place on the timeline.
  for (const std::uint32_t c : d.elements(node)) {
    const std::string& n = d.at(c).name;
    if (n == "marker" || n == "chapter-marker") {
      if (const auto s = time_attr(d, c, "start")) set_time(d, c, "start", P(*s, shift));
    } else if (is_story(n)) {
      if (const auto o = time_attr(d, c, "offset")) set_time(d, c, "offset", P(*o, shift));
    }
  }
  t_.reindex();
  changes_.push_back({"\"" + name + "\" now plays " + clock(op.source_in) + " of " +
                          (op.path.empty() ? std::string("its file") : file_stem(op.path)),
                      at, {}, {}});
  mark(at, "other take");
  return true;
}

bool editor::op_insert(const edit_op& op, std::uint32_t before, const media_probe& probe, std::string* error) {
  const rational dur = snap(op.duration);
  if (dur <= rational{}) {
    *error = "the duration is less than a frame";
    return false;
  }
  if (op.source_in < rational{} || (probe.duration.num > 0 && P(op.source_in, dur) > probe.duration)) {
    *error = "the file has no media for " + seconds_text(dur) + " from " + clock(op.source_in);
    return false;
  }
  if (before != kNone) {
    const item* b = item_of(before);
    if (!b || b->parent != -1 || !b->in_storyline) {
      *error = "\"before\" must be a clip in the storyline";
      return false;
    }
  }
  const std::string ref = ensure_asset(op.path, probe);
  xml::document& d = t_.doc();
  const auto a = t_.assets().find(ref);
  const rational base = a != t_.assets().end() ? a->second.start : rational{};
  const std::uint32_t clip = d.make_element("asset-clip");
  d.set_attr(clip, "ref", ref);
  d.set_attr(clip, "offset", "0s");
  d.set_attr(clip, "name", file_stem(op.path));
  d.set_attr(clip, "start", format_time(P(base, op.source_in)));
  d.set_attr(clip, "duration", format_time(dur));
  const rational at = before != kNone && item_of(before) ? item_of(before)->at : t_.length();
  // A transition at the insertion point would sit on a new edit.
  const auto els = d.elements(t_.project().spine);
  for (std::size_t k = 0; k < els.size(); ++k) {
    if (els[k] == before && k > 0 && d.at(els[k - 1]).name == "transition") d.remove(els[k - 1]);
  }
  d.insert(t_.project().spine, clip, before);
  relay();
  steps_.push_back({false, at, {}, dur});
  changes_.push_back({"inserted " + seconds_text(dur) + " of " + file_stem(op.path) + " at " + clock(at), at, {}, dur});
  mark(at, "inserted");
  return true;
}

bool editor::op_connect(const edit_op& op, const media_probe& probe, std::string* error) {
  const rational at = snap(op.at);
  const rational dur = snap(op.duration);
  if (dur <= rational{}) {
    *error = "the duration is less than a frame";
    return false;
  }
  if (op.lane == 0) {
    *error = "lane 0 is the storyline: use insert, or a lane above (1, 2, …) or below (-1, …)";
    return false;
  }
  if (op.source_in < rational{} || (probe.duration.num > 0 && P(op.source_in, dur) > probe.duration)) {
    *error = "the file has no media for " + seconds_text(dur) + " from " + clock(op.source_in);
    return false;
  }
  const int h = storyline_at(at);
  if (h < 0) {
    *error = clock(at) + " is not on the timeline";
    return false;
  }
  if (t_.items()[static_cast<std::size_t>(h)].kind == item_kind::other ||
      t_.items()[static_cast<std::size_t>(h)].kind == item_kind::audition) {
    *error = "MediaViewer does not attach clips to what sits at " + clock(at);
    return false;
  }
  const std::string ref = ensure_asset(op.path, probe);
  const item& host = t_.items()[static_cast<std::size_t>(storyline_at(at))];
  xml::document& d = t_.doc();
  const auto a = t_.assets().find(ref);
  const rational base = a != t_.assets().end() ? a->second.start : rational{};
  const std::uint32_t clip = d.make_element("asset-clip");
  d.set_attr(clip, "ref", ref);
  d.set_attr(clip, "lane", std::to_string(op.lane));
  d.set_attr(clip, "offset", format_time(P(host.start, M(at, host.at))));
  d.set_attr(clip, "name", file_stem(op.path));
  d.set_attr(clip, "start", format_time(P(base, op.source_in)));
  d.set_attr(clip, "duration", format_time(dur));
  if (!op.with_audio && probe.has_audio && probe.has_video) d.set_attr(clip, "srcEnable", "video");
  insert_anchor(host.node, clip);
  t_.reindex();
  changes_.push_back({"connected " + seconds_text(dur) + " of " + file_stem(op.path) + " at " + clock(at) +
                          " (lane " + std::to_string(op.lane) + ")",
                      at, {}, {}});
  mark(at, "B-roll");
  return true;
}

// ---- markers, titles, volume, enable ------------------------------------------------------

bool editor::op_marker(rational at0, const std::string& text, bool todo, std::string* error) {
  const rational at = snap(at0);
  const int h = storyline_at(at);
  if (h < 0) {
    *error = clock(at) + " is not on the timeline";
    return false;
  }
  const item& host = t_.items()[static_cast<std::size_t>(h)];
  if (host.kind == item_kind::other || host.kind == item_kind::audition) {
    *error = "MediaViewer does not mark what sits at " + clock(at);
    return false;
  }
  xml::document& d = t_.doc();
  const std::uint32_t m = d.make_element("marker");
  d.set_attr(m, "start", format_time(P(host.start, M(at, host.at))));
  d.set_attr(m, "duration", format_time(t_.project().frame.num > 0 ? t_.project().frame : rational{1, 1000}));
  d.set_attr(m, "value", text);
  if (todo) d.set_attr(m, "completed", "0");
  insert_marker_node(host.node, m);
  t_.reindex();
  changes_.push_back({"marker \"" + text + "\" at " + clock(at), at, {}, {}});
  return true;
}

bool editor::op_unmarker(std::uint32_t node, int marker, std::string* error) {
  const item* it = item_of(node);
  if (!it || marker < 0 || static_cast<std::size_t>(marker) >= it->markers.size()) {
    *error = "no such marker on that item";
    return false;
  }
  const std::string value = it->markers[static_cast<std::size_t>(marker)].value;
  const rational at = it->markers[static_cast<std::size_t>(marker)].at;
  t_.doc().remove(it->markers[static_cast<std::size_t>(marker)].node);
  t_.reindex();
  changes_.push_back({"removed marker \"" + value + "\"", at, {}, {}});
  return true;
}

bool editor::op_title(std::uint32_t node, const std::string& text, std::string* error) {
  const item* it = item_of(node);
  if (!it || (it->kind != item_kind::title && it->kind != item_kind::caption) || !it->editable) {
    *error = "only a title's or caption's text can be changed";
    return false;
  }
  xml::document& d = t_.doc();
  std::vector<std::string> lines;
  std::size_t from = 0;
  for (;;) {
    const std::size_t nl = text.find('\n', from);
    lines.push_back(text.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
    if (nl == std::string::npos) break;
    from = nl + 1;
  }
  const auto texts = d.elements(node, "text");
  if (texts.empty()) {
    *error = "the title has no text to change";
    return false;
  }
  for (std::size_t k = 0; k < texts.size(); ++k) {
    std::string line;
    if (k < lines.size()) line = lines[k];
    if (k + 1 == texts.size()) {
      for (std::size_t j = k + 1; j < lines.size(); ++j) line += "\n" + lines[j];
    }
    const auto runs = d.elements(texts[k], "text-style");
    if (runs.empty()) {
      d.set_text(texts[k], line);
    } else {
      d.set_text(runs[0], line);
      for (std::size_t r = 1; r < runs.size(); ++r) d.remove(runs[r]);
    }
  }
  const rational at = it->at;
  t_.reindex();
  changes_.push_back({"title text now \"" + text + "\"", at, {}, {}});
  return true;
}

bool editor::op_volume(std::uint32_t node, double db, std::string* error) {
  const item* it = item_of(node);
  if (!it || !it->editable ||
      !(it->kind == item_kind::asset_clip || it->kind == item_kind::audio || it->kind == item_kind::clip ||
        it->kind == item_kind::ref_clip || it->kind == item_kind::mc_clip || it->kind == item_kind::sync_clip)) {
    *error = "that item has no volume";
    return false;
  }
  xml::document& d = t_.doc();
  char amount[32];
  std::snprintf(amount, sizeof amount, "%.1fdB", db);
  std::uint32_t vol = d.first(node, "adjust-volume");
  if (vol != kNone) {
    d.set_attr(vol, "amount", amount);
  } else {
    vol = d.make_element("adjust-volume");
    d.set_attr(vol, "amount", amount);
    std::uint32_t before = kNone;
    for (const std::uint32_t c : d.elements(node)) {
      const std::string& n = d.at(c).name;
      if (n == "adjust-panner" || is_story(n) || is_markerish(n) || after_markers(n) || n == "mc-source") {
        before = c;
        break;
      }
    }
    d.insert(node, vol, before);
  }
  const rational at = it->at;
  const std::string name = it->name;
  t_.reindex();
  changes_.push_back({"\"" + name + "\" volume " + amount, at, {}, {}});
  return true;
}

bool editor::op_enable(std::uint32_t node, bool on, std::string* error) {
  const item* it = item_of(node);
  if (!it || !it->editable) {
    *error = it ? "\"" + it->name + "\" is not editable: " + it->not_editable : "no such item";
    return false;
  }
  xml::document& d = t_.doc();
  if (on && !d.attr(node, "enabled")) return true;
  d.set_attr(node, "enabled", on ? "1" : "0");
  const rational at = it->at;
  const std::string name = it->name;
  t_.reindex();
  changes_.push_back({std::string(on ? "enabled" : "disabled") + " \"" + name + "\"", at, {}, {}});
  return true;
}

}  // namespace mv::nle
