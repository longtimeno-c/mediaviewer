// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/screens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mv::enhance {
namespace {

const char* card_title(model_kind k) {
  switch (k) {
    case model_kind::still: return "Photos";
    case model_kind::video: return "Video";
    case model_kind::audio: return "Audio";
    case model_kind::restyle: return "Restyle (photos)";
    case model_kind::remove: return "Remove (photos)";
  }
  return "";
}

const char* model_noun(model_kind k) {
  switch (k) {
    case model_kind::still: return "photo model";
    case model_kind::video: return "video model";
    case model_kind::audio: return "audio model";
    case model_kind::restyle: return "restyle model";
    case model_kind::remove: return "remove model";
  }
  return "model";
}

std::string id_of(const char* what, model_kind k) { return std::string(what) + "." + kind_name(k); }

void text(json::writer& w, const std::string& id, const std::string& t, const char* style = "body") {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(t);
  if (std::string_view(style) != "body") w.key("style").string(style);
  w.end_object();
}

void button(json::writer& w, const std::string& id, const std::string& label, const char* style = "plain",
            bool disabled = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  if (std::string_view(style) != "plain") w.key("style").string(style);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

void progress(json::writer& w, const std::string& id, const std::string& label, double value,
              const char* look = nullptr) {
  w.begin_object().key("type").string("progress").key("id").string(id).key("label").string(label);
  w.key("value").number(value < 0 ? -1.0 : std::min(1.0, value));
  if (look) w.key("look").string(look);
  w.end_object();
}

// The live preview (contribution API 10): the latest frame, the original left
// of the split and the enhanced picture right of it, in a surface the host
// draws a slow glow round while the run goes.
void preview_node(json::writer& w, bool moving) {
  w.begin_object().key("type").string("surface").key("id").string("preview").key("surface").string("preview");
  w.key("label").string("Preview: the original on the left, enhanced on the right");
  w.key("height").integer(kPreviewHeight);
  w.key("glow").string(moving ? "moving" : "still");
  w.key("marks").begin_array();
  w.begin_object().key("x").number(0.12).key("y").number(0.9).key("text").string("Before").end_object();
  w.begin_object().key("x").number(0.86).key("y").number(0.9).key("text").string("Enhanced").end_object();
  w.end_array();
  w.end_object();
}

// Opens a stack; the caller adds children and closes it with end_stack.
void begin_stack(json::writer& w, const std::string& id, const char* axis = "vertical", int spacing = 8,
                 const char* look = nullptr) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string(axis);
  w.key("spacing").integer(spacing);
  if (look) w.key("look").string(look);
  w.key("children").begin_array();
}
void end_stack(json::writer& w) { w.end_array().end_object(); }

std::string of_text(std::uint64_t done, std::uint64_t total) {
  return size_text(done) + " of " + size_text(total);
}

void model_card_body(json::writer& w, const model_card& c) {
  const model_kind k = c.kind;
  if (!c.spec) {
    text(w, id_of("none", k), "This version of Enhance has no " + std::string(model_noun(k)) + " yet.", "secondary");
    return;
  }
  std::string line = c.spec->name;
  if (!c.spec->credit.empty()) line += " — " + c.spec->credit;
  text(w, id_of("name", k), line, "secondary");
  const std::string size = size_text(c.files.bytes);
  switch (c.st.state) {
    case dl_state::absent:
      text(w, id_of("state", k), "Not installed. The model is " + size + ", downloaded from Hugging Face.",
           "secondary");
      button(w, id_of("install", k), "Download the " + std::string(model_noun(k)) + " (" + size + ")", "primary");
      break;
    case dl_state::downloading:
      progress(w, id_of("progress", k), "Downloading the " + std::string(model_noun(k)),
               c.st.total_bytes ? static_cast<double>(c.st.done_bytes) / static_cast<double>(c.st.total_bytes) : -1);
      text(w, id_of("state", k), "Downloading… " + of_text(c.st.done_bytes, c.st.total_bytes), "secondary");
      button(w, id_of("stop", k), "Stop");
      break;
    case dl_state::waiting:
      progress(w, id_of("progress", k), "Downloading the " + std::string(model_noun(k)),
               c.st.total_bytes ? static_cast<double>(c.st.done_bytes) / static_cast<double>(c.st.total_bytes) : -1);
      text(w, id_of("state", k), "Waiting for Hugging Face… downloads resume automatically.", "secondary");
      button(w, id_of("stop", k), "Stop");
      break;
    case dl_state::stopped:
      text(w, id_of("state", k),
           "The model did not finish downloading (" + of_text(c.st.done_bytes, c.st.total_bytes) + ").", "secondary");
      button(w, id_of("install", k), "Resume", "primary");
      button(w, id_of("remove", k), "Remove what was downloaded", "destructive");
      break;
    case dl_state::no_space:
      text(w, id_of("state", k),
           "Not enough free disk: the model needs " + size_text(c.st.need_bytes) + ", and " +
               size_text(c.st.free_bytes) + " is free.",
           "secondary");
      button(w, id_of("install", k), "Try again");
      break;
    case dl_state::gone:
      text(w, id_of("state", k),
           "Hugging Face no longer has this version of the model. An update of Enhance will name a new one.",
           "secondary");
      button(w, id_of("remove", k), "Remove what was downloaded", "destructive");
      break;
    case dl_state::failed:
      text(w, id_of("state", k), "The download never matched the model's checksum, so it was not kept.", "secondary");
      button(w, id_of("install", k), "Try again");
      break;
    case dl_state::refused:
      text(w, id_of("state", k), "This model cannot be installed: its download is not one Enhance accepts.",
           "secondary");
      break;
    case dl_state::ready:
      text(w, id_of("state", k), "Installed · " + size, "secondary");
      button(w, id_of("remove", k), "Remove the " + std::string(model_noun(k)), "destructive");
      break;
  }
}

// Restyle's quality is bounded by the performance level: an option above it is
// shown, disabled, with the level it needs (owner 2026-11-04).
bool level_gated(model_kind k, const knob& kb) { return k == model_kind::restyle && kb.id == "quality"; }

void knob_node(json::writer& w, model_kind k, const knob& kb, const json::value& v, tier level) {
  const std::string id = "k." + std::string(kind_name(k)) + "." + kb.id;
  w.begin_object().key("id").string(id).key("label").string(kb.label);
  if (!kb.help.empty()) w.key("help").string(kb.help);
  switch (kb.t) {
    case knob::type::slider:
      w.key("type").string("slider").key("min").number(kb.min).key("max").number(kb.max);
      if (kb.step > 0) w.key("step").number(kb.step);
      w.key("value").number(v.k == json::kind::number ? (v.is_integer ? static_cast<double>(v.i) : v.d) : kb.def);
      break;
    case knob::type::choice:
      w.key("type").string("choice").key("options").begin_array();
      for (const knob_option& o : kb.options) {
        const int rank = quality_rank(o.id);
        if (level_gated(k, kb) && rank > policy_of(level).max_quality) {
          w.begin_object().key("id").string(o.id);
          w.key("text").string(o.text + " (needs " + level_words(tier_for_quality(rank)) + ")");
          w.key("disabled").boolean(true).end_object();
        } else {
          w.begin_object().key("id").string(o.id).key("text").string(o.text).end_object();
        }
      }
      w.end_array();
      w.key("value").string(v.k == json::kind::string ? v.s : kb.def_choice);
      break;
    case knob::type::toggle:
      w.key("type").string("toggle").key("value").boolean(v.k == json::kind::boolean ? v.b : kb.def_on);
      break;
    case knob::type::text:
      w.key("type").string("text_field").key("value").string(v.k == json::kind::string ? v.s : kb.def_text);
      if (!kb.placeholder.empty()) w.key("placeholder").string(kb.placeholder);
      break;
  }
  w.end_object();
}

const char* section_title(const sheet_view& v, model_kind k) {
  if (v.item == item_kind::media) return k == model_kind::audio ? "Sound" : "Picture";
  switch (k) {
    case model_kind::still: return "Photo";
    case model_kind::video: return "Picture";
    case model_kind::audio: return "Sound";
    case model_kind::restyle: return "Restyle";
    case model_kind::remove: return v.photo == photo_target::extend ? "Extend the picture" : "Remove what you brushed";
  }
  return "";
}

// Each model's card of knobs (the sheet's and the docked panel's).
void section_cards(json::writer& w, const sheet_view& v) {
  for (const sheet_section& s : v.sections) {
    begin_stack(w, id_of("sec", s.kind), "vertical", 8, "card");
    text(w, id_of("head", s.kind), section_title(v, s.kind), "heading");
    if (s.spec) {
      std::size_t shown = 0;
      for (const knob& kb : s.spec->knobs) {
        if (std::find(kb.kinds.begin(), kb.kinds.end(), s.kind) == kb.kinds.end()) continue;
        // Remove's "extend" choice is for extending only.
        if (kb.id == "extend" && v.photo != photo_target::extend) continue;
        const auto it = s.values.find(kb.id);
        knob_node(w, s.kind, kb, knob_value(kb, it == s.values.end() ? nullptr : &it->second), v.level);
        ++shown;
      }
      if (shown == 0) text(w, id_of("plain", s.kind), s.spec->name + " has nothing to set.", "secondary");
    } else {
      text(w, id_of("missing", s.kind), s.missing, "secondary");
    }
    end_stack(w);
  }
}

// The note, the preview, and a run's glowing progress and time left.
void run_nodes(json::writer& w, const sheet_view& v) {
  if (!v.note.empty()) text(w, "note", v.note, "secondary");
  if (v.preview) preview_node(w, v.running);
  if (v.running) {
    progress(w, "progress", "Enhancing", v.progress, "glow");
    text(w, "eta", v.eta.empty() ? std::string("Working…") : v.eta, "secondary");
  }
}

bool library_result(const std::string& path) { return path.rfind("photos:", 0) == 0; }

#if defined(_WIN32)
constexpr const char* kBin = "Recycle Bin";
#else
constexpr const char* kBin = "Trash";
#endif

// S5: what the run wrote, with Show, Keep editing and Discard. The thumbnail
// carries the new file's path for the host's Show (hidden for a library
// photo, whose key the viewer's thumbnails do not read).
void result_row(json::writer& w, const sheet_view& v) {
  const bool library = library_result(v.result_path);
  begin_stack(w, "result", "vertical", 10, "card");
  begin_stack(w, "result.head", "horizontal", 10);
  w.begin_object().key("type").string("thumbnail").key("id").string("result.thumb");
  w.key("path").string(v.result_path).key("size").integer(56);
  w.key("label").string(library ? std::string("The new photo") : v.result_name);
  if (library) w.key("hidden").boolean(true);
  w.end_object();
  begin_stack(w, "result.words", "vertical", 2);
  text(w, "result.title", library ? std::string("Added to your Photos library") : "Saved as " + v.result_name);
  text(w, "result.where",
       library ? std::string("A new photo beside this one. The original is not changed.")
               : std::string("A new file beside the original. The original is not changed."),
       "caption");
  end_stack(w);
  end_stack(w);
  if (!v.result_note.empty()) text(w, "result.note", v.result_note, "secondary");
  begin_stack(w, "result.buttons", "horizontal", 8);
  button(w, "result.show", "Show", "primary");
  button(w, "result.keep", "Keep editing");
  if (!library) button(w, "result.discard", std::string("Move to ") + kBin, "destructive");
  end_stack(w);
  end_stack(w);
}

const char* run_label(photo_target t) {
  switch (t) {
    case photo_target::remove: return "Remove";
    case photo_target::restyle: return "Restyle";
    case photo_target::extend: return "Extend";
    case photo_target::detail: break;
  }
  return "Enhance";
}

}  // namespace

