// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/engine.h"

#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mv::enhance {
namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::optional<std::string> read_text(const std::string& path, std::size_t cap) {
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(cap + 1, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  const auto got = static_cast<std::size_t>(in.gcount());
  if (got > cap) return std::nullopt;  // not a file this add-on wrote
  text.resize(got);
  return text;
}

std::vector<std::uint8_t> head_of(const std::string& path) {
  std::vector<std::uint8_t> head(256);
  std::ifstream in(fs_path(path), std::ios::binary);
  if (!in) return {};
  in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(in.gcount()));
  return head;
}

// The preview's height in points for a picture of `aspect` (width / height)
// across a sheet `width_pt` wide: the picture fills the width, within reason.
std::uint32_t preview_height(float width_pt, double aspect) {
  if (aspect <= 0) return kPreviewHeight;
  return static_cast<std::uint32_t>(std::clamp(std::lround(width_pt / aspect), 160L, 320L));
}

std::string name_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* model_noun(model_kind k) {
  switch (k) {
    case model_kind::still: return "photo model";
    case model_kind::video: return "video model";
    case model_kind::audio: return "audio model";
  }
  return "model";
}

// "k.<kind>.<knob>" -> (kind, knob).
std::optional<std::pair<model_kind, std::string>> knob_id(const std::string& node) {
  if (node.rfind("k.", 0) != 0) return std::nullopt;
  const std::size_t dot = node.find('.', 2);
  if (dot == std::string::npos) return std::nullopt;
  const auto k = kind_from(std::string_view(node).substr(2, dot - 2));
  if (!k) return std::nullopt;
  return std::pair{*k, node.substr(dot + 1)};
}

// "install.<kind>" -> (kind, ""), "remove.<kind>.<id>" -> (kind, id), for `what`.
std::optional<std::pair<model_kind, std::string>> action_of(const std::string& node, std::string_view what) {
  if (node.size() <= what.size() + 1 || node.compare(0, what.size(), what) != 0 || node[what.size()] != '.') {
    return std::nullopt;
  }
  const std::string_view rest = std::string_view(node).substr(what.size() + 1);
  const std::size_t dot = rest.find('.');
  const auto k = kind_from(rest.substr(0, dot));
  if (!k) return std::nullopt;
  return std::pair{*k, dot == std::string_view::npos ? std::string() : std::string(rest.substr(dot + 1))};
}

// A kind's model files: model.json first, then the others by name. Their
// ids are their names; a file whose name is not an id is not a model.
std::vector<std::pair<std::string, std::string>> model_files_in(const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> out;  // (id, path)
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::u8string name = it->path().filename().u8string();
    const auto id = model_id_of(std::string(name.begin(), name.end()));
    if (id) out.emplace_back(*id, dir + "/" + *id + ".json");
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    const bool da = a.first == kDefaultModelId, db = b.first == kDefaultModelId;
    return da != db ? da : a.first < b.first;
  });
  return out;
}

}  // namespace

engine::engine(engine_port port, std::string self_dir, std::string data_dir, ai::fetch_options fetch)
    : port_(std::move(port)), self_dir_(std::move(self_dir)), data_dir_(std::move(data_dir)) {
  std::vector<pin> pins;
  if (auto manifest = read_text(self_dir_ + "/manifest.json", 4u << 20)) pins = read_pins(*manifest);
  for (const model_kind k : kModelKinds) {
    auto& list = models_[static_cast<int>(k)];
    for (const auto& [id, file] : model_files_in(self_dir_ + "/models/" + kind_name(k))) {
      if (list.size() == 8) break;  // a choice, not a catalogue
      const auto text = read_text(file, 256u << 10);
      auto s = text ? parse_spec(*text, k) : std::nullopt;
      if (!s) {
        if (port_.log) port_.log(1, "enhance: a model file did not parse; that model is not offered");
        continue;
      }
      s->id = id;
      model_entry e;
      e.files = files_for(*s, pins);
      e.key = model_key(k, id);
      e.spec = std::move(*s);
      list.push_back(std::move(e));
    }
    // model.json's stays first (the default); the others, cheapest first.
    std::stable_sort(list.begin() + (list.empty() || list.front().spec.id != kDefaultModelId ? 0 : 1), list.end(),
                     [](const model_entry& a, const model_entry& b) { return a.files.bytes < b.files.bytes; });
  }
  if (!data_dir_.empty()) prefs_ = load_prefs(data_dir_ + "/enhance.json");
  downloads::port dp;
  dp.transport = port_.transport;
  dp.free_disk = port_.free_disk;
  dp.changed = [this](const std::string&) { on_changed(); };
  dl_ = std::make_unique<downloads>(data_dir_ + "/models", std::move(dp), std::move(fetch));
}

