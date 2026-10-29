// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/screens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// The key Enhance's command carries (manifest.json), as the page names it.
#if defined(__APPLE__)
#define ENHANCE_KEY_TEXT "⌘⇧R"
#else
#define ENHANCE_KEY_TEXT "Ctrl+Shift+R"
#endif

namespace mv::enhance {
namespace {

const char* card_title(model_kind k) {
  switch (k) {
    case model_kind::still: return "Photos";
    case model_kind::video: return "Video";
    case model_kind::audio: return "Audio";
  }
  return "";
}

const char* model_noun(model_kind k) {
  switch (k) {
    case model_kind::still: return "photo model";
    case model_kind::video: return "video model";
    case model_kind::audio: return "audio model";
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
void preview_node(json::writer& w, const sheet_view& v) {
  w.begin_object().key("type").string("surface").key("id").string("preview").key("surface").string("preview");
  w.key("label").string(v.preview_split ? "Preview: the original on the left, enhanced on the right"
                                        : "Preview: the photo as it is enhanced, top to bottom");
  w.key("height").integer(v.preview_height);
  w.key("glow").string(v.running ? "moving" : "still");
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

// The choice of a kind's model, on the page and the sheet.
void model_choice_node(json::writer& w, model_kind k, std::span<const knob_option> models, const std::string& value,
                       bool disabled) {
  w.begin_object().key("type").string("choice").key("id").string(id_of("model", k)).key("label").string("Model");
  w.key("options").begin_array();
  for (const knob_option& o : models) w.begin_object().key("id").string(o.id).key("text").string(o.text).end_object();
  w.end_array();
  w.key("value").string(value);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

// One of a kind's other models, when it holds files or is downloading:
// named, with its own Stop or Remove.
void other_model(json::writer& w, model_kind k, const model_choice& m) {
  const std::string id = std::string(kind_name(k)) + "." + m.spec->id;
  switch (m.st.state) {
    case dl_state::ready:
      text(w, "other." + id, "Also downloaded: " + m.spec->name + " · " + size_text(m.files.bytes), "secondary");
      button(w, "remove." + id, "Remove " + m.spec->name, "destructive");
      break;
    case dl_state::downloading:
    case dl_state::waiting:
      text(w, "other." + id, m.spec->name + " is downloading… " + of_text(m.st.done_bytes, m.st.total_bytes),
           "secondary");
      button(w, "stop." + id, "Stop");
      break;
    case dl_state::stopped:
    case dl_state::gone:
    case dl_state::failed:
      text(w, "other." + id, m.spec->name + " did not finish downloading.", "secondary");
      button(w, "remove." + id, "Remove what was downloaded", "destructive");
      break;
    default: break;
  }
}

void model_card_body(json::writer& w, const model_card& c) {
  const model_kind k = c.kind;
  if (c.models.empty()) {
    text(w, id_of("none", k), "This version of Enhance has no " + std::string(model_noun(k)) + " yet.", "secondary");
    return;
  }
  const model_choice& chosen = c.models[std::min(c.chosen, c.models.size() - 1)];
  if (c.models.size() > 1) {
    std::vector<knob_option> options;
    for (const model_choice& m : c.models) {
      const bool here = m.st.state == dl_state::ready;
      options.push_back({m.spec->id, m.spec->name + " · " + size_text(m.files.bytes) + (here ? "" : ", not downloaded")});
    }
    model_choice_node(w, k, options, chosen.spec->id, false);
  }
  if (!chosen.spec->summary.empty()) text(w, id_of("summary", k), chosen.spec->summary);
  std::string line = chosen.spec->name;
  if (!chosen.spec->credit.empty()) line += " — " + chosen.spec->credit;
  text(w, id_of("name", k), line, "secondary");
  const model_spec* spec = chosen.spec;
  const model_files& files = chosen.files;
  const dl_status& st = chosen.st;
  const std::string size = size_text(files.bytes);
  switch (st.state) {
    case dl_state::absent:
      text(w, id_of("state", k), "Not downloaded. The model is " + size + ", from Hugging Face.", "secondary");
      button(w, id_of("install", k), "Download " + spec->name + " (" + size + ")", "primary");
      break;
    case dl_state::downloading:
      progress(w, id_of("progress", k), "Downloading the " + std::string(model_noun(k)),
               st.total_bytes ? static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes) : -1);
      text(w, id_of("state", k), "Downloading… " + of_text(st.done_bytes, st.total_bytes), "secondary");
      button(w, id_of("stop", k), "Stop");
      break;
    case dl_state::waiting:
      progress(w, id_of("progress", k), "Downloading the " + std::string(model_noun(k)),
               st.total_bytes ? static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes) : -1);
      text(w, id_of("state", k), "Waiting for Hugging Face… downloads resume automatically.", "secondary");
      button(w, id_of("stop", k), "Stop");
      break;
    case dl_state::stopped:
      text(w, id_of("state", k),
           "The model did not finish downloading (" + of_text(st.done_bytes, st.total_bytes) + ").", "secondary");
      button(w, id_of("install", k), "Resume", "primary");
      button(w, id_of("remove", k), "Remove what was downloaded", "destructive");
      break;
    case dl_state::no_space:
      text(w, id_of("state", k),
           "Not enough free disk: the model needs " + size_text(st.need_bytes) + ", and " +
               size_text(st.free_bytes) + " is free.",
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
      button(w, id_of("remove", k), "Remove " + spec->name, "destructive");
      break;
  }
  for (std::size_t i = 0; i < c.models.size(); ++i) {
    if (i != c.chosen) other_model(w, k, c.models[i]);
  }
}

void knob_node(json::writer& w, model_kind k, const knob& kb, const json::value& v) {
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
      for (const knob_option& o : kb.options) w.begin_object().key("id").string(o.id).key("text").string(o.text).end_object();
      w.end_array();
      w.key("value").string(v.k == json::kind::string ? v.s : kb.def_choice);
      break;
    case knob::type::toggle:
      w.key("type").string("toggle").key("value").boolean(v.k == json::kind::boolean ? v.b : kb.def_on);
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
  }
  return "";
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

std::string settings_tree(std::span<const model_card> cards) {
  json::writer w;
  begin_stack(w, "root", "vertical", 12);
  text(w, "intro",
       "Enhance writes a restored copy of the photo, clip or audio file on screen. The original is never changed. "
       "Each kind has its own model, downloaded only when you ask for it. Where a kind has more than one, choose "
       "between a faster model and a slower one with more detail. Open Enhance from a file's right-click menu, the "
       "command palette, or " ENHANCE_KEY_TEXT ".");
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

std::string sheet_tree(const sheet_view& v) {
  json::writer w;
  // A scroll at the root: a tall sheet (two models' sections, the preview)
  // scrolls rather than cutting off its buttons. The host's own header
  // carries the title and Done.
  w.begin_object().key("type").string("scroll").key("id").string("root").key("axis").string("vertical");
  w.key("children").begin_array();
  if (!v.file_name.empty()) text(w, "file", v.file_name, "secondary");
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
  for (const sheet_section& s : v.sections) {
    begin_stack(w, id_of("sec", s.kind), "vertical", 8, "card");
    text(w, id_of("head", s.kind), section_title(v, s.kind), "heading");
    if (!s.models.empty()) model_choice_node(w, s.kind, s.models, s.model, v.running);
    if (s.spec) {
      std::size_t shown = 0;
      for (const knob& kb : s.spec->knobs) {
        if (std::find(kb.kinds.begin(), kb.kinds.end(), s.kind) == kb.kinds.end()) continue;
        const auto it = s.values.find(kb.id);
        knob_node(w, s.kind, kb, knob_value(kb, it == s.values.end() ? nullptr : &it->second));
        ++shown;
      }
      if (shown == 0) text(w, id_of("plain", s.kind), s.spec->name + " has nothing to set.", "secondary");
    } else {
      text(w, id_of("missing", s.kind), s.missing, "secondary");
      if (s.downloading) {
        progress(w, id_of("progress", s.kind), "Downloading", *s.downloading);
        button(w, id_of("stop", s.kind), "Stop");
      } else if (!s.install.empty()) {
        button(w, id_of("install", s.kind), s.install, "primary");
      }
    }
    end_stack(w);
  }
  if (!v.note.empty()) text(w, "note", v.note, "secondary");
  if (v.preview) {
    preview_node(w, v);
    // Said under it, not marked on it: a label over a light photo cannot be read.
    if (v.preview_split) text(w, "sides", "Before on the left, enhanced on the right.", "caption");
  }
  if (v.running) {
    progress(w, "progress", "Enhancing", v.progress, "glow");
    text(w, "eta", v.eta.empty() ? std::string("Working…") : v.eta, "secondary");
  }
  if (!v.status.empty()) text(w, "status", v.status, "secondary");
  begin_stack(w, "buttons", "horizontal", 8);
  if (v.running) {
    button(w, "cancel", "Cancel", "destructive");
  } else {
    button(w, "run", "Enhance", "primary", !v.can_run);
  }
  end_stack(w);
  text(w, "where", "A new file is written beside the original, which is never changed.", "caption");
  w.end_array().end_object();
  return w.take();
}

}  // namespace mv::enhance