std::string size_text(std::uint64_t bytes) {
  char buf[32];
  if (bytes >= 1'000'000'000ull) {
    std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1e9);
  } else {
    const auto mb = static_cast<unsigned long long>(std::llround(static_cast<double>(bytes) / 1e6));
    std::snprintf(buf, sizeof buf, "%llu MB", mb == 0 && bytes > 0 ? 1ull : mb);
  }
  return buf;
}

std::string time_left(double fraction, double elapsed_s) {
  if (fraction < 0.02 || fraction >= 1 || elapsed_s < 3) return {};
  const double left = elapsed_s * (1 - fraction) / fraction;
  if (left < 60) return "Less than a minute left";
  const auto minutes = static_cast<long long>(std::lround(left / 60));
  if (minutes < 90) return "About " + std::to_string(minutes) + (minutes == 1 ? " minute left" : " minutes left");
  const double hours = left / 3600;
  char buf[48];
  std::snprintf(buf, sizeof buf, "About %.1f hours left", hours);
  return buf;
}

std::vector<clip_target> targets_for(bool has_picture, bool has_sound) {
  std::vector<clip_target> out;
  if (has_picture) out.push_back(clip_target::picture);
  if (has_sound) out.push_back(clip_target::sound);
  if (has_picture && has_sound) out.push_back(clip_target::both);
  return out;
}