engine::~engine() {
  if (resume_.joinable()) resume_.join();
  if (runner_) runner_->cancel();
  runner_.reset();
  dl_->stop_all();
}

void engine::set_runner(std::unique_ptr<runner> r) {
  std::lock_guard lock(m_);
  runner_ = std::move(r);
}

void engine::resume_wanted() {
  if (resume_.joinable() || data_dir_.empty()) return;
  resume_ = std::thread([this] {
    for (const auto& list : models_) {
      for (const model_entry& e : list) {
        bool want = false;
        {
          std::lock_guard lock(m_);
          want = prefs_.want(e.key);
        }
        if (want && !dl_->ready(e.files)) dl_->start(e.key, e.files);
      }
    }
  });
}

const engine::model_entry* engine::find_model(model_kind k, const std::string& id) const {
  for (const model_entry& e : models_[static_cast<int>(k)]) {
    if (e.spec.id == id) return &e;
  }
  return nullptr;
}

const engine::model_entry* engine::chosen_locked(model_kind k) const {
  const auto& list = models_[static_cast<int>(k)];
  if (list.empty()) return nullptr;
  if (const model_entry* e = find_model(k, prefs_.use_of(k))) return e;
  return &list.front();  // model.json's, or the only one there is
}

const model_spec* engine::spec(model_kind k) const {
  std::lock_guard lock(m_);
  const model_entry* e = chosen_locked(k);
  return e ? &e->spec : nullptr;
}

dl_status engine::download_status(model_kind k) const {
  const model_entry* e = nullptr;
  {
    std::lock_guard lock(m_);
    e = chosen_locked(k);
  }
  return e ? dl_->status(e->key, e->files) : dl_status{};
}

prefs engine::current_prefs() const {
  std::lock_guard lock(m_);
  return prefs_;
}

void engine::on_changed() {
  std::lock_guard lock(m_);
  post_settings_locked();
  post_sheet_locked();
}

void engine::save_locked() {
  if (!data_dir_.empty() && !save_prefs(data_dir_ + "/enhance.json", prefs_) && port_.log) {
    port_.log(1, "enhance: its settings could not be saved");
  }
}

void engine::post_settings_locked() {
  if (settings_gen_ == 0 || !port_.post) return;
  std::vector<model_card> cards;
  for (const model_kind k : kModelKinds) {
    model_card c;
    c.kind = k;
    const model_entry* chosen = chosen_locked(k);
    for (const model_entry& e : models_[static_cast<int>(k)]) {
      if (&e == chosen) c.chosen = c.models.size();
      c.models.push_back({&e.spec, e.files, dl_->status(e.key, e.files)});
    }
    cards.push_back(std::move(c));
  }
  if (!port_.post(kSettingsScreen, settings_gen_, settings_tree(cards))) settings_gen_ = 0;
}

std::vector<model_kind> engine::kinds_for_locked() const {
  switch (item_) {
    case item_kind::still: return {model_kind::still};
    case item_kind::audio: return facts_.sound ? std::vector{model_kind::audio} : std::vector<model_kind>{};
    case item_kind::media:
      if (facts_.picture && facts_.sound) {
        switch (prefs_.target) {
          case clip_target::picture: return {model_kind::video};
          case clip_target::sound: return {model_kind::audio};
          case clip_target::both: return {model_kind::video, model_kind::audio};
        }
      }
      if (facts_.picture) return {model_kind::video};
      if (facts_.sound) return {model_kind::audio};
      return {};
    default: return {};
  }
}

