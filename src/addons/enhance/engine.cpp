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

// "install.<kind>" -> kind, for `what`.
std::optional<model_kind> action_kind(const std::string& node, std::string_view what) {
  if (node.size() <= what.size() + 1 || node.compare(0, what.size(), what) != 0 || node[what.size()] != '.') {
    return std::nullopt;
  }
  return kind_from(std::string_view(node).substr(what.size() + 1));
}

}  // namespace

engine::engine(engine_port port, std::string self_dir, std::string data_dir, ai::fetch_options fetch)
    : port_(std::move(port)), self_dir_(std::move(self_dir)), data_dir_(std::move(data_dir)) {
  std::vector<pin> pins;
  if (auto manifest = read_text(self_dir_ + "/manifest.json", 4u << 20)) pins = read_pins(*manifest);
  for (const model_kind k : kModelKinds) {
    const auto text = read_text(self_dir_ + "/models/" + kind_name(k) + "/model.json", 256u << 10);
    if (!text) continue;
    auto s = parse_spec(*text, k);
    if (!s) {
      if (port_.log) port_.log(1, "enhance: a model.json did not parse; that kind is not offered");
      continue;
    }
    files_[static_cast<int>(k)] = files_for(*s, pins);
    specs_[static_cast<int>(k)] = std::move(*s);
  }
  if (!data_dir_.empty()) prefs_ = load_prefs(data_dir_ + "/enhance.json");
  downloads::port dp;
  dp.transport = port_.transport;
  dp.free_disk = port_.free_disk;
  dp.changed = [this](model_kind k) { on_changed(k); };
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
    for (const model_kind k : kModelKinds) {
      bool want = false;
      {
        std::lock_guard lock(m_);
        want = prefs_.want(k) && specs_[static_cast<int>(k)].has_value();
      }
      if (want && !dl_->ready(files_[static_cast<int>(k)])) dl_->start(k, files_[static_cast<int>(k)]);
    }
  });
}

const model_spec* engine::spec(model_kind k) const {
  const auto& s = specs_[static_cast<int>(k)];
  return s ? &*s : nullptr;
}

dl_status engine::download_status(model_kind k) const { return dl_->status(k, files_[static_cast<int>(k)]); }

prefs engine::current_prefs() const {
  std::lock_guard lock(m_);
  return prefs_;
}

void engine::on_changed(model_kind) {
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
    c.spec = spec(k);
    c.files = files_[static_cast<int>(k)];
    if (c.spec) c.st = dl_->status(k, c.files);
    // A kind this build has no model for is named only when it is one the
    // issue promises (photos): video and audio always are.
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
    const model_spec* sp = spec(k);
    const dl_status st = sp ? dl_->status(k, files_[static_cast<int>(k)]) : dl_status{};
    if (sp && st.state == dl_state::ready) {
      s.spec = sp;
    } else if (!sp) {
      s.missing = "This version of Enhance has no " + std::string(model_noun(k)) + " yet.";
    } else if (st.state == dl_state::downloading || st.state == dl_state::waiting) {
      s.missing = "The " + std::string(model_noun(k)) + " is still downloading.";
    } else {
      s.missing = "The " + std::string(model_noun(k)) + " is not installed. Download it in Settings → Add-ons → Enhance.";
    }
    all_ready = all_ready && s.spec && runner_ && runner_->supports(k, item_);
    v.sections.push_back(std::move(s));
  }
  if (v.sections.empty()) all_ready = false;
  v.can_run = all_ready && v.note.empty() && !running_;
  if (!running_ && v.note.empty() && status_.empty() && !v.sections.empty()) {
    const bool models = std::all_of(v.sections.begin(), v.sections.end(), [](const sheet_section& s) { return s.spec; });
    if (models && !runner_) {
      v.status = "This build of Enhance cannot run its models yet.";
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

void engine::install(model_kind k) {
  if (!spec(k)) return;
  {
    std::lock_guard lock(m_);
    prefs_.want(k) = true;
    save_locked();
  }
  dl_->start(k, files_[static_cast<int>(k)]);
}

void engine::stop(model_kind k) {
  {
    std::lock_guard lock(m_);
    prefs_.want(k) = false;  // stopped by hand: not resumed at the next start
    save_locked();
  }
  dl_->stop(k);
  on_changed(k);
}

void engine::remove(model_kind k) {
  {
    std::lock_guard lock(m_);
    if (running_) return;  // a run may be reading the weights
    prefs_.want(k) = false;
    save_locked();
  }
  dl_->remove(k, files_[static_cast<int>(k)]);
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
  ev.preview = [this](preview_image before, preview_image after) {
    // Composed off the lock: a letterbox at the sheet's pixel size.
    float w_pt = 420, sc = 2;
    {
      std::lock_guard lock(m_);
      if (!running_ || sheet_gen_ == 0) return;
      w_pt = sheet_width_pt_;
      sc = sheet_scale_;
    }
    const auto ow = static_cast<std::uint32_t>(std::min(2048.0f, w_pt * sc));
    const auto oh = static_cast<std::uint32_t>(std::min(1024.0f, static_cast<float>(kPreviewHeight) * sc));
    const std::vector<std::uint8_t> bgra = compose_preview(before, after, ow, oh);
    std::lock_guard lock(m_);
    if (!running_ || sheet_gen_ == 0 || !port_.surface) return;
    if (!preview_) {
      // The node first: pixels for a surface the tree does not show are dropped.
      preview_ = true;
      post_sheet_locked();
    }
    (void)port_.surface(kSheetScreen, "preview", sheet_gen_, bgra, ow, oh);
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
  if (screen == kSettingsScreen) {
    if (kind != MV_SLOT_EVENT_CLICK) return;
    if (const auto k = action_kind(node, "install")) {
      install(*k);
    } else if (const auto k2 = action_kind(node, "stop")) {
      stop(*k2);
    } else if (const auto k3 = action_kind(node, "remove")) {
      remove(*k3);
    }
    return;
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
  const model_spec* sp = spec(id->first);
  const knob* kb = sp ? sp->find(id->second) : nullptr;
  if (!kb) return;
  if (auto ok = check_knob_value(*kb, *v)) {
    // Remembered for that kind of target; the host already shows the value.
    prefs_.of(id->first)[kb->id] = std::move(*ok);
    save_locked();
  }
}

}  // namespace mv::enhance