std::string settings_tree(std::span<const model_card> cards, const tier_view& tiers) {
  json::writer w;
  begin_stack(w, "root", "vertical", 12);
  text(w, "intro",
       "Enhance writes a restored copy of the photo, clip or audio file on screen. The original is never changed. "
       "Each kind has its own model, downloaded only when you ask for it.");
  // The performance level, as Local search's (owner 2026-11-04).
  begin_stack(w, "tiercard", "vertical", 8, "card");
  text(w, "tiertitle", "Performance", "heading");
  w.begin_object().key("type").string("choice").key("id").string("tier").key("label").string("Level");
  w.key("options").begin_array();
  w.begin_object().key("id").string("auto").key("text")
      .string(std::string("Automatic (") + tier_words(tiers.recommended) + " on this computer)").end_object();
  for (const tier t : {tier::potato, tier::intel, tier::standard, tier::maxxed}) {
    w.begin_object().key("id").string(tier_name(t)).key("text").string(tier_words(t)).end_object();
  }
  w.end_array();
  w.key("value").string(tiers.chosen);
  w.end_object();
  // The level in effect, as the Edit pane's Enhance tab names it in its chip.
  begin_stack(w, "tiernow", "horizontal", 4);
  text(w, "tierinuse", "In use:", "caption");
  text(w, "tierlevel", level_words(tiers.effective()), "caption");
  end_stack(w);
  text(w, "tierhelp",
       "Potato and Intel use fewer cores and fewer steps, and keep Restyle at Standard quality. Default allows High; "
       "Maxxed uses every core, the most steps and allows Best. Restyle starts at the best quality the level allows.",
       "caption");
  end_stack(w);
  for (const model_card& c : cards) {
    begin_stack(w, id_of("card", c.kind), "vertical", 8, "card");
    text(w, id_of("title", c.kind), card_title(c.kind), "heading");
    model_card_body(w, c);
    end_stack(w);
  }
  text(w, "privacy",
       "Downloading a model: Hugging Face sees this computer's network address. Nothing about your files is sent, and "
       "Enhance runs on this computer.",
       "caption");
  end_stack(w);
  return w.take();
}