sheet_view engine::sheet_locked() const {
  sheet_view v;
  v.file_name = name_of(path_);
  v.item = item_;
  v.has_picture = facts_.picture;
  v.has_sound = facts_.sound;
  v.target = prefs_.target;
  if (v.item == item_kind::media && !(facts_.picture && facts_.sound)) {
    v.target = facts_.picture ? clip_target::picture : clip_target::sound;
  }
  v.running = running_;
  v.progress = progress_;
  v.eta = eta_;
  v.preview = preview_;
  v.preview_split = preview_split_;
  v.preview_height = preview_ ? posted_height_ : kPreviewHeight;
  v.status = status_;
  switch (item_) {
    case item_kind::animation: v.note = "Enhance has no model for animations yet."; break;
    case item_kind::document: v.note = "Enhance works on photos, clips and audio, not on documents."; break;
    case item_kind::unknown: v.note = "Enhance cannot tell what kind of file this is."; break;
    case item_kind::audio:
      if (!facts_.sound) v.note = "This audio file cannot be read (it may be protected).";
      break;
    case item_kind::media:
      if (!facts_.picture && !facts_.sound) v.note = "This file has no picture or sound Enhance can read.";
      break;
    case item_kind::still: break;
  }
  bool all_ready = true;
  for (const model_kind k : kinds_for_locked()) {
    sheet_section s;
    s.kind = k;
    s.values = prefs_.of(k);
    const model_entry* e = chosen_locked(k);
    const dl_status st = e ? dl_->status(e->key, e->files) : dl_status{};
    if (e) {
      s.model = e->spec.id;
      const auto& list = models_[static_cast<int>(k)];
      if (list.size() > 1) {
        for (const model_entry& m : list) s.models.push_back({m.spec.id, m.spec.name});
      }
    }
    if (e && st.state == dl_state::ready) {
      s.spec = &e->spec;
    } else if (!e) {
      s.missing = "This version of Enhance has no " + std::string(model_noun(k)) + " yet.";
    } else if (st.state == dl_state::downloading || st.state == dl_state::waiting) {
      s.missing = e->spec.name + " is downloading.";
      s.downloading = st.total_bytes ? static_cast<double>(st.done_bytes) / static_cast<double>(st.total_bytes) : -1.0;
    } else if (st.state == dl_state::refused || st.state == dl_state::gone) {
      s.missing = e->spec.name + " cannot be downloaded. An update of Enhance will name a new one.";
    } else {
      s.missing = e->spec.name + " is not downloaded yet. It runs on this computer; only the download uses the network.";
      s.install = (st.state == dl_state::stopped ? "Resume the download (" : "Download (") + size_text(e->files.bytes) + ")";
    }
    all_ready = all_ready && s.spec && runner_ && runner_->supports(k, item_);
    v.sections.push_back(std::move(s));
  }
  if (v.sections.empty()) all_ready = false;
  v.can_run = all_ready && v.note.empty() && !running_;
  if (!running_ && v.note.empty() && status_.empty() && !v.sections.empty()) {
    const bool models = std::all_of(v.sections.begin(), v.sections.end(), [](const sheet_section& s) { return s.spec; });
    if (models && !runner_) {
      // The host has no rows to read and write with (an older MediaViewer).
      v.status = "This version of MediaViewer cannot run Enhance's models. Update MediaViewer to use them.";
    } else if (models) {
      for (const sheet_section& s : v.sections) {
        if (runner_->supports(s.kind, item_)) continue;
        v.status = s.kind == model_kind::video ? "This version of Enhance cannot enhance a clip's picture yet."
                   : s.kind == model_kind::still ? "This version of Enhance cannot enhance photos yet."
                                                  : "This version of Enhance cannot run this model.";
        break;
      }
    }
  }
  return v;
}

void engine::post_sheet_locked() {
  if (sheet_gen_ == 0 || !port_.post) return;
  if (!port_.post(kSheetScreen, sheet_gen_, sheet_tree(sheet_locked()))) sheet_gen_ = 0;
}

void engine::settings_opened(std::uint64_t generation) {
  std::lock_guard lock(m_);
  settings_gen_ = generation;
  post_settings_locked();
}

void engine::sheet_opened(std::uint64_t generation, const std::string& request_path, float width_pt, float scale) {
  std::string path;
  {
    std::lock_guard lock(m_);
    path = pending_path_.empty() ? request_path : pending_path_;
    pending_path_.clear();
    if (running_ && path != path_) path = path_;  // a run keeps its item
  }
  // Read outside the lock: a slow disk delays only this sheet.
  const item_kind item = path.empty() ? item_kind::unknown : sniff(head_of(path));
  media_facts facts;
  if ((item == item_kind::media || item == item_kind::audio) && port_.probe) facts = port_.probe(path);
  std::lock_guard lock(m_);
  sheet_gen_ = generation;
  if (width_pt >= 100 && width_pt <= 2000) sheet_width_pt_ = width_pt;
  if (scale >= 1 && scale <= 4) sheet_scale_ = scale;
  if (!running_) {
    if (path != path_) status_.clear();
    preview_ = false;  // a preview belongs to the run that drew it
    preview_split_ = false;
    preview_aspect_ = 0;
    path_ = path;
    item_ = item;
    facts_ = facts;
  }
  post_sheet_locked();
}