void level_quality(std::map<std::string, json::value>& values, const model_spec& spec, tier level) {
  if (spec.kind != model_kind::restyle) return;
  const knob* kb = spec.find("quality");
  if (!kb || kb->t != knob::type::choice) return;
  const int top = policy_of(level).max_quality;
  if (const auto it = values.find(kb->id); it != values.end()) {
    if (const auto ok = check_knob_value(*kb, it->second); ok && quality_rank(ok->s) <= top) return;
  }
  const knob_option* pick = nullptr;
  for (const knob_option& o : kb->options) {
    const int rank = quality_rank(o.id);
    if (rank <= top && (!pick || rank > quality_rank(pick->id))) pick = &o;
  }
  if (!pick) return;
  json::value v;
  v.k = json::kind::string;
  v.s = pick->id;
  values[kb->id] = std::move(v);
}

std::string edit_text(std::span<const std::int64_t> keep_ns) {
  const std::size_t pieces = keep_ns.size() / 2;
  if (pieces == 0) return {};
  std::int64_t ns = 0;
  for (std::size_t i = 0; i + 1 < keep_ns.size(); i += 2) ns += keep_ns[i + 1] - keep_ns[i];
  const std::int64_t s = (ns + 500'000'000) / 1'000'000'000;
  std::string len = std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
  if (s >= 3600) {
    const std::int64_t m = s / 60 % 60;
    len = std::to_string(s / 3600) + ":" + (m < 10 ? "0" : "") + std::to_string(m) + ":" +
          (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
  }
  return "The edit from the Video Editor: " + std::to_string(pieces) + (pieces == 1 ? " piece, " : " pieces, ") +
         len + " in all. The new clip is the edit, enhanced; the original is not changed.";
}

std::string sheet_tree(const sheet_view& v) {
  json::writer w;
  begin_stack(w, "root", "vertical", 12);
  text(w, "title", "Enhance", "heading");
  if (!v.file_name.empty()) text(w, "file", v.file_name, "secondary");
  if (!v.edit.empty()) text(w, "edit", v.edit, "secondary");
  if (v.item == item_kind::media) {
    const auto targets = targets_for(v.has_picture, v.has_sound);
    if (targets.size() > 1) {
      w.begin_object().key("type").string("choice").key("id").string("target").key("label").string("Enhance");
      w.key("options").begin_array();
      for (const clip_target t : targets) {
        const char* text_of = t == clip_target::picture ? "The picture" : t == clip_target::sound ? "The sound" : "Both";
        w.begin_object().key("id").string(target_name(t)).key("text").string(text_of).end_object();
      }
      w.end_array();
      w.key("value").string(target_name(v.target));
      if (v.running) w.key("disabled").boolean(true);
      w.end_object();
    }
  }
  if (v.item == item_kind::still && (v.offers_restyle || v.offers_remove || v.offers_extend)) {
    w.begin_object().key("type").string("choice").key("id").string("photo").key("label").string("Enhance");
    w.key("options").begin_array();
    w.begin_object().key("id").string(photo_target_name(photo_target::detail)).key("text").string("More detail").end_object();
    if (v.offers_restyle) {
      w.begin_object().key("id").string(photo_target_name(photo_target::restyle)).key("text").string("Restyle with words").end_object();
    }
    if (v.offers_remove) {
      w.begin_object().key("id").string(photo_target_name(photo_target::remove)).key("text").string("Remove what you brushed").end_object();
    }
    if (v.offers_extend) {
      w.begin_object().key("id").string(photo_target_name(photo_target::extend)).key("text").string("Extend the picture").end_object();
    }
    w.end_array();
    w.key("value").string(photo_target_name(v.photo));
    if (v.running) w.key("disabled").boolean(true);
    w.end_object();
  }
  section_cards(w, v);
  run_nodes(w, v);
  if (!v.status.empty()) text(w, "status", v.status, "secondary");
  text(w, "where", "Enhance writes a new file beside the original. The original is not changed.", "caption");
  begin_stack(w, "buttons", "horizontal", 8);
  if (v.running) {
    button(w, "cancel", "Cancel", "destructive");
  } else {
    button(w, "close", "Close");
    button(w, "run", "Enhance", "primary", !v.can_run);
  }
  end_stack(w);
  end_stack(w);
  return w.take();
}

std::string tool_tree(const sheet_view& v) {
  json::writer w;
  begin_stack(w, std::string("tool.") + photo_target_name(v.photo), "vertical", 12);
  section_cards(w, v);
  run_nodes(w, v);
  const bool result = !v.running && !v.result_path.empty();
  if (result) {
    result_row(w, v);
  } else if (!v.status.empty()) {
    text(w, "status", v.status, "secondary");
  }
  // Remove runs from the card above it (its Remove, or Return in the brush),
  // which hands the strokes on screen over first: the panel adds no second
  // Remove beside it, before or after a result.
  const bool own_run = v.photo != photo_target::remove;
  if (v.running || own_run) {
    begin_stack(w, "buttons", "horizontal", 8);
    if (v.running) {
      button(w, "cancel", "Cancel", "destructive");
    } else if (result) {
      // Another go with other values: a second new file, the first one kept.
      button(w, "run", std::string(run_label(v.photo)) + " again", "plain", !v.can_run);
    } else {
      button(w, "run", run_label(v.photo), "primary", !v.can_run);
    }
    end_stack(w);
  }
  if (!result && !v.running) {
    text(w, "where",
         v.library ? std::string("Adds a new photo to your Photos library. The original is not changed.")
                   : std::string("Writes a new file beside the original. The original is not changed."),
         "caption");
  }
  end_stack(w);
  return w.take();
}

}  // namespace mv::enhance