void engine::closed(const std::string& screen) {
  bool cancel_run = false;
  {
    std::lock_guard lock(m_);
    if (screen == kSettingsScreen) settings_gen_ = 0;
    if (screen == kSheetScreen) {
      sheet_gen_ = 0;
      status_.clear();
      // Leaving the sheet is leaving the item: its job stops (#297 "Navigation
      // away cancels the job"). The runner reports the cancel as usual.
      cancel_run = running_;
    }
  }
  if (cancel_run) cancel();
}

void engine::command(const std::vector<std::string>& paths) {
  {
    std::lock_guard lock(m_);
    if (!paths.empty()) pending_path_ = paths.front();
  }
  if (port_.show) port_.show(kSheetScreen, true);
}

void engine::install(model_kind k, const std::string& id) {
  const model_entry* e = nullptr;
  {
    std::lock_guard lock(m_);
    e = id.empty() ? chosen_locked(k) : find_model(k, id);
    if (!e) return;
    prefs_.wanted.insert(e->key);
    save_locked();
  }
  dl_->start(e->key, e->files);
}

void engine::stop(model_kind k, const std::string& id) {
  const model_entry* e = nullptr;
  {
    std::lock_guard lock(m_);
    e = id.empty() ? chosen_locked(k) : find_model(k, id);
    if (!e) return;
    prefs_.wanted.erase(e->key);  // stopped by hand: not resumed at the next start
    save_locked();
  }
  dl_->stop(e->key);
  on_changed();
}

void engine::remove(model_kind k, const std::string& id) {
  const model_entry* e = nullptr;
  {
    std::lock_guard lock(m_);
    if (running_) return;  // a run may be reading the weights
    e = id.empty() ? chosen_locked(k) : find_model(k, id);
    if (!e) return;
    prefs_.wanted.erase(e->key);
    save_locked();
  }
  dl_->remove(e->key, e->files, cache_dir_for(data_dir_ + "/models/" + kind_name(k), e->spec.id));
}

void engine::choose(model_kind k, const std::string& id) {
  std::lock_guard lock(m_);
  if (running_ || !find_model(k, id)) return;  // a run keeps the model it started with
  prefs_.use_of(k) = id;
  save_locked();
  post_settings_locked();
  post_sheet_locked();
}

void engine::run() {
  job_request r;
  runner* run = nullptr;
  {
    std::lock_guard lock(m_);
    const sheet_view v = sheet_locked();
    if (!v.can_run || !runner_) return;
    r.path = path_;
    r.item = item_;
    r.target = v.target;
    for (const sheet_section& s : v.sections) {
      job_request::part p;
      p.kind = s.kind;
      p.spec = s.spec;
      p.models_dir = data_dir_ + "/models/" + kind_name(s.kind);
      for (const knob& kb : s.spec->knobs) {
        const auto it = s.values.find(kb.id);
        p.values[kb.id] = knob_value(kb, it == s.values.end() ? nullptr : &it->second);
      }
      r.parts.push_back(std::move(p));
    }
    if (std::string why = runner_->check(r); !why.empty()) {
      status_ = std::move(why);
      post_sheet_locked();
      return;
    }
    running_ = true;
    progress_ = -1;
    eta_.clear();
    preview_ = false;
    preview_split_ = false;
    preview_aspect_ = 0;
    run_started_ = std::chrono::steady_clock::now();
    status_.clear();
    post_sheet_locked();
    run = runner_.get();
  }
  job_events ev;
  ev.progress = [this](double f) {
    std::lock_guard lock(m_);
    if (!running_) return;
    // Whole percent steps, and the progress node alone: the rest of the sheet
    // stays as it is.
    if (f >= 0 && progress_ >= 0 && std::floor(f * 100) == std::floor(progress_ * 100)) return;
    progress_ = f;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started_).count();
    eta_ = time_left(f, elapsed);
    if (sheet_gen_ == 0) return;
    json::writer w;
    w.begin_array().begin_object().key("op").string("set").key("id").string("progress");
    w.key("props").begin_object().key("value").number(f < 0 ? -1.0 : std::min(1.0, f)).end_object();
    w.end_object();
    w.begin_object().key("op").string("set").key("id").string("eta");
    w.key("props").begin_object().key("text").string(eta_.empty() ? std::string("Working…") : eta_).end_object();
    w.end_object().end_array();
    if (!port_.diff || !port_.diff(kSheetScreen, sheet_gen_, w.str())) post_sheet_locked();
  };
  // A preview's pixels at the sheet's size, posted to its surface. The tree
  // goes first when the surface is new or its shape changed (pixels for a
  // surface the tree does not show are dropped).
  const auto post_preview = [this](const preview_image& shape, bool split,
                                    const std::function<std::vector<std::uint8_t>(std::uint32_t, std::uint32_t)>& draw) {
    float w_pt = 420, sc = 2;
    std::uint32_t h_pt = kPreviewHeight;
    {
      std::lock_guard lock(m_);
      if (!running_ || sheet_gen_ == 0) return;
      w_pt = sheet_width_pt_;
      sc = sheet_scale_;
      if (!shape.empty()) preview_aspect_ = static_cast<double>(shape.width) / shape.height;
      h_pt = preview_height(w_pt, preview_aspect_);
    }
    const auto ow = static_cast<std::uint32_t>(std::min(2048.0f, w_pt * sc));
    const auto oh = static_cast<std::uint32_t>(std::min(1024.0f, static_cast<float>(h_pt) * sc));
    // Composed off the lock.
    const std::vector<std::uint8_t> bgra = draw(ow, oh);
    std::lock_guard lock(m_);
    if (!running_ || sheet_gen_ == 0 || !port_.surface) return;
    if (!preview_ || preview_split_ != split || posted_height_ != h_pt) {
      preview_ = true;
      preview_split_ = split;
      posted_height_ = h_pt;
      post_sheet_locked();
    }
    (void)port_.surface(kSheetScreen, "preview", sheet_gen_, bgra, ow, oh);
  };
  ev.preview = [post_preview](preview_image before, preview_image after) {
    const bool split = !before.empty() && !after.empty();
    post_preview(before.empty() ? after : before, split, [&](std::uint32_t ow, std::uint32_t oh) {
      return compose_preview(before, after, ow, oh);
    });
  };
  ev.reveal = [post_preview](preview_image partial, reveal_front at) {
    post_preview(partial, false, [&](std::uint32_t ow, std::uint32_t oh) { return compose_reveal(partial, at, ow, oh); });
  };
  ev.done = [this](bool ok, const std::string& output, const std::string& message) {
    std::lock_guard lock(m_);
    running_ = false;
    progress_ = -1;
    status_ = ok ? "Saved as " + output + "." : message;
    post_sheet_locked();
  };
  run->start(std::move(r), std::move(ev));
}

void engine::cancel() {
  runner* r = nullptr;
  {
    std::lock_guard lock(m_);
    if (!running_ || !runner_) return;
    r = runner_.get();
  }
  r->cancel();
}

void engine::event(const std::string& screen, const std::string& node, std::uint32_t kind,
                   const std::string& value_json) {
  if (screen != kSettingsScreen && screen != kSheetScreen) return;
  if (kind == MV_SLOT_EVENT_CLICK) {
    // A model's buttons, on the page and (Download, Stop) on the sheet.
    if (const auto a = action_of(node, "install")) {
      install(a->first, a->second);
      return;
    }
    if (const auto a = action_of(node, "stop")) {
      stop(a->first, a->second);
      return;
    }
    if (const auto a = action_of(node, "remove"); a && screen == kSettingsScreen) {
      remove(a->first, a->second);
      return;
    }
  }
  if (kind == MV_SLOT_EVENT_CHANGE) {
    if (const auto a = action_of(node, "model"); a && a->second.empty()) {
      const auto v = json::parse(value_json, 4);
      if (v && v->k == json::kind::string) choose(a->first, v->s);
      return;
    }
  }
  if (screen != kSheetScreen) return;
  if (kind == MV_SLOT_EVENT_CLICK) {
    if (node == "close") {
      if (port_.show) port_.show(kSheetScreen, false);
    } else if (node == "run") {
      run();
    } else if (node == "cancel") {
      cancel();
    }
    return;
  }
  if (kind != MV_SLOT_EVENT_CHANGE) return;
  const auto v = json::parse(value_json, 4);
  if (!v) return;
  std::lock_guard lock(m_);
  if (running_) return;
  if (node == "target") {
    if (v->k != json::kind::string) return;
    for (const clip_target t : {clip_target::picture, clip_target::sound, clip_target::both}) {
      if (v->s == target_name(t)) prefs_.target = t;
    }
    save_locked();
    post_sheet_locked();  // other sections now
    return;
  }
  const auto id = knob_id(node);
  if (!id) return;
  const model_entry* e = chosen_locked(id->first);
  const model_spec* sp = e ? &e->spec : nullptr;
  const knob* kb = sp ? sp->find(id->second) : nullptr;
  if (!kb) return;
  if (auto ok = check_knob_value(*kb, *v)) {
    // Remembered for that kind of target; the host already shows the value.
    prefs_.of(id->first)[kb->id] = std::move(*ok);
    save_locked();
  }
}

}  // namespace mv::enhance
